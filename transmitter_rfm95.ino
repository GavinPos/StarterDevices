// ───────────────────────────────────────────────────────────────
//  Reliable Multi-Device Transmitter for Adafruit RFM95W LoRa
//  • No interrupt pin (polling mode)
//  • 8 target devices (0–7)
//  • Commands: DISCOVER, SYNC, FLASH, START:<pattern>, VOLUME:<n>
//  • Each transmission waits for ACK or retries up to 5×
//  • Configured for 200 m+ reliable range
// ───────────────────────────────────────────────────────────────

#include <SPI.h>
#include <RH_RF95.h>
#include "Packets.h"   // your shared struct definitions

// ─────────── Pin assignments ───────────
#define RFM95_CS    10
#define RFM95_RST   9
#define RF95_FREQ   915.0  // NZ/AU ISM band

// No DIO0 interrupt pin (polling mode)
RH_RF95 rf95(RFM95_CS);

// ─────────── Radio tuning ───────────
static const int NUM_SLAVES = 8;
static const uint8_t RETRY_COUNT = 5;
static const uint16_t ACK_TIMEOUT_MS = 150;

unsigned long startDelaySec = 2;
uint8_t deviceVolume = 20;
uint16_t seqId = 1;
uint8_t flashSeq = 0;

// ─────────── Message wrapper ───────────
struct LoraMsg {
  uint8_t targetId;
  uint8_t type;
  uint16_t seq;
  char payload[48];
};

// ─────────── Utility prototypes ───────────
bool sendReliable(uint8_t targetId, const void* data, size_t len);
bool waitForAck(uint8_t expectTarget, uint16_t expectSeq);
void checkSlave(int idx);
void sendSync(int idx);
void sendFlash(int idx);
void handleMultiStartBinary(const char* input);

// ─────────── Setup ───────────
void setup() {
  Serial.begin(115200);
  delay(100);

  // Manual reset pulse
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    Serial.println("RFM95 init failed – check wiring!");
    while (1);
  }

  // Long-range, high-reliability config
  rf95.setFrequency(RF95_FREQ);
  rf95.setTxPower(20, false);         // max TX power
  rf95.setSpreadingFactor(12);        // SF12 = longest range
  rf95.setSignalBandwidth(125000);    // 125 kHz
  rf95.setCodingRate4(8);             // 4/8 FEC
  rf95.setPreambleLength(8);

  Serial.println("RFM95 Reliable TX (polling mode) ready!");
}

// ─────────── Main loop ───────────
void loop() {
  static char buf[160];
  static size_t idx = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    if (c == '\n' || idx >= sizeof(buf) - 1) {
      buf[idx] = '\0';
      idx = 0;

      if (strcasecmp(buf, "DISCOVER") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { checkSlave(i); delay(50); }

      } else if (strcasecmp(buf, "SYNC") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendSync(i); delay(50); }

      } else if (strcasecmp(buf, "FLASH") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendFlash(i); delay(50); }

      } else if (strncasecmp(buf, "START:", 6) == 0) {
        handleMultiStartBinary(buf + 6);

      } else if (strncasecmp(buf, "VOLUME:", 7) == 0) {
        int v = atoi(buf + 7);
        if (v < 0) v = 0; if (v > 30) v = 30;
        deviceVolume = (uint8_t)v;
        Serial.print("Default volume set to "); Serial.println(deviceVolume);
      }
    } else {
      buf[idx++] = c;
    }
  }
}

// ─────────── Reliable send + ACK wait ───────────
bool sendReliable(uint8_t targetId, const void* data, size_t len) {
  for (int attempt = 1; attempt <= RETRY_COUNT; ++attempt) {
    rf95.send((uint8_t*)data, len);
    rf95.waitPacketSent();

    unsigned long start = millis();
    while (millis() - start < ACK_TIMEOUT_MS) {
      if (rf95.available()) {
        LoraMsg ack{};
        uint8_t rlen = sizeof(ack);
        if (rf95.recv((uint8_t*)&ack, &rlen)) {
          if (ack.targetId == targetId &&
              strcmp(ack.payload, "ACK") == 0)
          {
            Serial.print("ACK from "); Serial.print(targetId);
            Serial.print(" seq "); Serial.println(((LoraMsg*)data)->seq);
            return true;
          }
        }
      }
    }
    Serial.print("Retry "); Serial.print(attempt);
    Serial.print(" → Device "); Serial.println(targetId);
  }

  Serial.print("FAIL "); Serial.println(targetId);
  return false;
}

