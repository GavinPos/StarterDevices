#include <SPI.h>
#include <RH_RF95.h>
#include <DFRobotDFPlayerMini.h>
#include "Packets.h"

// ─────────── Pin Assignments ───────────
#define RFM95_CS    10
#define RFM95_RST   9
#define RF95_FREQ   915.0

RH_RF95 rf95(RFM95_CS);   // polling mode, no interrupt pin

// ─────────── Device Config ───────────
#define DEVICE_ID   3     // CHANGE per device (0–7)

// LEDs
const int RED_LED=5, ORANGE_LED=6, GREEN_LED=7;

// Time sync
unsigned long timeOffset = 0;

// DFPlayer
DFRobotDFPlayerMini dfplayer;
const uint16_t BOOT_GUARD_MS = 1800;
const uint8_t  PLAY_VOL = 20;
const uint8_t  START_VOL = 20;
const uint16_t STARTUP_SOUND_MS = 500;
const uint8_t  TRACK_RED=1, TRACK_ORANGE=2, TRACK_GREEN=3, TRACK_START=4;
const uint8_t  TRACK_FOR_STEP[4] = { TRACK_RED, TRACK_ORANGE, TRACK_GREEN, TRACK_START };
const uint16_t LEAD_T_MS[3] = { 1000, 1000, 1000 };

uint16_t lastSeq = 0;
unsigned long lastMasterStart = 0;

// ─────────── Scheduler ───────────
struct Action { unsigned long t; uint8_t type; uint8_t arg; };
enum { ACT_PLAY=1, ACT_LED_RED=2, ACT_LED_ORANGE=3, ACT_LED_GREEN=4, ACT_ALL_OFF=5 };
#define MAX_ACTIONS 12
Action actions[MAX_ACTIONS];
int currentStep=0, totalSteps=0;
uint8_t scheduleVolume = PLAY_VOL;

static inline void addAction(unsigned long t, uint8_t type, uint8_t arg=0) {
  if (totalSteps < MAX_ACTIONS) actions[totalSteps++] = {t, type, arg};
}
static inline void sortActionsByTime() {
  for (int i=1;i<totalSteps;i++){ Action key=actions[i]; int j=i-1;
    while (j>=0 && (long)(actions[j].t - key.t) > 0) { actions[j+1]=actions[j]; j--; }
    actions[j+1]=key;
  }
}

void allLedsOff(){ digitalWrite(RED_LED,LOW); digitalWrite(ORANGE_LED,LOW); digitalWrite(GREEN_LED,LOW); }
static inline void audioDisarm(){ dfplayer.volume(0); dfplayer.stop(); }
void playSoft(uint8_t track, uint8_t targetVol){
  targetVol = constrain(targetVol,0,30);
  dfplayer.stop(); dfplayer.volume(targetVol); dfplayer.playMp3Folder(track);
}
void executeStep(int idx){
  switch(actions[idx].type){
    case ACT_PLAY:        playSoft(actions[idx].arg, scheduleVolume); break;
    case ACT_LED_RED:     digitalWrite(RED_LED,HIGH); break;
    case ACT_LED_ORANGE:  digitalWrite(RED_LED,LOW); digitalWrite(ORANGE_LED,HIGH); break;
    case ACT_LED_GREEN:   digitalWrite(ORANGE_LED,LOW); digitalWrite(GREEN_LED,HIGH); break;
    case ACT_ALL_OFF:     allLedsOff(); audioDisarm(); break;
  }
}
void runScheduledSteps(){
  if (currentStep>=totalSteps || totalSteps==0) return;
  unsigned long now = micros();
  if ((long)(now - actions[currentStep].t) >= 0) { executeStep(currentStep); currentStep++; }
}

// ─────────── Parse START Packet ───────────
void parseStartBinary(const StartPacketV1& pkt){
  if (pkt.seq == lastSeq && pkt.masterStart == lastMasterStart) return;

  unsigned long base = pkt.masterStart + timeOffset;
  totalSteps = 0; currentStep = 0;
  scheduleVolume = pkt.volume;

  auto ds_to_us = [](uint16_t ds)->unsigned long { return (unsigned long)ds * 100000UL; };
  unsigned long ledT[3] = {0,0,0};
  for (uint8_t i=0; i<pkt.steps && i<3; i++) ledT[i] = base + ds_to_us(pkt.t_ds[i]);

  for (uint8_t i=0; i<pkt.steps && i<3; i++){
    unsigned long playT = ledT[i] - (unsigned long)LEAD_T_MS[i]*1000UL;
    if ((long)(playT - base) < 0) playT = base;
    addAction(playT, ACT_PLAY, TRACK_FOR_STEP[i]);
    addAction(ledT[i], (i==0)?ACT_LED_RED:(i==1)?ACT_LED_ORANGE:ACT_LED_GREEN);
  }
  if (pkt.steps >= 4){
    unsigned long allOffT = base + ds_to_us(pkt.t_ds[3]);
    addAction(allOffT, ACT_ALL_OFF);
  }
  sortActionsByTime();

  lastSeq = pkt.seq;
  lastMasterStart = pkt.masterStart;
}

