#include <SPI.h>
#include <RF24.h>
#include <string.h>    // for strtok, strcmp, strncpy, etc.

// ─── GLOBALS ────────────────────────────────────────────────────────────
RF24 radio(9, 10);               // CE, CSN

const byte broadcastPipe[6] = "BCAST";
const byte commandPipe[6]   = "CMDCH";
const char  myID[]          = "01";  // 🟡 Numeric-only ID

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
void handleSequenceCommand(const char* cmdText);

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

  // Start out listening on the discovery pipe
  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();

  // LED pins
  pinMode(RED_LED,    OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED,  OUTPUT);
  allLedsOff();
  
  delay(500);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(ORANGE_LED, HIGH);
  digitalWrite(GREEN_LED, HIGH);
  delay(200);
  allLedsOff();
}

// ─── MAIN LOOP ──────────────────────────────────────────────────────────
void loop() {
  if (!radio.available()) {
    return;
  }

  char raw[32] = {0};
  radio.read(raw, sizeof(raw));

  // Copy to a buffer we can strtok()
  char buf[32];
  strncpy(buf, raw, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';

  // Parse "ID|TO:xxx|CMD:yyy"
  char *msgId    = strtok(buf, "|");
  char *toField  = strtok(nullptr, "|");
  char *cmdField = strtok(nullptr, "|");

  if (!msgId || !toField || !cmdField) {
    // malformed packet → skip
    return;
  }

  // duplicate ID? skip
  if (seenBefore(msgId)) {
    return;
  }
  recordMsgId(msgId);

  // strip prefixes
  char *toValue  = (strncmp(toField,  "TO:",  3) == 0) ? toField  + 3 : toField;
  char *cmdValue = (strncmp(cmdField, "CMD:", 4) == 0) ? cmdField + 4 : cmdField;

  // — WHO handler —
  if ( strcmp(toValue,  "ALL") == 0
    && strcmp(cmdValue, "WHO") == 0
    && !listeningToCommandPipe)
  {
    // blink all LEDs to show we're alive
    int idNum = atoi(myID);
    blinkLedFor("ALL", 500 * idNum, 3);

    // reply on the broadcast pipe with our ID
    radio.stopListening();
    radio.openWritingPipe(broadcastPipe);
    radio.write(myID, strlen(myID) + 1);
    radio.txStandBy();

    // now switch to command channel
    radio.openReadingPipe(1, commandPipe);
    radio.startListening();
    listeningToCommandPipe = true;
    return;
  }

  // — Commands for us (or broadcast) —
  if ( strcmp(toValue, "ALL") == 0
    || strcmp(toValue, myID) == 0 )
  {
    allLedsOff();

    if (strcmp(cmdValue, "RESET") == 0) {
      // go back to listening on broadcast
      radio.stopListening();
      radio.openReadingPipe(1, broadcastPipe);
      radio.startListening();
      listeningToCommandPipe = false;
      allLedsOff();
    }
    else if (strncmp(cmdValue, "SEQ[", 4) == 0) {
      handleSequenceCommand(cmdValue);
    }
    else if (cmdValue[0] == '0') {
      allLedsOff();
    }
    else if (cmdValue[0] == '1') {
      digitalWrite(RED_LED, HIGH);
    }
    else if (cmdValue[0] == '2') {
      digitalWrite(ORANGE_LED, HIGH);
    }
    else if (cmdValue[0] == '3') {
      digitalWrite(GREEN_LED, HIGH);
    }
  }
}

// ─── MESSAGE HISTORY ────────────────────────────────────────────────────
bool seenBefore(const char *id) {
  for (int i = 0; i < MSG_HISTORY_SIZE; i++) {
    if (strcmp(msgHistory[i], id) == 0) {
      return true;
    }
  }
  return false;
}

void recordMsgId(const char *id) {
  // store the two-char ID + nul
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
  uint16_t halfPeriod = durationMs / (blinkCount * 2);
  for (uint8_t i = 0; i < blinkCount; i++) {
    if      (strcmp(color, "RED")    == 0) digitalWrite(RED_LED,    HIGH);
    else if (strcmp(color, "ORANGE") == 0) digitalWrite(ORANGE_LED, HIGH);
    else if (strcmp(color, "GREEN")  == 0) digitalWrite(GREEN_LED,  HIGH);
    else if (strcmp(color, "ALL")    == 0) {
      digitalWrite(RED_LED, HIGH);
      digitalWrite(ORANGE_LED, HIGH);
      digitalWrite(GREEN_LED, HIGH);
    }
    delay(halfPeriod);
    allLedsOff();
    delay(halfPeriod);
  }
}

// ─── SEQUENCE PARSER ─────────────────────────────────────────────────────
void handleSequenceCommand(const char* commandText) {
  float tR, tO, tG;
  if (sscanf(commandText, "SEQ[%f,%f,%f]", &tR, &tO, &tG) == 3) {
    digitalWrite(RED_LED, HIGH);
    delay((unsigned long)(tR * 1000));
    allLedsOff();

    digitalWrite(ORANGE_LED, HIGH);
    delay((unsigned long)(tO * 1000));
    allLedsOff();

    digitalWrite(GREEN_LED, HIGH);
    delay((unsigned long)(tG * 1000));
    allLedsOff();
  }
}
