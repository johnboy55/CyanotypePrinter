/* ============================================================
 * Printhead MCU - UART Slave (Arduino Nano)
 * V1.0 -- see /VERSIONS.md at repo root.
 * - Controls 4 UV LEDs (Microsecond precision)
 * - Controls CR Touch (D3 Servo, D7 Signal)
 * - Background polls 2 TMP36 Temp Sensors
 * - NeoPixel Status Indicator on D8 (Message Queue)
 * - Optional I2C status OLED on A4/A5 (SDA/SCL), probed at boot and never drawn
 *   while an LED is firing -- set OLED_ENABLED to 0 below to compile it out
 *
 * Requires (Library Manager): "Adafruit SSD1306" + "Adafruit GFX Library".
 * D4 and D9 are unused on V1.0 (no Y-limit switch, no Mega-side e-stop line);
 * only 2 of the 6 possible temp channels are populated, which is what leaves
 * A4/A5 free for the OLED's I2C bus instead of being TMP36 inputs.
 * ============================================================ */

// Set to 0 to compile the OLED out entirely (frees A4/A5 and ~600 bytes of RAM) -- the
// first thing to try if the UART link to the Mega ever misbehaves, to rule I2C out.
#define OLED_ENABLED 0

#include <Arduino.h>
#include <Servo.h>
#include <Adafruit_NeoPixel.h>
#if OLED_ENABLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

// --- Hardware Pins (Mapped to Custom PCB) ---
const int PIN_LED[4]      = {10, 11, 12, 13};
// Only 2 channels populated (T[0]=printhead LED temp, the one with the thermal
// cutoff; T[1] spare/ambient). A2/A3 are unused; A4/A5 are the OLED's I2C bus.
const int PIN_TEMP[2]     = {A0, A1};
const int PIN_PROBE_SIG   = 7;  // LIMIT_SURFACE -- also the Z hard-limit switch
const int PIN_PROBE_SERVO = 3;  // Hardware PWM for CR Touch
const int PIN_NEOPIXEL    = 8;  // NEO_IN

// --- Status OLED (optional accessory) ---
// Common 0.91" SSD1306 module is 128x32; a 0.96" module is 128x64 -- change
// OLED_HEIGHT to 64 if that's what's wired. Set OLED_ADDR to 0x3D if the module
// doesn't respond on the more common 0x3C (check its datasheet/silkscreen).
#if OLED_ENABLED
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C
const unsigned long OLED_REFRESH_MS = 250;
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
// Only true if the display ACKed its address at boot AND hasn't since caused an I2C
// fault. Adafruit_SSD1306::begin() itself never checks for a device (it only fails on
// malloc), so presence is probed explicitly in setup(). Once false it stays false:
// the UART link and LED timing always take priority over the display.
bool oledPresent = false;
unsigned long lastOledUpdate = 0;
uint16_t lastOledSig = 0xFFFF;    // hash of what's on screen, so unchanged content isn't re-sent
#endif

// --- Thermal Safety ---
// Enforced locally regardless of whether the Mega is polling: the Nano cuts its own LED
// power on overtemp rather than relying on a signal back to the Mega (V1.0 has no such
// line -- see /VERSIONS.md).
const float TEMP_TRIP_C  = 120.0f; // T[0] (printhead LED temp) at/above this: force LEDs off
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
byte currentTemps[2] = {0, 0};
unsigned long lastTempRead = 0;
String inBuffer = "";

// ============================================================
// --- Helper Functions ---
// ============================================================

// Several commands (P during Z-homing's step loop, T on a periodic poll, previously S in
// waitForPrinthead()'s busy-wait) arrive from the Mega far more often than "once" -- each
// call used to queue its own flash, so a burst of identical polls backed up the FIFO and
// then played back-to-back once it drained, looking like rapid repeated flashing for what
// was really one ongoing check. Coalesce: if this exact flash is already playing or
// already the only thing queued, don't pile another copy on -- but a genuinely different
// command still gets its own flash immediately.
uint32_t pendingC1 = C_OFF, pendingC2 = C_OFF;
bool hasPending = false;

