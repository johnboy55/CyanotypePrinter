/* ============================================================
 * Printhead MCU - UART Slave (Arduino Nano)
 * - Controls 4 UV LEDs (Microsecond precision)
 * - Controls CR Touch (D3 Servo, D7 Signal)
 * - Background polls 6 TMP36 Temp Sensors
 * - NeoPixel Status Indicator on D8 (Message Queue)
 * ============================================================ */

#include <Arduino.h>
#include <Servo.h>
#include <Adafruit_NeoPixel.h>

// --- Hardware Pins (Mapped to Custom PCB) ---
const int PIN_LED[4]      = {10, 11, 12, 13};
const int PIN_TEMP[6]     = {A0, A1, A2, A3, A4, A5};
const int PIN_PROBE_SIG   = 7;  // LIMIT_SURFACE -- also the Z hard-limit switch
const int PIN_PROBE_SERVO = 3;  // Hardware PWM for CR Touch
const int PIN_NEOPIXEL    = 8;  // NEO_IN
// NEW -- confirm against actual wiring before flashing:
const int PIN_Y_LIMIT     = 4;  // Y-axis hard-limit switch (INPUT_PULLUP, active LOW)
const int PIN_ESTOP_OUT   = 9;  // Drives Mega's single interrupt pin (A8) LOW while any stop condition is active

// --- Thermal Safety ---
const float TEMP_TRIP_C  = 120.0f; // T[0] (printhead LED temp) at/above this: force LEDs off + assert e-stop
const float TEMP_CLEAR_C = 110.0f; // Hysteresis: must drop back below this before the trip can clear
bool thermalTripped = false;

// --- CR Touch ---
Servo crTouch;

