// ───────────────────────────────────────────────────────────────
//  Reliable Multi-Device Transmitter for Adafruit RFM95W LoRa
//  • DIO0 interrupt on pin 2 (RX/TX done)
//  • Hardware reset on pin 9
//  • Commands:
//       DISCOVER → Ping each device (expects ACK, quiet retries)
//       START:<pattern> → Send timed multi-device schedule (expects ACK)
//       FLASH → Broadcast flash command (no ACK wait)
//  • Each reliable command retries up to 3×
//  • Configured for 200 m+ reliable range at SF7 (fast & robust)
// ───────────────────────────────────────────────────────────────

#include <SPI.h>
#include <RH_RF95.h>
#include "Packets.h"     // Shared packet definitions

// ─────────── Pin assignments ───────────
#define RFM95_CS   10
#define RFM95_RST  9
#define RFM95_INT  2
#define RF95_FREQ  915.0   // NZ/AU ISM band

// ─────────── Radio setup ───────────
RH_RF95 rf95(RFM95_CS, RFM95_INT);
static const uint8_t  RETRY_COUNT    = 3;
static const uint16_t ACK_TIMEOUT_MS = 300;
static const uint8_t  NUM_SLAVES     = 8;

uint16_t seqId         = 1;
unsigned long startDelaySec = 2;   // Delay between TX command and start time

// ─────────── Function prototypes ───────────
bool sendReliable(uint8_t targetId, const void* data, size_t len, bool quiet = false);
bool sendDiscover(uint8_t targetId);
void sendFlashBroadcast();
void handleStartCommand(const char* input);

// ─────────── Setup ───────────
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  pinMode(RFM95_INT, INPUT);

  if (!rf95.init()) {
    Serial.println("RFM95 init failed – check wiring!");
    while (1);
  }

  // SF7 for shorter airtime, 200 m+ range in open field
  rf95.setFrequency(RF95_FREQ);
  rf95.setTxPower(20, false);
  rf95.setSpreadingFactor(7);
  rf95.setSignalBandwidth(125000);
  rf95.setCodingRate4(8);
  rf95.setPreambleLength(8);

  Serial.println("RFM95 Transmitter Ready!");
  Serial.println("Commands:");
  Serial.println("  DISCOVER              - Ping each device and expect ACKs");
  Serial.println("  FLASH                 - Broadcast flash command (no ACK)");
  Serial.println("  START:00{1,2,3}@20;01{1.5,2.5,3.5}@15 - Multi-device start");
  Serial.println();
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
        Serial.println("Running DISCOVER...");
        for (uint8_t i = 1; i <= NUM_SLAVES; i++) {
          bool ok = sendDiscover(i);
          Serial.print("DEV");
          if (i < 10) Serial.print('0');
          Serial.print(i);
          Serial.println(ok ? " ✅" : " ❌");
          delay(100);
        }

      } else if (strcasecmp(buf, "FLASH") == 0) {
        sendFlashBroadcast();

      } else if (strncasecmp(buf, "START:", 6) == 0) {
        handleStartCommand(buf + 6);
      }

    } else {
      buf[idx++] = c;
    }
  }
}

// ─────────── Reliable ACK send base ───────────
bool sendReliable(uint8_t targetId, const void* data, size_t len, bool quiet) {
  for (uint8_t attempt = 1; attempt <= RETRY_COUNT; ++attempt) {
    rf95.send((uint8_t*)data, len);
    rf95.waitPacketSent();

    unsigned long start = millis();
    while (millis() - start < ACK_TIMEOUT_MS) {
      if (rf95.available()) {
        ReadyAckV1 ack{};
        uint8_t rlen = sizeof(ack);
        if (rf95.recv((uint8_t*)&ack, &rlen)) {
          if (ack.type == MSG_READY_V1 && ack.seq == ((StartPacketV1*)data)->seq) {
            if (!quiet) {
              Serial.print("ACK from DEV");
              if (targetId < 10) Serial.print('0');
              Serial.println(targetId);
            }
            return true;
          }
        }
      }
    }
    if (!quiet) {
      Serial.print("Retry "); Serial.print(attempt);
      Serial.print(" → DEV");
      if (targetId < 10) Serial.print('0');
      Serial.println(targetId);
    }
  }
  if (!quiet) {
    Serial.print("FAIL → DEV");
    if (targetId < 10) Serial.print('0');
    Serial.println(targetId);
  }
  return false;
}

