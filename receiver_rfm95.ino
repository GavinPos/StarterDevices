// ───────────────────────────────────────────────────────────────
//  Reliable Multi-Device Receiver for Adafruit RFM95W LoRa
//  • Listens for DISCOVER, START, and BROADCAST (FLASH)
//  • Responds with ACKs for 1-to-1 commands
//  • Executes timed LED/audio steps for START command
//  • Uses DFPlayer Mini for audio playback
//  • No Serial output (headless deployment)
//  • FIXED: Proper timing alignment with master clock
// ───────────────────────────────────────────────────────────────

#include <SPI.h>
#include <RH_RF95.h>
#include <DFRobotDFPlayerMini.h>
#include "Packets.h"

// ─────────── Pin Assignments ───────────
#define RFM95_CS   10
#define RFM95_RST  9
#define RFM95_INT  2
#define RF95_FREQ  915.0

#define LED_RED     5
#define LED_ORANGE  6
#define LED_GREEN   7

#define DEVICE_ID   2   // Change per device (00–07)

// ─────────── Radio & Audio Setup ───────────
RH_RF95 rf95(RFM95_CS, RFM95_INT);
DFRobotDFPlayerMini dfplayer;

// ─────────── Timing & Audio Settings ───────────
unsigned long timeOffset = 0;
const uint8_t  DEFAULT_VOL = 20;
const uint16_t LEAD_T_MS[3] = {1000, 1000, 1000};
const uint8_t  TRACK_RED=1, TRACK_ORANGE=2, TRACK_GREEN=3, TRACK_START=4;
const uint8_t  TRACK_FOR_STEP[4] = {TRACK_RED, TRACK_ORANGE, TRACK_GREEN, TRACK_START};

// ─────────── Scheduler ───────────
struct Action { unsigned long t; uint8_t type; uint8_t arg; };
enum { ACT_PLAY=1, ACT_LED_RED=2, ACT_LED_ORANGE=3, ACT_LED_GREEN=4, ACT_ALL_OFF=5 };
#define MAX_ACTIONS 12
Action actions[MAX_ACTIONS];
uint8_t totalSteps = 0;
uint8_t currentStep = 0;
uint8_t scheduleVolume = DEFAULT_VOL;

// ─────────── Utility helpers ───────────
void allLedsOff() { digitalWrite(LED_RED,LOW); digitalWrite(LED_ORANGE,LOW); digitalWrite(LED_GREEN,LOW); }
void audioStop() { dfplayer.stop(); dfplayer.volume(0); }

static inline void addAction(unsigned long t, uint8_t type, uint8_t arg=0) {
  if (totalSteps < MAX_ACTIONS) actions[totalSteps++] = {t,type,arg};
}

void sortActions() {
  for (int i=1;i<totalSteps;i++){
    Action key=actions[i]; int j=i-1;
    while (j>=0 && (long)(actions[j].t - key.t)>0){ actions[j+1]=actions[j]; j--; }
    actions[j+1]=key;
  }
}

void executeStep(int idx){
  switch(actions[idx].type){
    case ACT_PLAY:       dfplayer.stop(); dfplayer.volume(scheduleVolume); dfplayer.playMp3Folder(actions[idx].arg); break;
    case ACT_LED_RED:    digitalWrite(LED_RED,HIGH); break;
    case ACT_LED_ORANGE: digitalWrite(LED_RED,LOW); digitalWrite(LED_ORANGE,HIGH); break;
    case ACT_LED_GREEN:  digitalWrite(LED_ORANGE,LOW); digitalWrite(LED_GREEN,HIGH); break;
    case ACT_ALL_OFF:    allLedsOff(); audioStop(); break;
  }
}

void runScheduledSteps(){
  if (currentStep>=totalSteps || totalSteps==0) return;
  unsigned long now = micros();
  if ((long)(now - actions[currentStep].t) >= 0) { executeStep(currentStep++); }
}

