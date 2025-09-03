#include <SPI.h>
#include <RF24.h>
#include <DFRobotDFPlayerMini.h>

RF24 radio(9, 10);  // CE, CSN
static const byte thisDeviceAddr[6] = "DEV03";

// ---------- LED pins ----------
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

// ---------- Time management ----------
unsigned long timeOffset = 0;     // local micros() - masterMicros
unsigned long eventMicros[4];     // (legacy buffer; not used by new scheduler)
int currentStep = 0;
int totalSteps  = 0;

// ---------- DFPlayer ----------
DFRobotDFPlayerMini dfplayer;
const uint16_t BOOT_GUARD_MS    = 1800; // wait out bootloader chatter on TX0
const uint8_t  PLAY_VOL         = 20;   // runtime volume (0..30)
const uint8_t  START_VOL        = 20;   // startup test volume
const uint16_t STARTUP_SOUND_MS = 500;  // 0 = skip startup tone

// Tracks for each light (DFPlayer expects 0001.mp3, 0002.mp3, 0003.mp3, etc.)
const uint8_t TRACK_RED    = 1;
const uint8_t TRACK_ORANGE = 2;
const uint8_t TRACK_GREEN  = 3;
const uint8_t TRACK_START  = 4;
const uint8_t TRACK_FOR_STEP[4] = { TRACK_RED, TRACK_ORANGE, TRACK_GREEN, TRACK_START };

// Audio lead (ms) BEFORE each LED so sound starts slightly early
const uint16_t LEAD_T_MS[3] = { 1000, 1000, 1000 };  // tune per light

// ---------- Dedupe state for ST2 ----------
uint16_t lastSeq = 0;
unsigned long lastMasterStart = 0;

// ---------- Action scheduler ----------
struct Action { unsigned long t; uint8_t type; uint8_t arg; };
enum { ACT_PLAY=1, ACT_LED_RED=2, ACT_LED_ORANGE=3, ACT_LED_GREEN=4, ACT_ALL_OFF=5 };
#define MAX_ACTIONS 12
Action actions[MAX_ACTIONS];
uint8_t scheduleVolume = PLAY_VOL;

static inline void addAction(unsigned long t, uint8_t type, uint8_t arg=0) {
  if (totalSteps < MAX_ACTIONS) actions[totalSteps++] = {t, type, arg};
}

static inline void sortActionsByTime() {
  for (int i=1;i<totalSteps;i++){
    Action key = actions[i]; int j=i-1;
    while (j>=0 && (long)(actions[j].t - key.t) > 0) { actions[j+1]=actions[j]; j--; }
    actions[j+1]=key;
  }
}

