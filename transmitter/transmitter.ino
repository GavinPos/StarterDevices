#include <SPI.h>
#include <RF24.h>

RF24 radio(9, 10);  // CE, CSN

static const byte slaveAddrs[][6] = { "DEV00","DEV01","DEV02","DEV03","DEV04","DEV05","DEV06","DEV07" };
const int NUM_SLAVES = 8;

unsigned long startDelaySec = 2;

// Default volume (0..30) if a device block omits @<vol>
uint8_t deviceVolume = 20;

// New: sequence id (advance once per START batch)
static uint16_t seqId = 1;
static uint8_t flashSeq = 0;

void setup() {
  Serial.begin(115200);
  radio.begin();

  // ---- Robust RF config ----
  radio.setPALevel(RF24_PA_MAX);         // use RF24_PA_LOW for close bench tests
  radio.setAutoAck(true);
  radio.setRetries(5, 15);               // 5 retries, 15*250us backoff
  radio.setDataRate(RF24_250KBPS);       // best sensitivity
  radio.setChannel(108);                 // ~2.508 GHz (less Wi-Fi overlap)
  radio.setCRCLength(RF24_CRC_16);
  radio.enableDynamicPayloads();
  radio.enableAckPayload();

  // Start in RX by default (TX switches per send)
  radio.openReadingPipe(0, slaveAddrs[0]); // not used for TX, but keeps state sane
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
      }
      else if (strcasecmp(buf, "SYNC") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendSync(i); delay(10); }
      }
      else if (strcasecmp(buf, "FLASH") == 0) {
        for (int i = 0; i < NUM_SLAVES; i++) { sendFlash(i); delay(10); }
      }
      else if (strncasecmp(buf, "START:", 6) == 0) {
        handleMultiStart(buf + 6);
      }
      else if (strncasecmp(buf, "VOLUME:", 7) == 0) {
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

// ───────────────── helpers ─────────────────

void txBeginTo(int idx) {
  radio.stopListening();
  radio.openWritingPipe(slaveAddrs[idx]);
  radio.flush_tx();
}

void txEnd() {
  radio.startListening();
}

void checkSlave(int idx) {
  txBeginTo(idx);
  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write("CHECK", 6); // include NUL doesn't matter here
    if (ok) break;
    delay(10);
  }
  txEnd();

  Serial.print("CHECK "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " ACKed" : " FAILED");
  Serial.println();
}

void sendSync(int idx) {
  unsigned long masterMicros = micros();
  char buf[32];
  snprintf(buf, sizeof(buf), "SYNC:%lu", masterMicros);

  txBeginTo(idx);
  bool ok = radio.write(buf, strlen(buf) + 1);
  txEnd();

  Serial.print("SYNC  "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " OK" : " FAIL");
  Serial.print(" @"); Serial.println(masterMicros);
}

bool pollReadyAck(int idx, uint16_t expectSeq) {
  // Send a tiny ping to solicit the preloaded ACK payload from RX
  char ping[16];
  snprintf(ping, sizeof(ping), "PING:%u", (unsigned)expectSeq);

  txBeginTo(idx);
  bool ok = radio.write(ping, strlen(ping) + 1);
  bool ready = false;

  if (ok && radio.isAckPayloadAvailable()) {
    char ack[32] = {0};
    radio.read(&ack, sizeof(ack));
    unsigned ackSeq = 0;
    if (sscanf(ack, "RDY:%u", &ackSeq) == 1 && ackSeq == expectSeq) {
      ready = true;
    }
  }
  txEnd();
  return ready;
}

// UPDATED: now includes seq + per-device volume and READY polling
void sendStartSequence(int idx, const char *stepList, unsigned long masterStartTime, uint8_t vol) {
  sendSync(idx);
  // Build ST2 packet: ST2:<seq>|<t0|t1|...>:<masterStart>:<vol>
  char buf[128] = "ST2:";

  char seqStr[12];
  snprintf(seqStr, sizeof(seqStr), "%u|", (unsigned)seqId);
  strncat(buf, seqStr, sizeof(buf)-strlen(buf)-1);

  strncat(buf, stepList, sizeof(buf) - strlen(buf) - 1);

  char tail[48];
  if (vol > 30) vol = 30;
  snprintf(tail, sizeof(tail), ":%lu:%u", masterStartTime, (unsigned)vol);
  strncat(buf, tail, sizeof(buf) - strlen(buf) - 1);

  // Send ST2
  txBeginTo(idx);
  bool ok = radio.write(buf, strlen(buf) + 1);
  txEnd();
 
  Serial.print("START "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " TXOK " : " TXFAIL ");
  Serial.print("Payload='"); Serial.print(buf); Serial.println("'");

  // If ST2 went through, immediately poll for READY via ack payload
  bool ready = false;
  if (ok) {
    // try up to 3 polls with small jittered backoff
    for (int attempt = 0; attempt < 3 && !ready; ++attempt) {
      delay(15 + (idx * 7 + attempt * 11) % 20);
      ready = pollReadyAck(idx, seqId);
    }
  }
  Serial.print("READY "); Serial.print((char*)slaveAddrs[idx]);
  Serial.println(ready ? " YES" : " NO");
}

