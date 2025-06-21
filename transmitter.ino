#include <SPI.h>
#include <RF24.h>

// ─── GLOBALS ────────────────────────────────────────────────────────────
RF24    radio(9, 10);                // CE, CSN
const byte broadcastPipe[6] = "BCAST";
const byte commandPipe  [6] = "CMDCH";

const int MAX_DEVICES = 99;
char  discoveredIDs[MAX_DEVICES][6];
int   deviceCount = 0;

// ─── MESSAGE ID COUNTER ─────────────────────────────────────────────────
uint8_t nextMsgId = 0;  // will roll 0→99

// ─── FORWARD DECLARATIONS ───────────────────────────────────────────────
void performDiscovery();
void buildCommandMessage(char *outBuf, size_t bufSize, const char *target, const char *cmd);
void sendWithId(const char *baseMsg);

// ─── SETUP ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  radio.begin();
  radio.setRetries(5, 15);
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_250KBPS);
  radio.enableDynamicPayloads();
}

// ─── MAIN LOOP ──────────────────────────────────────────────────────────
void loop() {
  static char buf[64];
  static size_t idx = 0;

// command supported:
// - devices
// - broadcast
// - <device id> <cmd>
//     commands can be 0,1,2,3,RESET,WHO,SEQ[r,o,g]
  // Read one line from Serial into buf[]
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    if (c == '\n' || idx >= sizeof(buf) - 1) {
      buf[idx] = '\0';
      idx = 0;

      // — List devices —
      if (strcasecmp(buf, "devices") == 0) {
        if (deviceCount == 0) {
          Serial.println("None Registered");
        } else {
          for (int i = 0; i < deviceCount; i++) {
            Serial.print("Device: ");
            Serial.println(discoveredIDs[i]);
          }
        }
      }
      // — Broadcast color test —
      else if (strcasecmp(buf, "broadcast") == 0) {
        broadcastColorSequence();
        Serial.println("Broadcast sequence complete.");
      }
      // — <ID> <CMD> path —
      else {
        char target[16] = {0};
        char cmd[32]    = {0};

        if (sscanf(buf, "%15s %31s", target, cmd) == 2) {
          // build the TO:…|CMD:… part
          char baseMsg[64];
          buildCommandMessage(baseMsg, sizeof(baseMsg), target, cmd);

          Serial.print("Command Sent: ");
          Serial.println(baseMsg);

          // select pipe & send with ID + 3× retry
          radio.openWritingPipe(commandPipe);
          radio.stopListening();
          sendWithId(baseMsg);

          Serial.println("OK");

          if (strcasecmp(cmd, "RESET") == 0) {
            delay(200);
            performDiscovery();
          }
        }
        else {
          Serial.println("❌ Invalid input. Use: <ID> <CMD>");
        }
      }
    }
    else {
      buf[idx++] = c;
    }
  }
}

// ─── FORMAT HELPERS ─────────────────────────────────────────────────────

// Builds "TO:<target>|CMD:<cmd>"
void buildCommandMessage(char *outBuf, size_t bufSize, const char *target, const char *cmd) {
  snprintf(outBuf, bufSize, "TO:%s|CMD:%s", target, cmd);
}

// Prepends a 2-digit ID + '|' then sends 3× with a small delay
void sendWithId(const char *baseMsg) {
  // next ID (01–99, then 00)
  nextMsgId = (nextMsgId + 1) % 100;

  char fullMsg[80];
  char idStr[3];
  snprintf(idStr, sizeof(idStr), "%02u", nextMsgId);
  snprintf(fullMsg, sizeof(fullMsg), "%s|%s", idStr, baseMsg);

  for (int i = 0; i < 3; i++) {
    radio.write(fullMsg, strlen(fullMsg) + 1, false);
    Serial.print("Full Command Sent: ");
    Serial.println(fullMsg);
    radio.txStandBy();
    delay(5);    // slight gap between repeats
  }
}

// ─── BROADCAST COLOR SEQUENCE ────────────────────────────────────────────
void broadcastColorSequence() {
  radio.openWritingPipe(commandPipe);
  radio.stopListening();

  // Temporarily keep retries at your usual settings
  radio.setRetries(1, 1);

  const char *target = "ALL";
  char baseMsg[64];

  auto cycleCmd = [&](const char *cmd) {
    buildCommandMessage(baseMsg, sizeof(baseMsg), target, cmd);
    sendWithId(baseMsg);
    delay(500);
  };

  // 3× green, orange, red, back to orange, green, then off
  cycleCmd("3");
  cycleCmd("2");
  cycleCmd("1");
  cycleCmd("2");
  cycleCmd("3");
  cycleCmd("0");

  // Restore defaults if you need to ACK subsequent commands
  radio.setRetries(5, 15);
}

// ─── DISCOVERY ROUTINE ──────────────────────────────────────────────────
void performDiscovery() {
  // switch to broadcast pipe & go to TX mode
  radio.openWritingPipe(broadcastPipe);
  radio.stopListening();

  // build "TO:ALL|WHO"
  char baseMsg[64];
  buildCommandMessage(baseMsg, sizeof(baseMsg), "ALL", "WHO");

  // send with ID prefix and 3× repeats
  sendWithId(baseMsg);

  radio.txStandBy();
  Serial.println("OK");

  // give radios a moment before listening
  delay(10);

  // listen for replies
  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();

  deviceCount = 0;
  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (radio.available()) {
      char id[6] = {0};
      radio.read(&id, sizeof(id));

      bool found = false;
      for (int i = 0; i < deviceCount; i++) {
        if (strcmp(id, discoveredIDs[i]) == 0) {
          found = true;
          break;
        }
      }
      if (!found && deviceCount < MAX_DEVICES) {
        strcpy(discoveredIDs[deviceCount], id);
        Serial.print("Device: ");
        Serial.println(id);
        deviceCount++;
      }
    }
  }

  radio.stopListening();
  Serial.println("Discovery complete.");
}