// ---------- Fwds ----------
void allLedsOff();
void flashLights();
void parseAndScheduleSequence(const char* data);   // ST2
void runScheduledSteps();
void executeStep(int idx);
static inline void preloadAckReady(uint16_t seq);
static inline void audioDisarm();
void playSoft(uint8_t track, uint8_t targetVol = PLAY_VOL);

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

  // DFPlayer init
  if (dfplayer.begin(Serial)) {
    delay(800);                             // <-- important on many clones
	dfplayer.outputDevice(DFPLAYER_DEVICE_SD);
	dfplayer.EQ(DFPLAYER_EQ_NORMAL);
	dfplayer.stop();
	dfplayer.volume(0);
							
							   
    if (STARTUP_SOUND_MS > 0) {
      dfplayer.volume(START_VOL);
      dfplayer.playMp3Folder(TRACK_START);      // or any track you want on boot
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
  // Drain all queued packets (important if any handler blocks briefly)
  while (radio.available()) {
																 
					 
								
    uint8_t len = radio.getDynamicPayloadSize();
    if (!len || len > 32) { radio.flush_rx(); break; }
				
		  

    char msg[33] = {0};
    radio.read(msg, len);

    if (strcmp(msg, "CHECK") == 0) {
      radio.stopListening();
      char reply[16];
      snprintf(reply, sizeof(reply), "ACK:%s", thisDeviceAddr + 3);
      (void)radio.write(reply, strlen(reply) + 1);
      radio.startListening();
    }
    else if (strncmp(msg, "SYNC:", 5) == 0) {
      unsigned long masterMicros;
      if (sscanf(msg + 5, "%lu", &masterMicros) == 1) {
        timeOffset = micros() - masterMicros;   // (local - master)
      }
    }
    else if (strncmp(msg, "ST2:", 4) == 0) {
      parseAndScheduleSequence(msg + 4);      // preloads RDY:<seq>
    }
    else if (strncmp(msg, "PING:", 5) == 0) {
      // READY ack payload was preloaded in ST2 parser
																
    }
    else if (strncmp(msg, "FLASH:", 6) == 0) {
      flashLights();                // simple debug animation
   
      radio.flush_rx();
      radio.startListening();
    }
  }

  runScheduledSteps();
}

// ---------- ST2 parser with dedupe + READY ack preload ----------
void parseAndScheduleSequence(const char* data) {
  // data = "<seq>|<t0|t1|t2|t3>:<masterStart>:<vol>"
  char temp[80];
  strncpy(temp, data, sizeof(temp));
  temp[sizeof(temp)-1] = '\0';

  // seq
  char *bar = strchr(temp, '|');
  if (!bar) return;
  *bar = '\0';
  uint16_t seq = (uint16_t)atoi(temp);

  // volume (last colon)
  char* lastColon = strrchr(bar+1, ':');
  if (!lastColon) return;
  *lastColon = '\0';
  uint8_t newVolume = constrain(atoi(lastColon+1), 0, 30);

  // masterStart (second-to-last colon)
  char* colon = strrchr(bar+1, ':');
  if (!colon) return;
  *colon = '\0';
  unsigned long masterStart = strtoul(colon+1, NULL, 10);

  // Dedupe
  if (seq == lastSeq && masterStart == lastMasterStart) {
    preloadAckReady(seq);
    return;
  }

  // Parse times t0|t1|t2|[t3]
  float timeSec[4]; int steps = 0;
  char* tok = strtok(bar+1, "|");
  while (tok && steps < 4) { timeSec[steps++] = atof(tok); tok = strtok(NULL, "|"); }

  // Build actions with leads
  unsigned long masterNow = masterStart + timeOffset;
  unsigned long ledT[3] = {0,0,0};
  for (int i = 0; i < steps && i < 3; i++) {
    ledT[i] = masterNow + (unsigned long)(timeSec[i] * 1000000UL);
  }
									  

  scheduleVolume = newVolume;
  totalSteps = 0; currentStep = 0;

  for (int i = 0; i < steps && i < 3; i++) {
    unsigned long playT = ledT[i] - (unsigned long)LEAD_T_MS[i] * 1000UL;
    addAction(playT,   ACT_PLAY,      TRACK_FOR_STEP[i]);
    addAction(ledT[i], (i==0)?ACT_LED_RED:(i==1)?ACT_LED_ORANGE:ACT_LED_GREEN);
  }

  if (steps >= 4) {
    unsigned long allOffT = masterNow + (unsigned long)(timeSec[3] * 1000000UL);
    addAction(allOffT, ACT_ALL_OFF);
  }

  sortActionsByTime();

  // Update dedupe keys + READY ack
  lastSeq = seq;
  lastMasterStart = masterStart;

																 
  preloadAckReady(seq);
}

static inline void preloadAckReady(uint16_t seq) {
  char ack[16];
  snprintf(ack, sizeof(ack), "RDY:%u", (unsigned)seq);
																									 
  radio.writeAckPayload(0, ack, strlen(ack)+1);  // returned with next ACK to TX
}

void runScheduledSteps() {
  if (currentStep >= totalSteps || totalSteps == 0) return;
  unsigned long now = micros();
  if ((long)(now - actions[currentStep].t) >= 0) {
    executeStep(currentStep);
    currentStep++;
  }
}

void executeStep(int idx) {
  switch (actions[idx].type) {
    case ACT_PLAY:
      playSoft(actions[idx].arg, scheduleVolume);
      break;

    case ACT_LED_RED:
      digitalWrite(RED_LED, HIGH);
      break;

    case ACT_LED_ORANGE:
      digitalWrite(RED_LED, LOW);
      digitalWrite(ORANGE_LED, HIGH);
      break;

    case ACT_LED_GREEN:
      digitalWrite(ORANGE_LED, LOW);
      digitalWrite(GREEN_LED, HIGH);
      break;

    case ACT_ALL_OFF:
      allLedsOff();
      audioDisarm();
      break;
  }
}

static inline void audioDisarm(){ dfplayer.volume(0); dfplayer.stop(); }

void playSoft(uint8_t track, uint8_t targetVol) {
  // Non-blocking: set volume instantly and start playback, then return.
  targetVol = constrain(targetVol, 0, 30);
  dfplayer.stop();                 // optional: ensure clean start
  dfplayer.volume(targetVol);      // set final volume immediately
  dfplayer.playMp3Folder(track);            // start playback
  // no delay(), no loops — returns immediately so scheduling stays precise
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