void sendFlash(int idx) {
  char pkt[16];
  snprintf(pkt, sizeof(pkt), "FLASH:%u", (unsigned)flashSeq++);  // "FLASH:0", "FLASH:1", ...

  radio.openWritingPipe(slaveAddrs[idx]);
  radio.stopListening();
  radio.flush_tx();

  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write(pkt, strlen(pkt) + 1);
    if (ok) break;
    delay(5);
  }
  radio.startListening();

  Serial.print("FLASH "); Serial.print((char*)slaveAddrs[idx]);
  Serial.println(ok ? " OK" : " FAIL");
}

// Handles START:00{0,2,5}@18;01{1,4,6}@22;...
void handleMultiStart(const char *input) {
  // compute when “time zero” is (master time + delay)
  unsigned long masterStartTime = micros() + startDelaySec * 1000000UL;
  Serial.print("Master Start Time: "); Serial.println(masterStartTime);

  // make a mutable copy of the command string
  char buf[160];
  strncpy(buf, input, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';

  unsigned long earliestGreenTime = 0xFFFFFFFFUL;

  // ─── outer split on “;”
  char *saveptr1;
  char *entry = strtok_r(buf, ";", &saveptr1);
  while (entry) {
    while (*entry == ' ') entry++;

    char *openBrace  = strchr(entry, '{');
    char *closeBrace = strchr(entry, '}');
    if (!openBrace || !closeBrace || closeBrace < openBrace) {
      Serial.println("Invalid entry format");
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    char idStr[3] = { entry[0], entry[1], '\0' };
    int  id       = atoi(idStr);
    if (id < 0 || id >= NUM_SLAVES) {
      Serial.print("Invalid ID: "); Serial.println(idStr);
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    // copy steps between braces as raw (comma-separated)
    char rawSteps[64];
    int  rawLen = closeBrace - openBrace - 1;
    strncpy(rawSteps, openBrace + 1, rawLen);
    rawSteps[rawLen] = '\0';

    // Convert to pipe-separated
    char pipeSteps[64];
    strncpy(pipeSteps, rawSteps, sizeof(pipeSteps));
    pipeSteps[sizeof(pipeSteps)-1] = '\0';
    for (int i = 0; pipeSteps[i]; i++) {
      if (pipeSteps[i] == ',') pipeSteps[i] = '|';
    }

    // optional "@vol"
    uint8_t volForThis = deviceVolume;
    char *afterBrace = closeBrace + 1;
    while (*afterBrace == ' ' || *afterBrace == '\t') afterBrace++;
    if (*afterBrace == '@') {
      int v = atoi(afterBrace + 1);
      if (v < 0) v = 0; if (v > 30) v = 30;
      volForThis = (uint8_t)v;
    }

    // find the 3rd (green) value to compute earliest green time
    char rawCopy[64];
    strncpy(rawCopy, rawSteps, sizeof(rawCopy)); rawCopy[sizeof(rawCopy)-1] = '\0';
    char *saveptr2; char *tok = strtok_r(rawCopy, ",", &saveptr2);
    int idxTok = 0, greenOffset = -1;
    while (tok) {
      if (idxTok == 2) { greenOffset = atoi(tok); break; }
      tok = strtok_r(NULL, ",", &saveptr2); idxTok++;
    }
    if (greenOffset >= 0) {
      unsigned long gtime = masterStartTime + (unsigned long)greenOffset * 1000000UL;
      if (gtime < earliestGreenTime) earliestGreenTime = gtime;
    }

    delay(5);
    sendStartSequence(id, pipeSteps, masterStartTime, volForThis);
    delay(10);

    entry = strtok_r(NULL, ";", &saveptr1);
  }

  // bump sequence id once per batch
  seqId++;

  // fire the very first green marker for host app
  if (earliestGreenTime != 0xFFFFFFFFUL) {
    while ((long)(micros() - earliestGreenTime) < 0) { delay(1); }
    Serial.println("STARTTIMER");
  }
}