void triggerNeo(uint32_t color1, uint32_t color2) {
  bool sameAsPending = hasPending && color1 == pendingC1 && color2 == pendingC2;
  bool alreadyShowing = (qHead != qTail) || (neoState != 0);
  if (sameAsPending && alreadyShowing) return;

  uint8_t nextHead = (qHead + 1) % NEO_QUEUE_SIZE;
  if (nextHead != qTail) {
    queueC1[qHead] = color1;
    queueC2[qHead] = color2;
    qHead = nextHead;
    pendingC1 = color1;
    pendingC2 = color2;
    hasPending = true;
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

#if OLED_ENABLED
// Redraws the status OLED. display() pushes the whole 512-byte frame over I2C (~15ms, and
// up to 25ms *per transaction* if the bus is faulty), during which loop() can't turn UV
// LEDs off on time or answer the Mega -- exposures can be as short as ~30ms, so this must
// never run while an LED is active. It also skips redraws when nothing on screen changed,
// and permanently disables itself on any I2C timeout so a bad bus can't starve the UART.
void updateOled() {
  if (!oledPresent) return;
  if (ledActive[0] || ledActive[1] || ledActive[2] || ledActive[3]) return;
  unsigned long now = millis();
  if (now - lastOledUpdate < OLED_REFRESH_MS) return;
  lastOledUpdate = now;

  uint8_t state = thermalTripped ? 2 : 0;   // 0=READY, 2=HOT (FIRE is never drawn: see above)
  uint16_t sig = ((uint16_t)state << 8) ^ ((uint16_t)currentTemps[0] << 4) ^ currentTemps[1];
  if (sig == lastOledSig) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.setTextSize(2);
  oled.println(state == 2 ? "HOT!" : "READY");
  oled.setTextSize(1);
  oled.print("T0:"); oled.print(currentTemps[0]); oled.print("C ");
  oled.print("T1:"); oled.print(currentTemps[1]); oled.println("C");
  oled.display();
  lastOledSig = sig;

#ifdef WIRE_HAS_TIMEOUT
  if (Wire.getWireTimeoutFlag()) {
    oledPresent = false;   // bus fault -- give up on the display for this power cycle
    Wire.end();            // releases A4/A5 and the TWI pull-ups
  }
#endif
}
#else
inline void updateOled() {}
#endif

void processCommand(String cmd) {
  cmd.trim(); 

  if (cmd == "S") {
    uint8_t activeMask = 0;
    for (int i = 0; i < 4; i++) {
      if (ledActive[i]) bitSet(activeMask, i);
    }
    // Piggyback T[0] (printhead LED temp) on every status reply. The Mega polls S in a
    // tight loop to detect end-of-fire (S:0) and used to follow that up with a separate
    // T round-trip to get the temp for that same moment -- folding it in here saves that
    // extra request/response pair on every fired pixel.
    Serial.print("S:");
    Serial.print(activeMask);
    Serial.print(",");
    Serial.print(currentTemps[0]);
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
    for(int i = 0; i < 2; i++) {
      Serial.print(currentTemps[i]);
      if(i < 1) Serial.print(",");
    }
    Serial.print("\n");
    triggerNeo(C_INFO, C_SUCC);
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
  crTouch.attach(PIN_PROBE_SERVO);
  crTouch.write(90); // Stow on boot

#if OLED_ENABLED
  // Optional accessory. Probe the address ourselves (with a short Wire timeout so a stuck
  // bus can't hang setup() before the UART is serviced); only if it ACKs do we let the
  // library allocate its frame buffer and take over. No display => Wire is shut back off.
  Wire.begin();
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(5000, true);
#endif
  Wire.beginTransmission(OLED_ADDR);
  bool oledAcked = (Wire.endTransmission() == 0);
#ifdef WIRE_HAS_TIMEOUT
  if (Wire.getWireTimeoutFlag()) oledAcked = false;
#endif
  if (oledAcked && oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    oledPresent = true;
    oled.clearDisplay();
    oled.display();
  } else {
    Wire.end();
  }
#endif

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
    for (int i = 0; i < 2; i++) {
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

  // 5. Update NeoPixel State Machine
  handleNeoPixel();

  // 6. Refresh the status OLED, if one's wired
  updateOled();
}