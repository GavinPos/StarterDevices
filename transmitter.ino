// ───────────────── Transmitter (binary START) ─────────────────
// Keep your existing includes
#include <SPI.h>
#include <RF24.h>
#include "Packets.h"   // ← the shared structs above

RF24 radio(9, 10);  // CE, CSN

static const byte slaveAddrs[][6] = {
  "DEV00","DEV01","DEV02","DEV03","DEV04","DEV05","DEV06","DEV07"
};
const int NUM_SLAVES = 8;

unsigned long startDelaySec = 2;
uint8_t deviceVolume = 20;

static uint16_t seqId = 1;
static uint8_t  flashSeq = 0;

void txBeginTo(int idx) {
  radio.stopListening();
  radio.openWritingPipe(slaveAddrs[idx]);
  radio.flush_tx();
}
void txEnd() { radio.startListening(); }

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(true);
  radio.setRetries(5, 15);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(108);
  radio.setCRCLength(RF24_CRC_16);
  radio.enableDynamicPayloads();
  radio.enableAckPayload();

  radio.openReadingPipe(0, slaveAddrs[0]); // keep state sane
  radio.startListening();
}

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
        for (int i = 0; i < NUM_SLAVES; i++) { checkSlave(i); delay(10); }
      } else if (strcasecmp(buf, "SYNC") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendSync(i); delay(10); }
      } else if (strcasecmp(buf, "FLASH") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendFlash(i); delay(10); }
      } else if (strncasecmp(buf, "START:", 6) == 0) {
        handleMultiStartBinary(buf + 6);           // <<<<<<<<<< binary path
      } else if (strncasecmp(buf, "VOLUME:", 7) == 0) {
        int v = atoi(buf + 7); if (v<0) v=0; if (v>30) v=30;
        deviceVolume = (uint8_t)v;
        Serial.print("Default volume set to "); Serial.println(deviceVolume);
      }
    } else {
      buf[idx++] = c;
    }
  }
}

// ───────────── helpers you already had (mostly unchanged) ─────────────

void checkSlave(int idx) {
  txBeginTo(idx);
  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write("CHECK", 6);
    if (ok) break;
    delay(10);
  }
  txEnd();
  Serial.print("CHECK "); Serial.print((char*)slaveAddrs[idx]);
  Serial.println(ok ? " ACKed" : " FAILED");
}

void sendSync(int idx) {
  unsigned long masterMicros = micros();
  char sbuf[32];
  snprintf(sbuf, sizeof(sbuf), "SYNC:%lu", masterMicros);
  txBeginTo(idx);
  bool ok = radio.write(sbuf, strlen(sbuf) + 1);
  txEnd();

  Serial.print("SYNC  "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " OK" : " FAIL");
  Serial.print(" @"); Serial.println(masterMicros);
}

void sendFlash(int idx) {
  char pkt[16];
  snprintf(pkt, sizeof(pkt), "FLASH:%u", (unsigned)flashSeq++);
  txBeginTo(idx);
  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write(pkt, strlen(pkt) + 1);
    if (ok) break;
    delay(5);
  }
  txEnd();
  Serial.print("FLASH "); Serial.print((char*)slaveAddrs[idx]);
  Serial.println(ok ? " OK" : " FAIL");
}

bool pollReadyAckBinary(int idx, uint16_t expectSeq) {
  // Send a tiny ping (string is fine) to solicit ACK payload
  char ping[16];
  snprintf(ping, sizeof(ping), "PING:%u", (unsigned)expectSeq);

  txBeginTo(idx);
  bool ok = radio.write(ping, strlen(ping) + 1);
  bool ready = false;

  if (ok && radio.isAckPayloadAvailable()) {
    ReadyAckV1 ack{};
    radio.read(&ack, sizeof(ack));
    if (ack.type == MSG_READY_V1 && ack.seq == expectSeq) ready = true;
  }
  txEnd();
  return ready;
}

