#include <SPI.h>
#include <RF24.h>

// Uncomment to enable Serial debugging
//#define DEBUG_MODE

RF24 radio(9, 10);  // CE, CSN

const byte broadcastPipe[6] = "BCAST";
const byte commandPipe[6]   = "CMDCH";
const char myID[] = "02";  // 🟡 Numeric-only ID

// LED pins
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

bool listeningToCommandPipe = false;

void setup() {
#ifdef DEBUG_MODE
  Serial.begin(115200);
#endif

  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_250KBPS);
  radio.enableDynamicPayloads();

  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();

#ifdef DEBUG_MODE
  Serial.print("Slave "); Serial.print(myID); Serial.println(" ready.");
#endif

  pinMode(RED_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  allLedsOff();
}

void loop() {
  if (radio.available()) {
    char msg[32] = {0};
    radio.read(&msg, sizeof(msg));
#ifdef DEBUG_MODE
    Serial.print("Received: "); Serial.println(msg);
#endif

    if (strcmp(msg, "WHO") == 0 && !listeningToCommandPipe) {
      radio.stopListening();

      int idNumber = atoi(myID);
      blinkLedFor("ALL", 500 * idNumber, 3);

      radio.openWritingPipe(broadcastPipe);
      radio.write(&myID, sizeof(myID));
#ifdef DEBUG_MODE
      Serial.println("Responded to WHO");
#endif

      radio.openReadingPipe(1, commandPipe);
      radio.startListening();
      listeningToCommandPipe = true;
    }

    else if (strncmp(msg, "TO:ALL", 6) == 0 || strstr(msg, myID)) {
      char* cmdPtr = strstr(msg, "CMD:");
      if (cmdPtr) {
        char* commandText = cmdPtr + 4;
#ifdef DEBUG_MODE
        Serial.print("Executing command: ");
        Serial.println(commandText);
#endif

        allLedsOff();

        if (strncmp(commandText, "RESET", strlen("RESET")) == 0) {
#ifdef DEBUG_MODE
          Serial.println("Resetting to broadcast mode...");
#endif
          radio.stopListening();
          radio.openReadingPipe(1, broadcastPipe);
          radio.startListening();
          listeningToCommandPipe = false;
          allLedsOff();
        }
        else if (strncmp(commandText, "SEQ[", 4) == 0) {
          handleSequenceCommand(commandText);
        }
        else if (commandText[0] == '0') {
          allLedsOff();
        }
        else if (commandText[0] == '1') {
          digitalWrite(RED_LED, HIGH);
        }
        else if (commandText[0] == '2') {
          digitalWrite(ORANGE_LED, HIGH);
        }
        else if (commandText[0] == '3') {
          digitalWrite(GREEN_LED, HIGH);
        }
        else {
#ifdef DEBUG_MODE
          Serial.println("Unknown command.");
#endif
        }
      }
    }
  }
}

void allLedsOff() {
  digitalWrite(RED_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
}

void blinkLedFor(const char* color, uint16_t durationMs, uint8_t blinkCount) {
  if (blinkCount == 0) return;
  uint16_t segment = durationMs / (blinkCount * 2);
  for (uint8_t i = 0; i < blinkCount; i++) {
    if (strcmp(color, "RED") == 0) digitalWrite(RED_LED, HIGH);
    else if (strcmp(color, "ORANGE") == 0) digitalWrite(ORANGE_LED, HIGH);
    else if (strcmp(color, "GREEN") == 0) digitalWrite(GREEN_LED, HIGH);
    else if (strcmp(color, "ALL") == 0) {
      digitalWrite(RED_LED, HIGH);
      digitalWrite(ORANGE_LED, HIGH);
      digitalWrite(GREEN_LED, HIGH);
    } else {
#ifdef DEBUG_MODE
      Serial.print("Unknown color: "); Serial.println(color);
#endif
      return;
    }

    delay(segment);
    allLedsOff();
    delay(segment);
  }
}

void handleSequenceCommand(const char* commandText) {
  float redTime = 0, orangeTime = 0, greenTime = 0;
  int parsed = sscanf(commandText, "SEQ[%f,%f,%f]", &redTime, &orangeTime, &greenTime);

  if (parsed == 3) {
#ifdef DEBUG_MODE
    Serial.print("SEQ parsed - RED: "); Serial.print(redTime);
    Serial.print(" ORANGE: "); Serial.print(orangeTime);
    Serial.print(" GREEN: "); Serial.println(greenTime);
#endif

    digitalWrite(RED_LED, HIGH);
    delay((unsigned long)(redTime * 1000));
    allLedsOff();

    digitalWrite(ORANGE_LED, HIGH);
    delay((unsigned long)(orangeTime * 1000));
    allLedsOff();

    digitalWrite(GREEN_LED, HIGH);
    delay((unsigned long)(greenTime * 1000));
    allLedsOff();
  } else {
#ifdef DEBUG_MODE
    Serial.println("SEQ command format invalid.");
#endif
  }
}
