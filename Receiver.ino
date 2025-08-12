#include <SPI.h>
#include <RF24.h>
#include <DFRobotDFPlayerMini.h>

RF24 radio(9, 10);  // CE, CSN
static const byte thisDeviceAddr[6] = "DEV02";

// LED pins
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

// Time management
unsigned long timeOffset = 0;     // masterTime - micros()
unsigned long eventMicros[4];     // trigger times
int currentStep = 0;
int totalSteps  = 0;

// ---- DFPlayer ----
DFRobotDFPlayerMini dfplayer;
const uint16_t BOOT_GUARD_MS    = 1800; // wait out Arduino bootloader chatter on TX0
const uint8_t  PLAY_VOL         = 30;   // runtime volume (0..30)
const uint8_t  START_VOL        = 5;    // startup test volume
const uint16_t STARTUP_SOUND_MS = 0;

// Dedupe state for ST2
uint16_t lastSeq = 0;
unsigned long lastMasterStart = 0;

// Fwds
void allLedsOff();
void flashLights();
void parseAndScheduleSequence(const char* data);     // legacy
void parseAndScheduleSequenceV2(const char* data);   // ST2
void runScheduledSteps();
void executeStep(int step);
static inline void preloadAckReady(uint16_t seq);

void setup() {
  // LEDs
  pinMode(RED_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  allLedsOff();

  // ---- Protect DFPlayer from boot chatter on pins 0/1 ----
  Serial.begin(9600);
  delay(BOOT_GUARD_MS);
  while (Serial.available()) { Serial.read(); }

  // Init DFPlayer muted
  if (dfplayer.begin(Serial)) {
    dfplayer.volume(0);
    dfplayer.stop();
    // Optional startup tone
    dfplayer.volume(START_VOL);
    if (STARTUP_SOUND_MS > 0) {
      dfplayer.play(4);
      delay(STARTUP_SOUND_MS);
      dfplayer.stop();
      dfplayer.volume(0);
    }
  }
  // NOTE: During sketch upload, disconnect DFPlayer RX from Arduino TX0.

  // Radio
  radio.begin();
  radio.setPALevel(RF24_PA_MAX);       // use RF24_PA_LOW if very close to TX
  radio.setAutoAck(true);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(108);
  radio.setCRCLength(RF24_CRC_16);
  radio.enableDynamicPayloads();
  radio.enableAckPayload();

  radio.openReadingPipe(0, thisDeviceAddr);
  radio.startListening();

  // Short self-test flash
  for (int i = 0; i < 3; i++) {
    digitalWrite(RED_LED, HIGH);
    digitalWrite(ORANGE_LED, HIGH);
    digitalWrite(GREEN_LED, HIGH);
    delay(250);
    allLedsOff();
    delay(250);
  }
}

void loop() {
  if (radio.available()) {
    char msg[32] = {0};
    radio.read(&msg, sizeof(msg));

    if (strcmp(msg, "CHECK") == 0) {
      // (Auto-ACK handles the ack bit itself)
      // Could preload a small status payload if desired
    }
    else if (strncmp(msg, "SYNC:", 5) == 0) {
      unsigned long masterMicros;
      if (sscanf(msg + 5, "%lu", &masterMicros) == 1) {
        unsigned long localtime = micros();
        timeOffset = localtime - masterMicros;
      }
    }
    else if (strncmp(msg, "ST2:", 4) == 0) {
      parseAndScheduleSequenceV2(msg + 4);
      // preload ack payload "RDY:<seq>" so TX can fetch it with a PING
      // (parseAndScheduleSequenceV2 calls preloadAckReady)
    }
    else if (strncmp(msg, "ST:", 3) == 0) {
      // legacy path kept for compatibility (no READY ack)
      parseAndScheduleSequence(msg + 3);
    }
    else if (strncmp(msg, "PING:", 5) == 0) {
      // TX is polling for READY ack payload; do nothing here.
      // If ST2 was processed, an ack payload is already queued.
    }
    else if (strncmp(msg, "FLASH:", 6) == 0) {
      flashLights();
    }
  }

  runScheduledSteps();
}

// Legacy ST parser (kept)
void parseAndScheduleSequence(const char* data) {
  float timeSec[4];
  int steps = 0;

  char temp[32];
  strncpy(temp, data, sizeof(temp));
  temp[sizeof(temp) - 1] = '\0';

  char* lastColon = strrchr(temp, ':');
  uint8_t newVolume = PLAY_VOL;
  if (lastColon) { *lastColon = '\0'; newVolume = constrain(atoi(lastColon + 1), 0, 30); }

  char* colon = strrchr(temp, ':');
  unsigned long masterStart = 0;
  if (colon) { *colon = '\0'; masterStart = strtoul(colon + 1, NULL, 10); }

  char* token = strtok(temp, "|");
  while (token && steps < 4) { timeSec[steps++] = atof(token); token = strtok(NULL, "|"); }

  unsigned long masterNow = masterStart + timeOffset;
  for (int i = 0; i < steps; i++) eventMicros[i] = masterNow + (unsigned long)(timeSec[i] * 1000000UL);
  totalSteps = steps; currentStep = 0;
  dfplayer.volume(newVolume);
}

// New ST2 parser with dedupe + ack payload preload
void parseAndScheduleSequenceV2(const char* data) {
  // data = "<seq>|<t0|t1|t2|t3>:<masterStart>:<vol>"
  char temp[64];
  strncpy(temp, data, sizeof(temp));
  temp[sizeof(temp)-1] = '\0';

  // Extract seq
  char *bar = strchr(temp, '|');
  if (!bar) return;
  *bar = '\0';
  uint16_t seq = (uint16_t)atoi(temp);

  // Extract last colon (vol)
  char* lastColon = strrchr(bar+1, ':');
  if (!lastColon) return;
  *lastColon = '\0';
  uint8_t newVolume = constrain(atoi(lastColon+1), 0, 30);

  // Extract second-to-last colon (masterStart)
  char* colon = strrchr(bar+1, ':');
  if (!colon) return;
  *colon = '\0';
  unsigned long masterStart = strtoul(colon+1, NULL, 10);

  // Dedupe: ignore if exactly same seq & start already loaded
  if (seq == lastSeq && masterStart == lastMasterStart) {
    preloadAckReady(seq);  // still confirm readiness
    return;
  }

  // Parse step list (bar+1 now holds "t0|t1|...")
  float timeSec[4]; int steps = 0;
  char* tok = strtok(bar+1, "|");
  while (tok && steps < 4) { timeSec[steps++] = atof(tok); tok = strtok(NULL, "|"); }

  // Schedule absolute times
  unsigned long masterNow = masterStart + timeOffset;
  for (int i = 0; i < steps; i++) {
    eventMicros[i] = masterNow + (unsigned long)(timeSec[i] * 1000000UL);
  }
  totalSteps = steps; currentStep = 0;

  // Apply volume
  dfplayer.volume(newVolume);

  // Update dedupe keys
  lastSeq = seq;
  lastMasterStart = masterStart;

  // Preload ack payload so TX can fetch with the next PING:<seq>
  preloadAckReady(seq);
}

static inline void preloadAckReady(uint16_t seq) {
  char ack[16];
  snprintf(ack, sizeof(ack), "RDY:%u", (unsigned)seq);
  // Pipe 0 (we listen on 0). This will be returned with the ACK of the next received packet from TX.
  radio.writeAckPayload(0, ack, strlen(ack)+1);
}

void runScheduledSteps() {
  if (currentStep >= totalSteps || totalSteps == 0) return;
  unsigned long now = micros();
  if ((long)(now - eventMicros[currentStep]) >= 0) {
    executeStep(currentStep);
    currentStep++;
  }
}

static inline void audioArm()   { dfplayer.volume(PLAY_VOL); }
static inline void audioDisarm(){ dfplayer.volume(0); dfplayer.stop(); }

void executeStep(int step) {
  switch (step) {
    case 0:  digitalWrite(RED_LED, HIGH);    audioArm(); dfplayer.play(1); break;
    case 1:  digitalWrite(RED_LED, LOW);     digitalWrite(ORANGE_LED, HIGH); audioArm(); dfplayer.play(2); break;
    case 2:  digitalWrite(ORANGE_LED, LOW);  digitalWrite(GREEN_LED, HIGH);  audioArm(); dfplayer.play(3); break;
    case 3:  allLedsOff();                   audioDisarm(); break;
  }
}

void flashLights(){
  for (int i = 0; i < 3; i++) {
    digitalWrite(GREEN_LED, HIGH);  delay(200);  digitalWrite(GREEN_LED, LOW);
    digitalWrite(ORANGE_LED, HIGH); delay(200);  digitalWrite(ORANGE_LED, LOW);
    digitalWrite(RED_LED, HIGH);    delay(200);  digitalWrite(RED_LED, LOW);
  }
}

void allLedsOff() {
  digitalWrite(RED_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
}
