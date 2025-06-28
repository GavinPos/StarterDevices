#include <SPI.h>
#include <RF24.h>

// ─── GLOBALS ────────────────────────────────────────────────────────────
RF24    radio(9, 10);                // CE, CSN
const byte broadcastPipe[6] = "BCAST";
const byte commandPipe[6]  = "CMDCH";

const int MAX_DEVICES = 99;
char  discoveredIDs[MAX_DEVICES][3];  // store as 2-char strings + null
int   deviceCount = 0;

// ─── MESSAGE ID COUNTER ─────────────────────────────────────────────────
uint8_t nextMsgId = 0;  // will roll 00→99

// ─── FORWARD DECLARATIONS ───────────────────────────────────────────────
void performDiscovery();
void sendCommand(const char *deviceList, char cmd);

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

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    if (c == '\n' || idx >= sizeof(buf) - 1) {
      buf[idx] = '\0';
      idx = 0;

      // — List devices that are already registered —
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
      // — Broadcast color test, sends commands to devices to cycle their lights —
      else if (strcasecmp(buf, "broadcast") == 0) {
        // cycle commands 3→2→1→2→3→0
        sendCommand("ALL", '3'); delay(250);
        sendCommand("ALL", '2'); delay(250);
        sendCommand("ALL", '1'); delay(250);
        sendCommand("ALL", '2'); delay(250);
        sendCommand("ALL", '3'); delay(250);
        sendCommand("ALL", '0'); delay(250);

        Serial.println("Broadcast sequence complete.");
      }
      // — <devicelist> <cmd> —
      else {
        char deviceList[27] = {0};
        char cmdChar[2]     = {0};

        if (sscanf(buf, "%26s %1s", deviceList, cmdChar) == 2) {
          Serial.print("Command Sent: ");
          radio.openWritingPipe(commandPipe);
          radio.stopListening();
          sendCommand(deviceList, cmdChar[0]);
          Serial.println("OK");

		// upon a RESET 'R' request, the reset has been sent to the devices to 
		// switch from command mode to broadcast mode, we then automatically 
		// send a who request on the broadcast channel for all to respond to
		// in a staggered manner
          if (cmdChar[0] == 'R') { // for RESET
            delay(200);
            performDiscovery();
          }
        } else {
          Serial.println("❌ Invalid input. Use: <2-char IDs…> <cmd>");
        }
      }
    }
    else {
      buf[idx++] = c;
    }
  }
}

// ─── SENDER ─────────────────────────────────────────────────────────────
// Message: <2-digit MsgID>|<2-charID×n…>|<cmdChar>
void sendCommand(const char *deviceList, char cmd) {
  nextMsgId = (nextMsgId + 1) % 100;

  char fullMsg[80];
  char idStr[3];
  snprintf(idStr, sizeof(idStr), "%02u", nextMsgId);
  snprintf(fullMsg, sizeof(fullMsg), "%s|%s|%c", idStr, deviceList, cmd);

  for (int i = 0; i < 3; i++) {
    radio.write(fullMsg, strlen(fullMsg) + 1, false);
    Serial.print("Full Command Sent: ");
    Serial.println(fullMsg);
    radio.txStandBy();
    delay(5);
  }
}

// ─── DISCOVERY ──────────────────────────────────────────────────────────
void performDiscovery() {
	// all devices should now be on the broadcast channel, listening
  radio.openWritingPipe(broadcastPipe);
  radio.stopListening();

  // ask all radios to respond with their 2-char ID
  sendCommand("ALL", 'W'); // interpret 'W' for WHO
  radio.txStandBy();
  Serial.println("OK");

  delay(10);
  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();

  deviceCount = 0;
  unsigned long start = millis();
  // waiting for 5 seconds for all channels to report in, each device has a 
  // 500 millisecond window to reply within
  while (millis() - start < 5000) {
    if (radio.available()) {
      char id[3] = {0};
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
        deviceCount++;
      }
    }
  }

  radio.stopListening();
  Serial.println("Discovery complete.");
  if (deviceCount == 0) {
    Serial.println("No Devices Registered");
  } else {
    for (int i = 0; i < deviceCount; i++) {
      Serial.print("Device: ");
      Serial.println(discoveredIDs[i]);
    }
  }
}
