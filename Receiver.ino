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
const uint8_t  START_VOL        = 5;   // runtime volume (0..30)
const uint16_t STARTUP_SOUND_MS = 0; // how long to let track 1 play at boot; set 0 to let it play fully

// Fwds
void allLedsOff();
void flashLights();
void parseAndScheduleSequence(const char* data);
void runScheduledSteps();
void executeStep(int step);

void setup() {
  // LEDs
  pinMode(RED_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  allLedsOff();

  // ---- Protect DFPlayer from boot chatter on pins 0/1 ----
  Serial.begin(9600);
  delay(BOOT_GUARD_MS);            // let bootloader TX finish
  while (Serial.available()) {     // dump any garbage bytes
    Serial.read();
  }

  // Init DFPlayer muted
  if (dfplayer.begin(Serial)) {
    dfplayer.volume(0);            // start silent
    dfplayer.stop();

    // ---- STARTUP SOUND CHECK ----
    dfplayer.volume(START_VOL);
    dfplayer.play(4);              // play first file (0001.mp3)
    if (STARTUP_SOUND_MS > 0) {
      delay(STARTUP_SOUND_MS);     // brief audible check
      dfplayer.stop();             // stop early (optional)
      dfplayer.volume(0);          // return to quiet until steps run
    }
  }
  // NOTE: During sketch upload, disconnect DFPlayer RX from Arduino TX0.

  // Radio
  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(true);
  radio.setDataRate(RF24_250KBPS);
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
      radio.stopListening();
      char reply[16];
      snprintf(reply, sizeof(reply), "ACK:%s", thisDeviceAddr + 3); // e.g. "ACK:02"
      radio.write(&reply, strlen(reply) + 1);
      radio.startListening();
    }
    else if (strncmp(msg, "SYNC:", 5) == 0) {
      unsigned long masterMicros;
      if (sscanf(msg + 5, "%lu", &masterMicros) == 1) {
        unsigned long localtime = micros();
        timeOffset = localtime - masterMicros;
      }
    }
    else if (strncmp(msg, "ST:", 3) == 0) {
      parseAndScheduleSequence(msg + 3);
    }
    else if (strncmp(msg, "FLASH:", 6) == 0) {
      flashLights();
    }
  }

  runScheduledSteps();
}

void parseAndScheduleSequence(const char* data) {
  float timeSec[4];
  int steps = 0;

  char temp[32];
  strncpy(temp, data, sizeof(temp));
  temp[sizeof(temp) - 1] = '\0';

  // Find last colon → volume
  char* lastColon = strrchr(temp, ':');
  uint8_t newVolume = PLAY_VOL; // default if not provided
  if (lastColon) {
    *lastColon = '\0';
    newVolume = constrain(atoi(lastColon + 1), 0, 30);
  }

  // Find second-to-last colon → masterStart
  char* colon = strrchr(temp, ':');
  unsigned long masterStart = 0;
  if (colon) {
    *colon = '\0';
    masterStart = strtoul(colon + 1, NULL, 10);
  }

  // Parse step times
  char* token = strtok(temp, "|");
  while (token && steps < 4) {
    timeSec[steps++] = atof(token);
    token = strtok(NULL, "|");
  }

  // Compute absolute event times
  unsigned long masterNow = masterStart + timeOffset;
  for (int i = 0; i < steps; i++) {
    eventMicros[i] = masterNow + (unsigned long)(timeSec[i] * 1000000UL);
  }
  totalSteps = steps;
  currentStep = 0;

  // Set DFPlayer volume immediately
  dfplayer.volume(newVolume);
}


void runScheduledSteps() {
  if (currentStep >= totalSteps || totalSteps == 0) return;

  unsigned long now = micros();
  if ((long)(now - eventMicros[currentStep]) >= 0) { // handle wrap
    executeStep(currentStep);
    currentStep++;
  }
}

static inline void audioArm() { dfplayer.volume(PLAY_VOL); }
static inline void audioDisarm() { dfplayer.volume(0); dfplayer.stop(); }

void executeStep(int step) {
  switch (step) {
    case 0:  // RED ON + play track 1
      digitalWrite(RED_LED, HIGH);
      audioArm();
      dfplayer.play(1);
      break;

    case 1:  // RED OFF, ORANGE ON + play track 2
      digitalWrite(RED_LED, LOW);
      digitalWrite(ORANGE_LED, HIGH);
      audioArm();
      dfplayer.play(2);
      break;

    case 2:  // ORANGE OFF, GREEN ON + play track 3
      digitalWrite(ORANGE_LED, LOW);
      digitalWrite(GREEN_LED, HIGH);
      audioArm();
      dfplayer.play(3);
      break;

    case 3:  // ALL OFF + quiet down
      allLedsOff();
      audioDisarm();
      break;
  }
}

void flashLights(){
  for (int i = 0; i < 3; i++) {
    digitalWrite(GREEN_LED, HIGH);
    delay(200);
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(ORANGE_LED, HIGH);
    delay(200);
    digitalWrite(ORANGE_LED, LOW);
    digitalWrite(RED_LED, HIGH);
    delay(200);
    digitalWrite(RED_LED, LOW);
  }
}

void allLedsOff() {
  digitalWrite(RED_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
}