// --- NeoPixel Setup ---
Adafruit_NeoPixel pixels(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// Color Definitions
#define C_BOOT  pixels.Color(0, 0, 255)     // Blue
#define C_FIRE  pixels.Color(128, 0, 128)   // Purple
#define C_PROBE pixels.Color(0, 255, 255)   // Cyan
#define C_INFO  pixels.Color(50, 50, 50)    // Dimmer white
#define C_RESET pixels.Color(255, 100, 0)   // Orange
#define C_SUCC  pixels.Color(0, 255, 0)     // Green
#define C_ERR   pixels.Color(255, 0, 0)     // Red
#define C_OFF   pixels.Color(0, 0, 0)       // Off

// NeoPixel Queue (Circular Buffer)
#define NEO_QUEUE_SIZE 10
uint32_t queueC1[NEO_QUEUE_SIZE];
uint32_t queueC2[NEO_QUEUE_SIZE];
uint8_t qHead = 0; 
uint8_t qTail = 0; 

// NeoPixel State Machine
uint8_t neoState = 0; // 0=Idle, 1=Color1, 2=Color2, 3=Gap
uint32_t activeC1 = C_OFF;
uint32_t activeC2 = C_OFF;
unsigned long neoTimer = 0;
const int FLASH_MS = 150; // Milliseconds per flash
const int GAP_MS   = 100; // Dark time between messages

// --- UV LED State Variables ---
unsigned long startAt[4] = {0, 0, 0, 0};
uint32_t durationUS[4] = {0, 0, 0, 0};
bool ledActive[4] = {false, false, false, false};

// --- Thermal & Serial State Variables ---
byte currentTemps[6] = {0, 0, 0, 0, 0, 0};
unsigned long lastTempRead = 0;
String inBuffer = "";

// ============================================================
// --- Helper Functions ---
// ============================================================

void triggerNeo(uint32_t color1, uint32_t color2) {
  uint8_t nextHead = (qHead + 1) % NEO_QUEUE_SIZE;
  if (nextHead != qTail) { 
    queueC1[qHead] = color1;
    queueC2[qHead] = color2;
    qHead = nextHead;
  }
}

void handleNeoPixel() {
  unsigned long now = millis();

  if (neoState == 0) {
    if (qHead != qTail) {
      activeC1 = queueC1[qTail];
      activeC2 = queueC2[qTail];
      qTail = (qTail + 1) % NEO_QUEUE_SIZE;
      pixels.setPixelColor(0, activeC1);
      pixels.show();
      neoState = 1;
      neoTimer = now;
    }
  } 
  else if (neoState == 1 && now - neoTimer >= FLASH_MS) {
    pixels.setPixelColor(0, activeC2);
    pixels.show();
    neoState = 2;
    neoTimer = now;
  } 
  else if (neoState == 2 && now - neoTimer >= FLASH_MS) {
    pixels.setPixelColor(0, C_OFF);
    pixels.show();
    neoState = 3;
    neoTimer = now;
  }
  else if (neoState == 3 && now - neoTimer >= GAP_MS) {
    neoState = 0; 
  }
}

void processCommand(String cmd) {
  cmd.trim(); 

  if (cmd == "S") {
    uint8_t activeMask = 0;
    for (int i = 0; i < 4; i++) {
      if (ledActive[i]) bitSet(activeMask, i); 
    }
    Serial.print("S:");
    Serial.print(activeMask);
    Serial.print("\n");
    //triggerNeo(C_INFO, C_SUCC);
  } 
  else if (cmd == "P") {
    bool triggered = (digitalRead(PIN_PROBE_SIG) == LOW);
    Serial.print(triggered ? "P:1\n" : "P:0\n");
    triggerNeo(C_PROBE, C_SUCC);
  }
  else if (cmd == "D") {
    crTouch.write(10); 
    triggerNeo(C_PROBE, C_SUCC);
  }
  else if (cmd == "U") {
    crTouch.write(90); 
    triggerNeo(C_PROBE, C_SUCC);
  }
  else if (cmd == "X") {
    crTouch.write(160); 
    triggerNeo(C_PROBE, C_SUCC);
  }
  else if (cmd == "T") {
    Serial.print("T:");
    for(int i = 0; i < 6; i++) {
      Serial.print(currentTemps[i]);
      if(i < 5) Serial.print(",");
    }
    Serial.print("\n");
    triggerNeo(C_INFO, C_SUCC);
  }
  else if (cmd == "L") {
    // Reports which stop condition(s) are currently active: bit0=Z, bit1=Y, bit2=overtemp.
    uint8_t mask = 0;
    if (digitalRead(PIN_PROBE_SIG) == LOW) bitSet(mask, 0);
    if (digitalRead(PIN_Y_LIMIT) == LOW)   bitSet(mask, 1);
    if (thermalTripped)                    bitSet(mask, 2);
    Serial.print("L:");
    Serial.print(mask);
    Serial.print("\n");
    triggerNeo(C_INFO, mask ? C_ERR : C_SUCC);
  }
  else if (cmd == "R") {
    for (int i = 0; i < 4; i++) {
      ledActive[i] = false;
      digitalWrite(PIN_LED[i], LOW);
    }
    triggerNeo(C_RESET, C_SUCC);
  }
  else if (cmd.startsWith("F:")) {
    if (thermalTripped) {
      // Refuse to arm LEDs while over temperature, even if the Mega hasn't caught up yet.
      triggerNeo(C_ERR, C_ERR);
      return;
    }
    int maskIdx = cmd.indexOf(':') + 1;
    int comma1 = cmd.indexOf(',', maskIdx);
    int comma2 = cmd.indexOf(',', comma1 + 1);
    int comma3 = cmd.indexOf(',', comma2 + 1);
    int comma4 = cmd.indexOf(',', comma3 + 1);

    if (comma4 > 0) {
      byte mask = cmd.substring(maskIdx, comma1).toInt();
      uint32_t dur[4];
      dur[0] = strtoul(cmd.substring(comma1 + 1, comma2).c_str(), NULL, 10);
      dur[1] = strtoul(cmd.substring(comma2 + 1, comma3).c_str(), NULL, 10);
      dur[2] = strtoul(cmd.substring(comma3 + 1, comma4).c_str(), NULL, 10);
      dur[3] = strtoul(cmd.substring(comma4 + 1).c_str(), NULL, 10);

      unsigned long now = micros();
      for (int i = 0; i < 4; i++) {
        if (bitRead(mask, i) == 1 && dur[i] > 0) {
          durationUS[i] = dur[i];
          startAt[i] = now;
          ledActive[i] = true;
        }
      }
      triggerNeo(C_FIRE, C_SUCC);
    } else {
      triggerNeo(C_FIRE, C_ERR);
    }
  } else {
    triggerNeo(C_ERR, C_ERR);
  }
}

// ============================================================
// --- Main Setup & Loop ---
// ============================================================

void setup() {
  Serial.begin(115200); 
  
  pixels.begin();
  pixels.setBrightness(100); 
  pixels.show();
  
  for (int i = 0; i < 4; i++) {
    pinMode(PIN_LED[i], OUTPUT);
    digitalWrite(PIN_LED[i], LOW);
  }
  
  pinMode(PIN_PROBE_SIG, INPUT_PULLUP);
  pinMode(PIN_Y_LIMIT, INPUT_PULLUP);
  pinMode(PIN_ESTOP_OUT, OUTPUT);
  digitalWrite(PIN_ESTOP_OUT, HIGH); // Idle inactive; Mega's A8 uses INPUT_PULLUP + FALLING interrupt.
  crTouch.attach(PIN_PROBE_SERVO);
  crTouch.write(90); // Stow on boot

  inBuffer.reserve(64);
  triggerNeo(C_BOOT, C_SUCC);
}

void loop() {
  // 1. Process Incoming UART Commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processCommand(inBuffer);
      inBuffer = "";
    } else {
      inBuffer += c;
    }
  }

  // 2. Refresh time after parsing to prevent underflow
  unsigned long now = micros();

  // 3. Handle UV LED Microsecond Firing 
  for (int i = 0; i < 4; i++) {
    if (ledActive[i]) {
      if (now - startAt[i] >= durationUS[i]) {
        digitalWrite(PIN_LED[i], LOW);
        ledActive[i] = false;
      } else {
        digitalWrite(PIN_LED[i], HIGH);
      }
    }
  }

  // 4. Background Temp Reading (100ms interval)
  if (millis() - lastTempRead > 100) {
    for (int i = 0; i < 6; i++) {
      int rawADC = analogRead(PIN_TEMP[i]);
      float voltage = rawADC * (5.0 / 1023.0);
      float tempC = (voltage - 0.5) * 100.0;
      currentTemps[i] = max(0, min(255, (int)tempC));
    }

    // T[0] is the printhead LED temp -- cut power immediately on overtemp, independent
    // of whether the Mega is currently free to poll for it.
    if (!thermalTripped && currentTemps[0] >= TEMP_TRIP_C) {
      thermalTripped = true;
      for (int i = 0; i < 4; i++) {
        ledActive[i] = false;
        digitalWrite(PIN_LED[i], LOW);
      }
      triggerNeo(C_ERR, C_ERR);
    } else if (thermalTripped && currentTemps[0] <= TEMP_CLEAR_C) {
      thermalTripped = false;
    }

    lastTempRead = millis();
  }

  // 5. Drive the shared e-stop line to the Mega: LOW while any stop condition is active.
  bool limitActive = (digitalRead(PIN_PROBE_SIG) == LOW) ||
                      (digitalRead(PIN_Y_LIMIT) == LOW) ||
                      thermalTripped;
  digitalWrite(PIN_ESTOP_OUT, limitActive ? LOW : HIGH);

  // 6. Update NeoPixel State Machine
  handleNeoPixel();
}