// ─────────── Discover (1-to-1 reliable, quiet retries) ───────────
bool sendDiscover(uint8_t targetId) {
  DiscoverPacketV1 pkt{};
  pkt.type     = MSG_DISCOVER_V1;
  pkt.seq      = seqId++;
  pkt.targetId = targetId;
  return sendReliable(targetId, &pkt, sizeof(pkt), true);  // quiet mode on
}

// ─────────── FLASH broadcast (no ACK) ───────────
void sendFlashBroadcast() {
  BroadcastPacketV1 pkt{};
  pkt.type    = MSG_BROADCAST_V1;
  pkt.seq     = seqId++;
  pkt.command = CMD_FLASH;

  rf95.send((uint8_t*)&pkt, sizeof(pkt));
  rf95.waitPacketSent();

  Serial.println("FLASH broadcast sent to all devices.");
}

// ─────────── START command (multi-device) ───────────
void handleStartCommand(const char* input) {
    char buf[160];
  strncpy(buf, input, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';

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
    if (id < 1 || id > NUM_SLAVES) {
      Serial.print("Invalid ID: "); Serial.println(idStr);
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    // Parse times
    char rawSteps[64];
    int rawLen = closeBrace - openBrace - 1;
    if (rawLen < 0) rawLen = 0;
    if ((size_t)rawLen >= sizeof(rawSteps)) rawLen = sizeof(rawSteps) - 1;
    strncpy(rawSteps, openBrace + 1, rawLen);
    rawSteps[rawLen] = '\0';

    double tSec[4] = {0, 0, 0, 0};
    uint8_t steps = 0;
    char tmp[64]; strncpy(tmp, rawSteps, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    char* sp2; char* tok = strtok_r(tmp, ",", &sp2);
    while (tok && steps < 4) {
      tSec[steps++] = atof(tok);
      tok = strtok_r(NULL, ",", &sp2);
    }
    if (steps < 3) { Serial.println("Need at least red,orange,green"); entry = strtok_r(NULL, ";", &saveptr1); continue; }

    // Parse optional volume @N
    uint8_t volForThis = 20; // default
    char* afterBrace = closeBrace + 1;
    while (*afterBrace == ' ' || *afterBrace == '\t') afterBrace++;
    if (*afterBrace == '@') {
      int v = atoi(afterBrace + 1);
      volForThis = constrain(v, 0, 30);
    }

    unsigned long currentClock = micros();
    unsigned long masterStartTime = currentClock + startDelaySec * 1000000UL;
    Serial.print("Master Start Time: "); Serial.println(masterStartTime);

    // Build packet
    StartPacketV1 pkt{};
    pkt.type        = MSG_START_V1;
    pkt.seq         = seqId++;
    pkt.targetId    = id;
    pkt.currentClock = currentClock;
    pkt.masterStart = masterStartTime;
    pkt.volume      = volForThis;
    pkt.steps       = steps;
    memset(pkt.t_ds, 0, sizeof(pkt.t_ds));
    for (uint8_t i = 0; i < steps; i++)
      pkt.t_ds[i] = (uint16_t)(tSec[i] * 10.0 + 0.5);

    // Send reliably (with ACK wait and retries)
    sendReliable(id, &pkt, sizeof(pkt));

    Serial.print("START → DEV");
    if (id < 10) Serial.print('0');
    Serial.print(id);
    Serial.print(" vol="); Serial.print(pkt.volume);
    Serial.print(" steps=[");
    for (uint8_t i = 0; i < steps; i++) {
      Serial.print(pkt.t_ds[i]);
      if (i + 1 < steps) Serial.print(',');
    }
    Serial.println("]");

    entry = strtok_r(NULL, ";", &saveptr1);
  }

  Serial.println("START sequence complete.");
}
