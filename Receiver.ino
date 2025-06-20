#include <SPI.h>
#include <RF24.h>

RF24 radio(9, 10);  // CE, CSN

const byte broadcastPipe[6] = "BCAST";
const byte commandPipe[6]   = "CMDCH";
const char myID[] = "02";  // 🟡 Numeric-only ID

// LED pins
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

bool listeningToCommandPipe = false;

#define MSG_HISTORY_SIZE 20

// space for 20 two-char IDs + null terminator
char msgHistory[MSG_HISTORY_SIZE][3];  
int historyIndex = 0;  // next slot to overwrite

void setup() {


  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_250KBPS);
  radio.enableDynamicPayloads();

  radio.openReadingPipe(1, broadcastPipe);
  radio.startListening();


  pinMode(RED_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  allLedsOff();
}

void loop() {
  if (radio.available()) {
	char raw[32] = {0};
	radio.read(&raw, sizeof(raw));

	// Make a working copy, since strtok modifies the buffer
	char buf[32];
	strncpy(buf, raw, sizeof(buf));

	char *msgId    = strtok(buf, "|");
	char *toField  = strtok(nullptr, "|");
	char *cmdField = strtok(nullptr, "|");
  
    if (!msgId || !toField || !cmdField) {
		return;
	}
	
	if (seenBefore(msgId)) {
	  // duplicate of one of the last 20 — ignore
		return;
	}
	// first time seeing this ID → record it
	recordMsgId(msgId);
  
	char *toValue = toField;
	if (toField && strncmp(toField, "TO:", 3) == 0) {
		toValue = toField + 3;
	}

	char *cmdValue = cmdField;
	if (cmdField && strncmp(cmdField, "CMD:", 4) == 0) {
		cmdValue = cmdField + 4;
	}
 
// WHO functionality, if 'TO:ALL|CMD:WHO' is received, write device ID to BROADCAST channel then switch to COMMAND channel
	if ( strcmp(toValue,  "ALL") == 0 && strcmp(cmdValue, "WHO") == 0 && !listeningToCommandPipe){
		radio.stopListening();

		int idNumber = atoi(myID);
		blinkLedFor("ALL", 500 * idNumber, 3);

		radio.openWritingPipe(broadcastPipe);
		radio.write(&myID, sizeof(myID));

		radio.openReadingPipe(1, commandPipe);
		radio.startListening();
		listeningToCommandPipe = true;
		
    } else if ( strcmp(toValue, "ALL") == 0 || strcmp(toValue, myID)  == 0) {
		allLedsOff();

		// RESET → go back to BROADCAST channel and listen
		if (strcmp(cmdValue, "RESET") == 0) {
			radio.stopListening();
			radio.openReadingPipe(1, broadcastPipe);
			radio.startListening();
			listeningToCommandPipe = false;
			allLedsOff();
		}
		// SEQ[x,y,z] → timed sequence
		else if (strncmp(cmdValue, "SEQ[", 4) == 0) {
			handleSequenceCommand(cmdValue);
		}
		// single-digit codes
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

bool seenBefore(const char *id) {
  for (int i = 0; i < MSG_HISTORY_SIZE; i++) {
    if (strcmp(msgHistory[i], id) == 0) {
      return true;
    }
  }
  return false;
}

void recordMsgId(const char *id) {
  // copy two chars + null
  strncpy(msgHistory[historyIndex], id, 2);
  msgHistory[historyIndex][2] = '\0';

  historyIndex = (historyIndex + 1) % MSG_HISTORY_SIZE;
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
    digitalWrite(RED_LED, HIGH);
    delay((unsigned long)(redTime * 1000));
    allLedsOff();

    digitalWrite(ORANGE_LED, HIGH);
    delay((unsigned long)(orangeTime * 1000));
    allLedsOff();

    digitalWrite(GREEN_LED, HIGH);
    delay((unsigned long)(greenTime * 1000));
    allLedsOff();
  } 
}
