#include <SPI.h>
#include <RF24.h>

RF24 radio(9, 10);  // CE, CSN

static const byte thisDeviceAddr[6] = "DEV00";  // 🔁 Set this per device

// LED pin definitions
const int RED_LED    = 5;
const int ORANGE_LED = 6;
const int GREEN_LED  = 7;

// Time management
unsigned long timeOffset = 0;  // masterTime - micros()
unsigned long eventMicros[4];  // trigger times
int currentStep = 0;
int totalSteps  = 0;

void setup() {
  // Setup LEDs
  pinMode(RED_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  allLedsOff();

  // Setup radio
  radio.begin();
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(true);
  radio.setDataRate(RF24_250KBPS);
  radio.openReadingPipe(0, thisDeviceAddr);
  radio.startListening();
  
  for (int i = 0; i < 3; i++) {
	  digitalWrite(RED_LED, HIGH);
	  digitalWrite(ORANGE_LED, HIGH);
	  digitalWrite(GREEN_LED, HIGH);
	  delay(250);
	  allLedsOff();
	  delay(250);
  }
}

void loop() {
  if (radio.available()) {
    char msg[32] = {0};
    radio.read(&msg, sizeof(msg));
    
	if (strcmp(msg, "CHECK") == 0) {
	  radio.stopListening();
	  char reply[16];
	  snprintf(reply, sizeof(reply), "ACK:%s", thisDeviceAddr + 3); // e.g. "ACK:03"
	  radio.write(&reply, strlen(reply) + 1);
	  radio.startListening();
	}

  else if (strncmp(msg, "SYNC:", 5) == 0) {
    unsigned long masterMicros;
    if (sscanf(msg + 5, "%lu", &masterMicros) == 1) {
      unsigned long localtime = micros();
      timeOffset = localtime - masterMicros;
    }
  }

  else if (strncmp(msg, "ST:", 3) == 0) {
    parseAndScheduleSequence(msg + 3);  // skip "ST:"
  }
	
	else if (strncmp(msg, "FLASH:", 6) == 0) {
      flashLights();
    }
  }

  runScheduledSteps();  // continuously check for trigger times
}

void parseAndScheduleSequence(const char* data) {
  float timeSec[4];
  int steps = 0;

  char temp[32];
  strncpy(temp, data, sizeof(temp));
  temp[sizeof(temp) - 1] = '\0';

  char* colon = strrchr(temp, ':');
  unsigned long masterStart = 0;
  if (colon) {
    *colon = '\0';                  // terminate the step‐list portion
    masterStart = strtoul(colon+1, NULL, 10);
  } 

  char* token = strtok(temp, "|");
  while (token && steps < 4) {
    timeSec[steps++] = atof(token);
    token = strtok(NULL, "|");
  }
 
  unsigned long masterNow = masterStart + timeOffset;

	for (int i = 0; i < steps; i++) {
	  eventMicros[i] = masterNow + (unsigned long)(timeSec[i] * 1000000UL);
	}
  totalSteps = steps; 
  currentStep = 0;
}

void runScheduledSteps() {
  if (currentStep >= totalSteps || totalSteps == 0) return;

  unsigned long now = micros();
  if (now >= eventMicros[currentStep]) {
    executeStep(currentStep);
    currentStep++;
  }
}

void executeStep(int step) {

  switch (step) {
    case 0:  // RED ON
      digitalWrite(RED_LED, HIGH);
      break;

    case 1:  // RED OFF, ORANGE ON
      digitalWrite(RED_LED, LOW);
      digitalWrite(ORANGE_LED, HIGH);
      break;

    case 2:  // ORANGE OFF, GREEN ON
      digitalWrite(ORANGE_LED, LOW);
      digitalWrite(GREEN_LED, HIGH);
      break;

    case 3:  // ALL OFF
      allLedsOff();
      break;
  }
}

void flashLights(){
	for (int i = 0; i < 3; i++) {
		digitalWrite(GREEN_LED, HIGH);
		delay(200);
		digitalWrite(GREEN_LED, LOW);
		digitalWrite(ORANGE_LED, HIGH);
		delay(200);
		digitalWrite(ORANGE_LED, LOW);
		digitalWrite(RED_LED, HIGH);
		delay(200);
		digitalWrite(RED_LED, LOW);
	}
}

void allLedsOff() {
  digitalWrite(RED_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
}