// ───────────── Binary START: console → packet per device ─────────────
// Accepts your existing console format per device: 00{r,o,g,f}@18;01{...};...
// r/o/g/f may be decimal strings like "1.1,3.2,5.3,7.4"
void handleMultiStartBinary(const char *input) {
  unsigned long masterStartTime = micros() + startDelaySec * 1000000UL;
  Serial.print("Master Start Time: "); Serial.println(masterStartTime);

  char buf[160];
  strncpy(buf, input, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';

  unsigned long earliestGreenTime = 0xFFFFFFFFUL;

  char *saveptr1;
  char *entry = strtok_r(buf, ";", &saveptr1);
  while (entry) {
    while (*entry==' '||*entry=='\t') entry++;

    char *openBrace  = strchr(entry, '{');
    char *closeBrace = strchr(entry, '}');
    if (!openBrace || !closeBrace || closeBrace < openBrace) {
      Serial.println("Invalid entry format");
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    char idStr[3] = { entry[0], entry[1], '\0' };
    int  id = atoi(idStr);
    if (id < 0 || id >= NUM_SLAVES) {
      Serial.print("Invalid ID: "); Serial.println(idStr);
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    // Extract r,o,g,f as decimals
    char rawSteps[64];
    int  rawLen = closeBrace - openBrace - 1;
    if (rawLen < 0) rawLen = 0;
    if ((size_t)rawLen >= sizeof(rawSteps)) rawLen = sizeof(rawSteps) - 1;
    strncpy(rawSteps, openBrace + 1, rawLen);
    rawSteps[rawLen] = '\0';

    double tSec[4] = {0,0,0,0};
    uint8_t steps = 0;
    {
      char tmp[64]; strncpy(tmp, rawSteps, sizeof(tmp)); tmp[sizeof(tmp)-1]='\0';
      char *sp2; char *tok = strtok_r(tmp, ",", &sp2);
      while (tok && steps < 4) {
        tSec[steps++] = atof(tok);
        tok = strtok_r(NULL, ",", &sp2);
      }
    }
    if (steps < 3) { Serial.println("Need at least red,orange,green"); entry = strtok_r(NULL, ";", &saveptr1); continue; }

    // Optional @vol
    uint8_t volForThis = deviceVolume;
    char *afterBrace = closeBrace + 1;
    while (*afterBrace==' '||*afterBrace=='\t') afterBrace++;
    if (*afterBrace == '@') {
      int v = atoi(afterBrace + 1);
      if (v < 0) v = 0; if (v > 30) v = 30;
      volForThis = (uint8_t)v;
    }

    // Convert to deciseconds (0.1 s), HALF_UP
    StartPacketV1 pkt{};
    pkt.type        = MSG_START_V1;
    pkt.seq         = seqId;
    pkt.masterStart = masterStartTime;
    pkt.volume      = volForThis;
    pkt.steps       = steps;
    for (uint8_t i=0;i<steps;i++) {
      uint16_t ds = (uint16_t) (tSec[i] * 10.0 + 0.5); // round to nearest 0.1
      pkt.t_ds[i] = ds;
    }

    // Track earliest green for host "STARTTIMER"
    unsigned long gtime = masterStartTime + (unsigned long)(tSec[2] * 1000000.0);
    if (gtime < earliestGreenTime) earliestGreenTime = gtime;

    // Send to device
    txBeginTo(id);
    bool ok = radio.write(&pkt, sizeof(pkt));
    txEnd();

    Serial.print("START_BIN "); Serial.print((char*)slaveAddrs[id]);
    Serial.print(ok ? " TXOK " : " TXFAIL ");
    Serial.print(" bytes="); Serial.print(sizeof(pkt));
    Serial.print(" seq="); Serial.print(pkt.seq);
    Serial.print(" vol="); Serial.print(pkt.volume);
    Serial.print(" tds=[");
    for (uint8_t i=0;i<steps;i++){ Serial.print(pkt.t_ds[i]); if(i+1<steps)Serial.print(','); }
    Serial.println("]");

    bool ready = false;
    if (ok) {
      for (int attempt=0; attempt<3 && !ready; ++attempt) {
        delay(15 + (id * 7 + attempt * 11) % 20);
        ready = pollReadyAckBinary(id, pkt.seq);
      }
    }
    Serial.print("READY "); Serial.print((char*)slaveAddrs[id]);
    Serial.println(ready ? " YES" : " NO");

    entry = strtok_r(NULL, ";", &saveptr1);
  }

  seqId++;

  if (earliestGreenTime != 0xFFFFFFFFUL) {
    while ((long)(micros() - earliestGreenTime) < 0) { delay(1); }
    Serial.println("STARTTIMER");
  }
}
