#include <SPI.h>
#include <RF24.h>
#include <string.h>    // for strtok, strcmp, strncpy, etc.

// ─── GLOBALS ────────────────────────────────────────────────────────────
RF24 radio(9, 10);               // CE, CSN

const byte broadcastPipe[6] = "BCAST";
const byte commandPipe[6]   = "CMDCH";
const char myID[]           = "01";  // 🟡 Numeric-only ID (2 chars)

// LED pins
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

bool listeningToCommandPipe = false;

#define MSG_HISTORY_SIZE 20
char msgHistory[MSG_HISTORY_SIZE][3];  // 20 slots of "XY\0"
int  historyIndex = 0;                 // next slot to overwrite

// ─── FORWARD DECLARATIONS ───────────────────────────────────────────────
bool seenBefore(const char *id);
void recordMsgId(const char *id);
void allLedsOff();
void blinkLedFor(const char* color, uint16_t durationMs, uint8_t blinkCount);
bool idInList(const char *list, const char *id);

// ─── SETUP ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // zero-clear our history buffer
  memset(msgHistory, 0, sizeof(msgHistory));

  // Radio init
  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_250KBPS);
  radio.enableDynamicPayloads();

  // Start out listening on the broadcast pipe
  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();

  // LED pins
  pinMode(RED_LED,    OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED,  OUTPUT);
  allLedsOff();

  // Flash all lights once, then flash green LED at startup
  digitalWrite(RED_LED, HIGH);
  digitalWrite(ORANGE_LED, HIGH);
  digitalWrite(GREEN_LED, HIGH);
  delay(500);
  allLedsOff();

  int idNum = atoi(myID);
  const uint16_t blinkDelay = 200;  // ms on/off per blink
  for (int i = 0; i < idNum; i++) {
    digitalWrite(GREEN_LED, HIGH);
    delay(blinkDelay);
    digitalWrite(GREEN_LED, LOW);
    delay(blinkDelay);
  }
}

// ─── MAIN LOOP ──────────────────────────────────────────────────────────
void loop() {
  if (!radio.available()) return;

  char raw[32] = {0};
  radio.read(raw, sizeof(raw));

  char buf[32];
  strncpy(buf, raw, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';

  // Parse new format: msgId|deviceList|cmdChar
  char *msgId      = strtok(buf, "|");
  char *deviceList = strtok(nullptr, "|");
  char *cmdField   = strtok(nullptr, "|");
  if (!msgId || !deviceList || !cmdField) return;

  // duplicate ID? skip
  if (seenBefore(msgId)) return;
  recordMsgId(msgId);

  char cmdChar = cmdField[0];

  // — WHO handler —  (deviceList == ALL, cmdChar == 'W')
  if (!listeningToCommandPipe && strcmp(deviceList, "ALL") == 0 && cmdChar == 'W') {
    // blink all LEDs to show alive
    blinkLedFor("ALL", 200, 1);

	// calculate response timeslot: each device has a 500ms window
    int idNum = atoi(myID);
    unsigned long slotDelay = (unsigned long)(idNum - 1) * 500UL;
    delay(slotDelay);

    // send our ID in our slot, with retries for reliability
	// transmitter code will de-dup any additional responses
    radio.stopListening();
    radio.openWritingPipe(broadcastPipe);
    for (int i = 0; i < 3; i++) {
      radio.write(myID, strlen(myID) + 1, false);
      radio.txStandBy();
      delay(10);
    }

    radio.openReadingPipe(1, commandPipe);
    radio.startListening();
    listeningToCommandPipe = true;
    return;
  }

  // — Commands for us — (deviceList contains 'ALL' or includes our myID)
  if (strcmp(deviceList, "ALL") == 0 || idInList(deviceList, myID)) {
    allLedsOff();
    if (cmdChar == 'R') {  // RESET
      radio.stopListening();
      radio.openReadingPipe(1, broadcastPipe);
      radio.startListening();
      listeningToCommandPipe = false;
      allLedsOff();
    }
    else if (cmdChar == '0')    allLedsOff();
    else if (cmdChar == '1')    digitalWrite(RED_LED,    HIGH);
    else if (cmdChar == '2')    digitalWrite(ORANGE_LED, HIGH);
    else if (cmdChar == '3')    digitalWrite(GREEN_LED,  HIGH);
  }
}

// ─── MESSAGE HISTORY ────────────────────────────────────────────────────
bool seenBefore(const char *id) {
  for (int i = 0; i < MSG_HISTORY_SIZE; i++) if (strcmp(msgHistory[i], id) == 0) return true;
  return false;
}

void recordMsgId(const char *id) {
  strncpy(msgHistory[historyIndex], id, 2);
  msgHistory[historyIndex][2] = '\0';
  historyIndex = (historyIndex + 1) % MSG_HISTORY_SIZE;
}

// ─── LED HELPERS ─────────────────────────────────────────────────────────
void allLedsOff() {
  digitalWrite(RED_LED,    LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(GREEN_LED,  LOW);
}

void blinkLedFor(const char* color, uint16_t durationMs, uint8_t blinkCount) {
  if (blinkCount == 0) return;
  for (uint8_t i = 0; i < blinkCount; i++) {
    if      (strcmp(color, "RED")    == 0) digitalWrite(RED_LED,    HIGH);
    else if (strcmp(color, "ORANGE") == 0) digitalWrite(ORANGE_LED, HIGH);
    else if (strcmp(color, "GREEN")  == 0) digitalWrite(GREEN_LED,  HIGH);
    else if (strcmp(color, "ALL")    == 0) {
      digitalWrite(RED_LED,    HIGH);
      digitalWrite(ORANGE_LED, HIGH);
      digitalWrite(GREEN_LED,  HIGH);
    }
    delay(durationMs / (blinkCount * 2));
    allLedsOff();
    delay(durationMs / (blinkCount * 2));
  }
}

// ─── NEW HELPER: Check if a 2-character ID is in the concatenated list ───
bool idInList(const char *list, const char *id) {
  size_t len = strlen(list);
  for (size_t i = 0; i + 1 < len; i += 2) {
    if (strncmp(&list[i], id, 2) == 0) return true;
  }
  return false;
}