// ─────────── CHECK command ───────────
void checkSlave(int idx) {
  LoraMsg pkt{};
  pkt.targetId = idx;
  pkt.seq = seqId++;
  strcpy(pkt.payload, "CHECK");
  sendReliable(idx, &pkt, sizeof(pkt));
}

// ─────────── SYNC command ───────────
void sendSync(int idx) {
  LoraMsg pkt{};
  pkt.targetId = idx;
  pkt.seq = seqId++;
  snprintf(pkt.payload, sizeof(pkt.payload), "SYNC:%lu", micros());
  sendReliable(idx, &pkt, sizeof(pkt));
}

// ─────────── FLASH command ───────────
void sendFlash(int idx) {
  LoraMsg pkt{};
  pkt.targetId = idx;
  pkt.seq = seqId++;
  snprintf(pkt.payload, sizeof(pkt.payload), "FLASH:%u", (unsigned)flashSeq++);
  sendReliable(idx, &pkt, sizeof(pkt));
}

// ─────────── START command (binary multi-device) ───────────
void handleMultiStartBinary(const char* input) {
  unsigned long masterStartTime = micros() + startDelaySec * 1000000UL;
  Serial.print("Master Start Time: "); Serial.println(masterStartTime);

  char buf[160];
  strncpy(buf, input, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';

  unsigned long earliestGreenTime = 0xFFFFFFFFUL;

  char* saveptr1;
  char* entry = strtok_r(buf, ";", &saveptr1);
  while (entry) {
    while (*entry == ' ' || *entry == '\t') entry++;

    char* openBrace  = strchr(entry, '{');
    char* closeBrace = strchr(entry, '}');
    if (!openBrace || !closeBrace) {
      Serial.println("Invalid entry format");
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    char idStr[3] = { entry[0], entry[1], '\0' };
    int id = atoi(idStr);
    if (id < 0 || id >= NUM_SLAVES) {
      Serial.print("Invalid ID: "); Serial.println(idStr);
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    char rawSteps[64];
    int rawLen = closeBrace - openBrace - 1;
    if (rawLen < 0) rawLen = 0;
    if ((size_t)rawLen >= sizeof(rawSteps)) rawLen = sizeof(rawSteps) - 1;
    strncpy(rawSteps, openBrace + 1, rawLen);
    rawSteps[rawLen] = '\0';

    double tSec[4] = {0,0,0,0};
    uint8_t steps = 0;
    {
      char tmp[64]; strncpy(tmp, rawSteps, sizeof(tmp));
      tmp[sizeof(tmp)-1] = '\0';
      char* sp2; char* tok = strtok_r(tmp, ",", &sp2);
      while (tok && steps < 4) {
        tSec[steps++] = atof(tok);
        tok = strtok_r(NULL, ",", &sp2);
      }
    }
    if (steps < 3) { Serial.println("Need at least red,orange,green"); entry = strtok_r(NULL, ";", &saveptr1); continue; }

    uint8_t volForThis = deviceVolume;
    char* afterBrace = closeBrace + 1;
    while (*afterBrace==' '||*afterBrace=='\t') afterBrace++;
    if (*afterBrace == '@') {
      int v = atoi(afterBrace + 1);
      if (v < 0) v = 0; if (v > 30) v = 30;
      volForThis = (uint8_t)v;
    }

    StartPacketV1 pkt{};
    pkt.type        = MSG_START_V1;
    pkt.seq         = seqId++;
    pkt.masterStart = masterStartTime;
    pkt.volume      = volForThis;
    pkt.steps       = steps;
    for (uint8_t i = 0; i < steps; i++) {
      uint16_t ds = (uint16_t)(tSec[i] * 10.0 + 0.5);
      pkt.t_ds[i] = ds;
    }

    unsigned long gtime = masterStartTime + (unsigned long)(tSec[2] * 1000000.0);
    if (gtime < earliestGreenTime) earliestGreenTime = gtime;

    sendReliable(id, &pkt, sizeof(pkt));

    Serial.print("START_BIN → "); Serial.print(id);
    Serial.print(" seq="); Serial.print(pkt.seq);
    Serial.print(" vol="); Serial.print(pkt.volume);
    Serial.print(" steps=[");
    for (uint8_t i = 0; i < steps; i++) {
      Serial.print(pkt.t_ds[i]);
      if (i + 1 < steps) Serial.print(',');
    }
    Serial.println("]");

    entry = strtok_r(NULL, ";", &saveptr1);
  }

  if (earliestGreenTime != 0xFFFFFFFFUL) {
    while ((long)(micros() - earliestGreenTime) < 0) delay(1);
    Serial.println("STARTTIMER");
  }
}
