#include "LowPower.h"

constexpr bool debug = false;
void setup() {
  pinMode(7, INPUT);
  pinMode(3, INPUT);
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);  // Switch OFF (high) relay
  if(debug) Serial.begin(115200);
}

static constexpr unsigned int ONTIME = 499; // 60 seconds
static constexpr unsigned int INITIAL_SETTLE_TIME = 50; // ~7 seconds
static unsigned int camOnTime = INITIAL_SETTLE_TIME;
static bool camOn = false;
static int  ledCount = 0;

void loop() {
  bool detect0 = digitalRead(7);
  bool detect1 = digitalRead(3);
  if(debug) {
    if(detect0) Serial.print("detect0"); else Serial.print("       "); 
    if(detect1) Serial.print(" detect1"); else Serial.print("        ");
    if(camOn) Serial.print(" camOn "); else Serial.print("       ");
    Serial.println(camOnTime);
  }
  bool haveMotion = detect0 || detect1;
  if (haveMotion) {
    camOnTime = ONTIME;
  } else {
    if(camOnTime > 0) {
      camOnTime = camOnTime - 1u;
    }
  }
  if(camOnTime == 0 && camOn) {
    camOn = false;
    digitalWrite(4, HIGH);  // Switch off
  } else if(camOnTime == ONTIME && !camOn) {
    camOn = true;
    digitalWrite(4, LOW);  // Switch on    
  }
  digitalWrite(13, ledCount == 0);  // Switch on relay
  if(++ledCount == 8) {
    ledCount = 0;
  }
  if(debug) {  
    delay(120);
  } else {
    LowPower.powerDown(SLEEP_120MS, ADC_OFF, BOD_ON);
  }
}