// ─────────── ACK helper ───────────
void sendAck(uint16_t seq) {
  ReadyAckV1 ack{};
  ack.type = MSG_READY_V1;
  ack.seq  = seq;
  rf95.send((uint8_t*)&ack, sizeof(ack));
  rf95.waitPacketSent();
}

// ─────────── Command handlers ───────────
void handleDiscover(const DiscoverPacketV1& pkt) {
  if (pkt.targetId != DEVICE_ID) return;
  sendAck(pkt.seq);
}

void handleStart(const StartPacketV1& pkt) {
  if (pkt.targetId != DEVICE_ID) return;  // Ignore if not for me	
  sendAck(pkt.seq);

  totalSteps=0; currentStep=0;
  scheduleVolume = pkt.volume;

  // ─── FIXED: Synchronize master and receiver clocks ───
  // Compute local start time based on master’s currentClock
  unsigned long localStart = micros() + (pkt.masterStart - pkt.currentClock);
  unsigned long base = localStart;

  for (uint8_t i=0; i<pkt.steps && i<3; i++){
    unsigned long stepTime = base + (unsigned long)pkt.t_ds[i]*100000UL;
    unsigned long playTime = stepTime - (unsigned long)LEAD_T_MS[i]*1000UL;
    if ((long)(playTime - base) < 0) playTime = base;
    addAction(playTime, ACT_PLAY, TRACK_FOR_STEP[i]);
    addAction(stepTime, (i==0)?ACT_LED_RED:(i==1)?ACT_LED_ORANGE:ACT_LED_GREEN);
  }

  if (pkt.steps >= 4) {
    unsigned long offT = base + (unsigned long)pkt.t_ds[3]*100000UL;
    addAction(offT, ACT_ALL_OFF);
  }

  sortActions();
}

void handleBroadcast(const BroadcastPacketV1& pkt) {
  if (pkt.command == CMD_FLASH) {
    for (int i=0;i<3;i++){
      digitalWrite(LED_GREEN,HIGH); delay(200);
      digitalWrite(LED_GREEN,LOW);  delay(200);
      digitalWrite(LED_ORANGE,HIGH); delay(200);
      digitalWrite(LED_ORANGE,LOW);  delay(200);
      digitalWrite(LED_RED,HIGH); delay(200);
      digitalWrite(LED_RED,LOW);  delay(200);
    }
  }
}

// ─────────── Setup ───────────
void setup() {
  pinMode(LED_RED,OUTPUT);
  pinMode(LED_ORANGE,OUTPUT);
  pinMode(LED_GREEN,OUTPUT);
  allLedsOff();

  // DFPlayer
  Serial.begin(9600);
  if (dfplayer.begin(Serial)) {
    dfplayer.outputDevice(DFPLAYER_DEVICE_SD);
    dfplayer.volume(DEFAULT_VOL);
    dfplayer.EQ(DFPLAYER_EQ_NORMAL);
  }

  // LoRa setup
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW); delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);

  rf95.init();
  rf95.setFrequency(RF95_FREQ);
  rf95.setTxPower(20, false);
  rf95.setSpreadingFactor(7);
  rf95.setSignalBandwidth(125000);
  rf95.setCodingRate4(8);
  rf95.setPreambleLength(8);
}

// ─────────── Main loop ───────────
void loop() {
  if (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    if (!rf95.recv(buf, &len)) return;
    if (len < 3) return;

    uint8_t msgType = buf[0];
    switch (msgType) {
      case MSG_DISCOVER_V1:
        if (len == sizeof(DiscoverPacketV1)) {
          DiscoverPacketV1 pkt; memcpy(&pkt, buf, len);
          handleDiscover(pkt);
        }
        break;

      case MSG_START_V1:
        if (len == sizeof(StartPacketV1)) {
          StartPacketV1 pkt; memcpy(&pkt, buf, len);
          handleStart(pkt);
        }
        break;

      case MSG_BROADCAST_V1:
        if (len == sizeof(BroadcastPacketV1)) {
          BroadcastPacketV1 pkt; memcpy(&pkt, buf, len);
          handleBroadcast(pkt);
        }
        break;

      default:
        break;
    }
  }

  runScheduledSteps();
}