// ─────────── Helper: flash test lights ───────────
void flashLights(){
  for (int i=0;i<3;i++){
    digitalWrite(GREEN_LED,HIGH);  delay(200);  digitalWrite(GREEN_LED,LOW);
    digitalWrite(ORANGE_LED,HIGH); delay(200);  digitalWrite(ORANGE_LED,LOW);
    digitalWrite(RED_LED,HIGH);    delay(200);  digitalWrite(RED_LED,LOW);
  }
}

// ─────────── Setup ───────────
void setup() {
  pinMode(RED_LED,OUTPUT); pinMode(ORANGE_LED,OUTPUT); pinMode(GREEN_LED,OUTPUT);
  allLedsOff();

  // DFPlayer serial (hardware serial)
  Serial.begin(9600);
  delay(BOOT_GUARD_MS);
  while (Serial.available()) { Serial.read(); }

  if (dfplayer.begin(Serial)) {
    delay(800);
    dfplayer.outputDevice(DFPLAYER_DEVICE_SD);
    dfplayer.EQ(DFPLAYER_EQ_NORMAL);
    dfplayer.stop();
    dfplayer.volume(0);
    if (STARTUP_SOUND_MS>0){
      dfplayer.volume(START_VOL); dfplayer.playMp3Folder(TRACK_START);
      delay(STARTUP_SOUND_MS); dfplayer.stop(); dfplayer.volume(0);
    }
  }

  // LoRa setup
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  rf95.init();
  rf95.setFrequency(RF95_FREQ);
  rf95.setTxPower(20, false);
  rf95.setSpreadingFactor(12);
  rf95.setSignalBandwidth(125000);
  rf95.setCodingRate4(8);
  rf95.setPreambleLength(8);

  // Self-test LED flash
  for(int i=0;i<3;i++){
    digitalWrite(RED_LED,HIGH); digitalWrite(ORANGE_LED,HIGH); digitalWrite(GREEN_LED,HIGH);
    delay(250); allLedsOff(); delay(250);
  }
}

// ─────────── ACK send ───────────
void sendAck(uint8_t targetId, uint16_t seq) {
  struct LoraMsg {
    uint8_t targetId;
    uint8_t type;
    uint16_t seq;
    char payload[48];
  } ack{ targetId, 1, seq, "ACK" };
  rf95.send((uint8_t*)&ack, sizeof(ack));
  rf95.waitPacketSent();
}

// ─────────── Main Loop ───────────
void loop() {
  if (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    if (!rf95.recv(buf, &len)) return;
    if (len < 4) return;

    struct LoraMsg {
      uint8_t targetId;
      uint8_t type;
      uint16_t seq;
      char payload[48];
    };
    LoraMsg* msg = (LoraMsg*)buf;

    if (msg->targetId != DEVICE_ID) return;

    // Binary START
    if (msg->payload[0] == MSG_START_V1 && len == sizeof(StartPacketV1)) {
      StartPacketV1 pkt{};
      memcpy(&pkt, buf, sizeof(pkt));
      parseStartBinary(pkt);
    }
    // CHECK
    else if (strncmp(msg->payload, "CHECK", 5) == 0) {
      sendAck(msg->targetId, msg->seq);
    }
    // SYNC
    else if (strncmp(msg->payload, "SYNC:", 5) == 0) {
      unsigned long masterMicros = strtoul(msg->payload + 5, NULL, 10);
      timeOffset = micros() - masterMicros;
      sendAck(msg->targetId, msg->seq);
    }
    // FLASH
    else if (strncmp(msg->payload, "FLASH:", 6) == 0) {
      flashLights();
      sendAck(msg->targetId, msg->seq);
    }
    // START (simple start trigger)
    else if (strncmp(msg->payload, "START", 5) == 0) {
      sendAck(msg->targetId, msg->seq);
    }
    // Otherwise ignore unknowns
  }

  runScheduledSteps();
}
