#include <SPI.h>
#include <RF24.h>

RF24 radio(9, 10);  // CE, CSN

static const byte slaveAddrs[][6] = { "DEV00", "DEV01", "DEV02", "DEV03", "DEV04", "DEV05", "DEV06", "DEV07" };
const int NUM_SLAVES = 2;

unsigned long startDelaySec = 2;

// Default volume (0..30) if a device block omits @<vol>
uint8_t deviceVolume = 18;

void setup() {
  Serial.begin(115200);
  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(true);
  radio.setRetries(5, 15);
  radio.setDataRate(RF24_250KBPS);
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

void checkSlave(int idx) {
  radio.openWritingPipe(slaveAddrs[idx]);
  radio.stopListening();
  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write("CHECK", 5);
    if (ok) break;
    delay(10);
  }
  radio.startListening();

  Serial.print("CHECK "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " ACKed" : " FAILED");
  Serial.println();
}

void sendSync(int idx) {
  unsigned long masterMicros = micros();
  char buf[32];
  snprintf(buf, sizeof(buf), "SYNC:%lu", masterMicros);

  radio.openWritingPipe(slaveAddrs[idx]);
  radio.stopListening();
  bool ok = radio.write(buf, strlen(buf) + 1);
  radio.startListening();

  Serial.print("SYNC  "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " OK" : " FAIL");
  Serial.print(" @"); Serial.println(masterMicros);
}

// UPDATED: now includes per-device volume
void sendStartSequence(int idx, const char *stepList, unsigned long masterStartTime, uint8_t vol) {
  sendSync(idx);

  char buf[128] = "ST:";                            // ST:<t0>|<t1>|...:<masterStart>:<vol>
  strncat(buf, stepList, sizeof(buf) - strlen(buf) - 1);

  char timeStr[32];
  snprintf(timeStr, sizeof(timeStr), ":%lu", masterStartTime);
  strncat(buf, timeStr, sizeof(buf) - strlen(buf) - 1);

  char volStr[8];
  if (vol > 30) vol = 30;
  snprintf(volStr, sizeof(volStr), ":%u", (unsigned)vol);
  strncat(buf, volStr, sizeof(buf) - strlen(buf) - 1);

  radio.openWritingPipe(slaveAddrs[idx]);
  radio.stopListening();
  bool ok = radio.write(buf, strlen(buf) + 1);
  radio.startListening();

  Serial.print("START "); Serial.print((char*)slaveAddrs[idx]);
  Serial.print(ok ? " OK " : " FAIL ");
  Serial.print("Payload='"); Serial.print(buf); Serial.println("'");
}

void sendFlash(int idx) {
  radio.openWritingPipe(slaveAddrs[idx]);
  radio.stopListening();
  bool ok = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ok = radio.write("FLASH:", 6);
    if (ok) break;
    delay(10);
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
    // trim leading spaces
    while (*entry == ' ') entry++;

    // find the {…} region
    char *openBrace  = strchr(entry, '{');
    char *closeBrace = strchr(entry, '}');
    if (!openBrace || !closeBrace || closeBrace < openBrace) {
      Serial.println("Invalid entry format");
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    // parse the two-digit ID
    char idStr[3] = { entry[0], entry[1], '\0' };
    int  id       = atoi(idStr);
    if (id < 0 || id >= NUM_SLAVES) {
      Serial.print("Invalid ID: "); Serial.println(idStr);
      entry = strtok_r(NULL, ";", &saveptr1);
      continue;
    }

    // copy out exactly what's between the braces as raw steps (comma-separated)
    char rawSteps[64];
    int  rawLen = closeBrace - openBrace - 1;
    strncpy(rawSteps, openBrace + 1, rawLen);
    rawSteps[rawLen] = '\0';

    // build the “pipe”-separated list for RF
    char pipeSteps[64];
    strncpy(pipeSteps, rawSteps, sizeof(pipeSteps));
    pipeSteps[sizeof(pipeSteps)-1] = '\0';
    for (int i = 0; pipeSteps[i]; i++) {
      if (pipeSteps[i] == ',') pipeSteps[i] = '|';
    }

    // NEW: parse optional "@<vol>" after the closing brace
    uint8_t volForThis = deviceVolume;
    char *afterBrace = closeBrace + 1;
    while (*afterBrace == ' ' || *afterBrace == '\t') afterBrace++;
    if (*afterBrace == '@') {
      int v = atoi(afterBrace + 1);
      if (v < 0) v = 0; if (v > 30) v = 30;
      volForThis = (uint8_t)v;
    }

    // ─── inner split on “,” to find the 3rd (green) value
    char rawCopy[64];
    strncpy(rawCopy, rawSteps, sizeof(rawCopy));
    rawCopy[sizeof(rawCopy)-1] = '\0';
    char *saveptr2;
    char *tok = strtok_r(rawCopy, ",", &saveptr2);
    int   idxTok = 0;
    int   greenOffset = -1;
    while (tok) {
      if (idxTok == 2) { greenOffset = atoi(tok); break; }
      tok = strtok_r(NULL, ",", &saveptr2);
      idxTok++;
    }

    // schedule the green time, track earliest
    if (greenOffset >= 0) {
      unsigned long gtime = masterStartTime + (unsigned long)greenOffset * 1000000UL;
      if (gtime < earliestGreenTime) earliestGreenTime = gtime;
    }

    // send the START packet to this slave (now includes per-device volume)
    sendStartSequence(id, pipeSteps, masterStartTime, volForThis);
    delay(10);

    entry = strtok_r(NULL, ";", &saveptr1);
  }

  // ─── finally, wait and fire the very first green light
  if (earliestGreenTime != 0xFFFFFFFFUL) {
    while (micros() < earliestGreenTime) { delay(1); }
    Serial.println("STARTTIMER");
  }
}
