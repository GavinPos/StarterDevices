#include <SPI.h>
#include <RF24.h>

#define DEBUG_MODE

// ─── GLOBALS ────────────────────────────────────────────────────────────
RF24    radio(9, 10);                // CE, CSN
const byte broadcastPipe[6] = "BCAST";
const byte commandPipe  [6] = "CMDCH";

const int MAX_DEVICES = 99;
char        discoveredIDs[MAX_DEVICES][6];
int         deviceCount = 0;


// ─── FORWARD DECLARATIONS ───────────────────────────────────────────────
void performDiscovery();

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

  // Read one line from Serial into buf[]
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\r') {
      continue;
    }
    if (c == '\n' || idx >= sizeof(buf) - 1) {
      buf[idx] = '\0';
      idx = 0;


      // === handle “devices” command ===
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
      else if (strcasecmp(buf, "broadcast") == 0) {
        broadcastColorSequence();
        Serial.println("Broadcast sequence complete.");
      }
      // === handle “<ID> <CMD>” ===
      else {
        char target[16] = {0};
        char cmd[32]    = {0};
        if (sscanf(buf, "%15s %31s", target, cmd) == 2) {
          char message[64];
          snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);

          Serial.print("Command Sent: ");
          Serial.println(message);

          radio.openWritingPipe(commandPipe);

          radio.stopListening();
          radio.write(message, strlen(message) + 1, false);

          radio.txStandBy(); 
          Serial.println("OK");

          if (strcasecmp(cmd, "RESET") == 0) {
            delay(200);
            performDiscovery();
          }
        } else {
          Serial.println("❌ Invalid input. Use: <ID> <CMD>");
        }
      }
    } else {
      buf[idx++] = c;
    }
  }
}

// ─── BROADCAST COLOR SEQUENCE ────────────────────────────────────────────
void broadcastColorSequence() {
  // 1) Switch to broadcast pipe & TX mode
  radio.openWritingPipe(commandPipe);
  radio.stopListening();

  // Temporarily disable retries for “fire-and-forget”
  radio.setRetries(5, 15);

  const char *target = "ALL";
  char message[64];

  for (int cycle = 0; cycle < 3; cycle++) {
  // GREEN_ON (cmd="3")
  {
    const char *cmd = "3";
    snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
    for (int rep = 0; rep < 3; rep++) {
      radio.write(message, strlen(message) + 1, true);
      #ifdef DEBUG_MODE
        Serial.print("→ BROADCAST: "); Serial.println(message);
      #endif
      radio.txStandBy();
      delay(5);
    }
    delay(500);
  }

  // ORANGE_ON (cmd="2")
  {
    const char *cmd = "2";
    snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
    for (int rep = 0; rep < 3; rep++) {
      radio.write(message, strlen(message) + 1, true);
      #ifdef DEBUG_MODE
        Serial.print("→ BROADCAST: "); Serial.println(message);
      #endif
      radio.txStandBy();
      delay(5);
    }
    delay(500);
  }

  // RED_ON (cmd="1")
  {
    const char *cmd = "1";
    snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
    for (int rep = 0; rep < 3; rep++) {
      radio.write(message, strlen(message) + 1, true);
      #ifdef DEBUG_MODE
        Serial.print("→ BROADCAST: "); Serial.println(message);
      #endif
      radio.txStandBy();
      delay(5);
    }
    delay(500);
  }

    // ORANGE_ON (cmd="2")
  {
    const char *cmd = "2";
    snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
    for (int rep = 0; rep < 3; rep++) {
      radio.write(message, strlen(message) + 1, true);
      #ifdef DEBUG_MODE
        Serial.print("→ BROADCAST: "); Serial.println(message);
      #endif
      radio.txStandBy();
      delay(5);
    }
    delay(500);
  }
}

  // GREEN_ON (cmd="3")
  {
    const char *cmd = "3";
    snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
    for (int rep = 0; rep < 3; rep++) {
      radio.write(message, strlen(message) + 1, true);
      #ifdef DEBUG_MODE
        Serial.print("→ BROADCAST: "); Serial.println(message);
      #endif
      radio.txStandBy();
      delay(5);
    }
    delay(500);
  }
  
  const char *cmd = "0";
  snprintf(message, sizeof(message), "TO:%s|CMD:%s", target, cmd);
  for (int rep = 0; rep < 3; rep++) {
    radio.write(message, strlen(message) + 1, true);
    #ifdef DEBUG_MODE
      Serial.print("→ BROADCAST: "); Serial.println(message);
    #endif
    radio.txStandBy();
    delay(5);
  }
    
  radio.txStandBy();
  // Restore your reliable‐transmit defaults for future ACK’d commands
  radio.setRetries(5, 15);

  // (Optionally) switch back to listening mode if needed:
  // radio.openReadingPipe(1, broadcastPipe);
  // radio.startListening();
}


// ─── DISCOVERY ROUTINE ──────────────────────────────────────────────────
void performDiscovery() {
  radio.openWritingPipe(broadcastPipe);
  radio.stopListening();
  const char whoMsg[] = "WHO";
  radio.write(&whoMsg, sizeof(whoMsg), true);
  #ifdef DEBUG_MODE
    Serial.println("→ RADIO SENT");
  #endif
  radio.txStandBy(); 
  Serial.println("OK");

  delay(10);

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
