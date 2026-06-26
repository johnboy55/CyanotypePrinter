/* ============================================================
 * UV Printhead Controller —  v2.2.0 (Serial Relay & Status Ping)
 * Architecture: Arduino Mega 2560 + CNC Shield V3
 * - ESP32 UI via Serial3
 * - Printhead MCU via Serial1
 * - Mesh Bed Leveling (MBL) via CR Touch
 * - Hardware Emergency Stop via PCINT
 * ============================================================ */

#include "uv_types.h" // Contains Btn, Image4x, Config structs, and CFG object
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>  
#include <Adafruit_SSD1306.h>
#include <stdarg.h>
#include <stdio.h>
#include <PinChangeInterrupt.h> 

#define FW_VERSION "UV Controller v2.2.0"

// ---------- Serial Definitions ----------
#define PRINTHEAD_SERIAL Serial1
#define PRINTHEAD_BAUD 115200

#define ESP_SERIAL Serial3 
#define ESP_BAUD 115200

// ---------- printf helper ----------
void serialPrintf(const char *fmt, ...) {
  char buf[128]; va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
  Serial.print(buf);
}

long lround_double(double x) { return (x >= 0.0) ? (long)(x + 0.5) : (long)(x - 0.5); }

// ---------- OLED & SD ----------
SdFat SD;
#define OLED_SCREEN_WIDTH 128
#define OLED_SCREEN_HEIGHT 64
#define OLED_RESET_PIN -1
Adafruit_SSD1306 display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);
const int PIN_SD_CS = 53;     

// ---------- PINS ----------
const int PIN_STEP = 2;       
const int PIN_DIR  = 5;       
const int PIN_EN   = 8;       

const int PIN_YL_STEP = 3;    
const int PIN_YR_STEP = 4;    
const int PIN_Y_DIR   = 6;    
const int PIN_YR_DIR  = 7;    

const int PIN_Z_STEP  = 12;   
const int PIN_Z_DIR   = 13;   

const int PIN_Z_CONTACT = A8; 

const int PIN_BTN_PLUS  = 26; 
const int PIN_BTN_MINUS = 27; 
const int PIN_BTN_ENTER = 28; 

// ---------- Motion & Geometry ----------
const int  MOTOR_STEPS_PER_REV = 200;
int   MICROSTEPS_X   = 16;
float LEAD_MM_X      = 8.0f;
float STEPS_PER_MM_X = 400.0f;

int   MICROSTEPS_Y   = 16;
float LEAD_MM_Y      = 4.0f;
float STEPS_PER_MM_Y = 320.0f;

float STEPS_PER_MM_Z = 400.0f; 

float MM_PER_PIXEL    = 2.0f;
int   STEPS_PER_PIXEL = 0;              
float LED_SPACING_MM  = 20.0f;
int   OFFSET_PIXELS   = 0;

int STEP_PULSE_US  = 5;                 
int STEP_PERIOD_US = 1000;

long currentSteps  = 0; 
long currentStepsY = 0; 
long currentStepsZ = 0; 

// ---------- Mesh Bed Leveling (MBL) ----------
const int MESH_SIZE = 3; 
float z_mesh[MESH_SIZE][MESH_SIZE];
bool  mesh_active = false;
const float MESH_MAX_X = 50.0f; 
const float MESH_MAX_Y = 50.0f; 

// The physical distance from the center of the LED array to the CR Touch pin
const float PROBE_OFFSET_X = 0.0f;  // mm (0 if centered left/right)
const float PROBE_OFFSET_Y = 30.0f; // mm (Positive if probe is behind the LEDs)

// ---------- LED arrays & Types ----------
const int NUM_LEDS = 4;
int ROW_FOR_LED[4] = {3, 2, 1, 0};

volatile bool emergencyStopTriggered = false;

float LUT16[16] = { 0.00, 0.10, 0.20, 0.30, 0.40, 0.55, 0.70, 0.90, 1.10, 1.30, 1.60, 2.00, 2.50, 3.20, 4.00, 5.00 };

// ---------- Button handling ----------
const uint32_t DEBOUNCE_MS = 35;
static inline bool isPressedRaw(const Btn &b, bool raw) { return b.activeHigh ? (raw==HIGH) : (raw==LOW); }
Btn btnPlus  = {PIN_BTN_PLUS,  HIGH,  0, false, false};
Btn btnMinus = {PIN_BTN_MINUS, HIGH,  0, false, false};
Btn btnEnter = {PIN_BTN_ENTER, HIGH, 0, false, false};

void displayPrintf(Adafruit_SSD1306 &disp, const char *fmt, ...) {
  char buf[64]; va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); disp.print(buf);
}

bool readButton(Btn &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.last && (millis() - b.lastChange) > DEBOUNCE_MS) {
    b.last = raw; b.lastChange = millis();
    if (isPressedRaw(b, raw)) b.pressedEvent = true;
  }
  if (b.pressedEvent) { b.pressedEvent = false; return true; }
  return false;
}
bool buttonHeld(const Btn &b) { return isPressedRaw(b, digitalRead(b.pin)); }

// ---------- OLED Helpers ----------
void oledHeader(const char* title) {
  display.clearDisplay(); display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE); display.setCursor(0,0);
  display.print(title); display.drawLine(0,10,127,10,SSD1306_WHITE);
}

void drawStatusRow(const bool ledOn[4], const String &rightText) {
  display.fillRect(0, 52, 128, 12, SSD1306_BLACK); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  int x = 0, y = 54;
  for (int i=0; i<4; ++i) {
    if (ledOn[i]) { display.fillRect(x, y-1, 14, 10, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); } 
    else { display.drawRect(x, y-1, 14, 10, SSD1306_WHITE); }
    display.setCursor(x+3, y); display.print(i+1); display.setTextColor(SSD1306_WHITE); x += 16;
  }
  int16_t bx,by; uint16_t bw,bh; display.getTextBounds(rightText, 0, y, &bx, &by, &bw, &bh);
  int rx = 128 - (int)bw - 2; if (rx < 68) rx = 68;
  display.setCursor(rx, y); display.print(rightText); display.display();
}

// ---------- Config Loading ----------
static String trimBoth(const String &s) {
  int a=0,b=(int)s.length();
  while (a<b && isspace((unsigned char)s[a])) ++a;
  while (b>a && isspace((unsigned char)s[b-1])) --b;
  return s.substring(a,b);
}
static bool parseFloatKey(const String &line, const char* key, float &out) {
  String k(key); k += "="; if (!line.startsWith(k)) return false;
  out = trimBoth(line.substring(k.length())).toFloat(); return true;
}

void loadConfig() {
  if (!SD.exists("config.txt")) {
    oledHeader("Config Error"); display.setCursor(0, 20); display.print("config.txt not found");
    display.display(); delay(2000); return;
  }
  File f = SD.open("config.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    parseFloatKey(line, "yBandAdvanceMM", CFG.yBandAdvanceMM);
    parseFloatKey(line, "mmPerPixel", CFG.mmPerPixel);
    parseFloatKey(line, "ledSpacingMM", CFG.ledSpacingMM);
  }
  f.close();
  
  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * CFG.mmPerPixel + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(CFG.ledSpacingMM / CFG.mmPerPixel + 0.5f));
  
  oledHeader("Config"); display.setCursor(0, 20); display.print("Loaded config.txt");
  display.display(); delay(1500);
}

// ---------- UART API (Printhead) ----------
void sendBatchToPrinthead(uint32_t durations[4]) {
  uint8_t mask = 0;
  for (int i=0; i<4; i++) { if (durations[i] > 0) bitSet(mask, i); }
  if (mask == 0) return;
  String cmd = "F:" + String(mask) + "," + String(durations[0]) + "," + 
               String(durations[1]) + "," + String(durations[2]) + "," + String(durations[3]) + "\n";
  PRINTHEAD_SERIAL.print(cmd);
}

void waitForPrinthead() {
  bool isBusy = true;
  while (isBusy) {
    if (emergencyStopTriggered) handleEmergencyStop();
    PRINTHEAD_SERIAL.print("S\n"); 
    unsigned long timeout = millis();
    while(!PRINTHEAD_SERIAL.available() && millis() - timeout < 10) {}
    if (PRINTHEAD_SERIAL.available()) {
      String response = PRINTHEAD_SERIAL.readStringUntil('\n'); response.trim();
      if (response == "S:0") isBusy = false;
    }
  }
}

bool isProbeTriggered() {
  PRINTHEAD_SERIAL.print("P\n"); 
  unsigned long timeout = millis();
  while(!PRINTHEAD_SERIAL.available() && millis() - timeout < 10) {}
  if (PRINTHEAD_SERIAL.available()) {
    String response = PRINTHEAD_SERIAL.readStringUntil('\n'); response.trim();
    if (response == "P:1") return true;
  }
  return false;
}

void deployProbe() {
  PRINTHEAD_SERIAL.print("D\n");
  delay(500); 
}

void stowProbe() {
  PRINTHEAD_SERIAL.print("U\n");
  delay(500); 
}

// ---------- Stepper Math & Motion ----------
float getInterpolatedZ(float x_mm, float y_mm) {
  if (!mesh_active) return 0.0f;
  x_mm = constrain(x_mm, 0.0f, MESH_MAX_X); y_mm = constrain(y_mm, 0.0f, MESH_MAX_Y);
  float spacing_x = MESH_MAX_X / (MESH_SIZE - 1); float spacing_y = MESH_MAX_Y / (MESH_SIZE - 1);
  int i = min((int)(x_mm / spacing_x), MESH_SIZE - 2); int j = min((int)(y_mm / spacing_y), MESH_SIZE - 2);
  float tx = (x_mm - (i * spacing_x)) / spacing_x; float ty = (y_mm - (j * spacing_y)) / spacing_y;
  float z00 = z_mesh[i][j], z10 = z_mesh[i+1][j], z01 = z_mesh[i][j+1], z11 = z_mesh[i+1][j+1];
  return (1 - tx)*(1 - ty)*z00 + tx*(1 - ty)*z10 + (1 - tx)*ty*z01 + tx*ty*z11;
}

void zPulse(bool dir) {
  digitalWrite(PIN_Z_DIR, dir ? HIGH : LOW); digitalWrite(PIN_Z_STEP, HIGH);
  delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_Z_STEP, LOW);
  currentStepsZ += dir ? 1 : -1;
}

float probeZ() {
  deployProbe();
  while (!isProbeTriggered()) { 
    zPulse(true); 
    delayMicroseconds(2000); 
  }
  float trigger_height = currentStepsZ / STEPS_PER_MM_Z;
  stowProbe(); 
  for(int i=0; i < (2.0f * STEPS_PER_MM_Z); i++) {
    zPulse(false); 
    delayMicroseconds(1000); 
  }
  return trigger_height;
}

void stepPulseOnceX() { digitalWrite(PIN_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_STEP, LOW); }
inline void yPulseLeft()  { digitalWrite(PIN_YL_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_YL_STEP, LOW); }
inline void yPulseRight() { digitalWrite(PIN_YR_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_YR_STEP, LOW); }

void stepN(long n, bool dir) {
  digitalWrite(PIN_DIR, dir ? HIGH : LOW);
  float currentY_mm = currentStepsY / STEPS_PER_MM_Y;
  float targetX_mm = (currentSteps + (dir ? n : -n)) / STEPS_PER_MM_X;
  long startZ = currentStepsZ;
  long targetZ = lround_double(getInterpolatedZ(targetX_mm, currentY_mm) * STEPS_PER_MM_Z);
  long dz = targetZ - startZ; bool zDir = (dz > 0); dz = abs(dz);
  long err = n / 2; 

  for (long i = 0; i < n; ++i) {
    if (emergencyStopTriggered) handleEmergencyStop();
    stepPulseOnceX(); currentSteps += dir ? 1 : -1;
    if (mesh_active && dz > 0) { err -= dz; if (err < 0) { zPulse(zDir); err += n; } }
    delayMicroseconds(max(0, STEP_PERIOD_US - STEP_PULSE_US));
  }
}

void yStepPair(long stepsL, long stepsR, bool dir) {
  if (stepsL <= 0 && stepsR <= 0) return;
  digitalWrite(PIN_Y_DIR, dir ? HIGH : LOW); digitalWrite(PIN_YR_DIR, dir ? HIGH : LOW); digitalWrite(PIN_EN, LOW);
  long N = max(stepsL, stepsR);
  float currentX_mm = currentSteps / STEPS_PER_MM_X;
  float targetY_mm = (currentStepsY + (dir ? N : -N)) / STEPS_PER_MM_Y;
  
  long startZ = currentStepsZ;
  long targetZ = lround_double(getInterpolatedZ(currentX_mm, targetY_mm) * STEPS_PER_MM_Z);
  long dz = targetZ - startZ; bool zDir = (dz > 0); dz = abs(dz);
  long err = N / 2; long accL = 0, accR = 0;

  for (long i = 0; i < N; ++i) {
    if (emergencyStopTriggered) handleEmergencyStop();
    accL += stepsL; accR += stepsR;
    if (accL >= N) { yPulseLeft(); accL -= N; }
    if (accR >= N) { yPulseRight(); accR -= N; }
    currentStepsY += dir ? 1 : -1;
    if (mesh_active && dz > 0) { err -= dz; if (err < 0) { zPulse(zDir); err += N; } }
    delayMicroseconds(max(0, STEP_PERIOD_US - STEP_PULSE_US));
  }
}

void moveToSteps(long target) { long d = target - currentSteps; if (d) stepN(labs(d), d>0); }
void moveToPixel(long p) { moveToSteps(p * (long)STEPS_PER_PIXEL); }
void yMoveMM(float mm, bool forward) { long s = lround_double(mm * STEPS_PER_MM_Y); yStepPair(s, s, forward); }
void xMoveMM(float mm, bool right) { long s = lround_double(mm * STEPS_PER_MM_X); stepN(s, right); }

// ---------- Image Handling ----------
float lutExposureFromByte(uint8_t v) {
  if (v <= 0) return LUT16[0]; if (v >= 255) return LUT16[15];
  int idx = v / 17; if (idx >= 15) return LUT16[15];
  float t = float(v - (idx * 17)) / 17.0f;
  return LUT16[idx] + t*(LUT16[idx+1] - LUT16[idx]);
}

uint8_t hexCharToVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a'); return 0;
}

void doPrintBMPSliceSerpentine(const Image4x &img, bool forward, bool byRow) {
  digitalWrite(PIN_EN, LOW);
  const int lastP = img.width - 1 + 3*OFFSET_PIXELS;
  const float expDiv = byRow ? .25 : 1.0;
  if (forward) moveToPixel(0); else moveToPixel(lastP);

  int pStart = forward ? 0 : lastP; int pEnd = forward ? lastP : 0; int pStep = forward ? +1 : -1;
  
  for (int p = pStart; ; p += pStep) {
    if (emergencyStopTriggered) handleEmergencyStop();
    uint32_t durUS[4] = {0,0,0,0}; bool willFire[4] = {false,false,false,false}; int activeCount = 0;
    
    for (int k=0;k<4;k++) {
      const int t = p - k*OFFSET_PIXELS;
      if (t >= 0 && t < img.width) {
        uint8_t v  = img.row[ ROW_FOR_LED[k] ][t];
        if (CFG.invertImage) v = 255 - v;
        const float s = lutExposureFromByte(v) * expDiv;
        if (s > 0.0f) { durUS[k] = (uint32_t)(s * 1e6f); willFire[k] = true; activeCount++; }
      }
    }
    const String right = String("Pos ") + String(p) + "/" + String(lastP);
    drawStatusRow(willFire, right);
    if (activeCount > 0) { sendBatchToPrinthead(durUS); waitForPrinthead(); }
    if (p == pEnd) break;
    stepN(STEPS_PER_PIXEL, forward);
  }
  digitalWrite(PIN_EN, HIGH);
}

// ---------- Safety Interrupt ----------
void contactInterruptHandler() { emergencyStopTriggered = true; digitalWrite(PIN_EN, HIGH); }

void handleEmergencyStop() {
  digitalWrite(PIN_EN, HIGH); PRINTHEAD_SERIAL.print("R\n"); 
  oledHeader("Z-CRASH!"); display.setCursor(0, 30); display.print("Contact switch hit.");
  display.setCursor(0, 45); display.print("System halted."); display.display();
  while(true) { delay(10); } 
}

// ---------- File Browser Helper ----------
bool listFilesToOLED(const char* dirPath, const char* ext, String &outFilename) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;
  
  const int MAX_FILES = 20; String files[MAX_FILES]; int count = 0;
  while (true) {
    File entry = dir.openNextFile(); if (!entry) break;
    if (!entry.isDirectory()) {
      char nameBuf[64]; 
      entry.getName(nameBuf, sizeof(nameBuf));
      String name = String(nameBuf);
      if (name.endsWith(ext) && count < MAX_FILES) { files[count++] = name; }
    }
    entry.close();
  }
  dir.close();
  if (count == 0) {
    oledHeader("Browser"); display.setCursor(0,20); display.print("No files found.");
    display.display(); delay(1500); return false;
  }

  int sel = 0, top = 0;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader(dirPath);
    for (int i=0; i<4; i++) {
      int idx = top + i; if (idx >= count) break;
      display.setCursor(0, 16 + (i*10));
      display.print(idx == sel ? "> " : "  "); display.print(files[idx]);
    }
    display.display();
    if (readButton(btnPlus)) { sel++; if (sel >= count) sel = count - 1; if (sel >= top + 4) top++; }
    if (readButton(btnMinus)) { sel--; if (sel < 0) sel = 0; if (sel < top) top--; }
    if (readButton(btnEnter)) { outFilename = String(dirPath) + "/" + files[sel]; return true; }
    if (buttonHeld(btnMinus)) { unsigned long t0=millis(); while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return false; } }
    delay(10);
  }
}

// ---------- Full Menus ----------

void menuMotor() { 
  digitalWrite(PIN_EN, LOW); float xpos = currentSteps / STEPS_PER_MM_X;
  while(true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("X Axis Ctrl");
    display.setCursor(0, 20); displayPrintf(display, "X: %.1f mm", xpos);
    display.setCursor(0, 50); display.print("[+/-] 10mm [Ent] Exit"); display.display();
    if (readButton(btnPlus)) { xMoveMM(10.0, true); xpos += 10.0; }
    if (readButton(btnMinus)) { xMoveMM(10.0, false); xpos -= 10.0; }
    if (readButton(btnEnter)) { digitalWrite(PIN_EN, HIGH); return; }
    delay(10);
  }
}

void menuYCtrl() { 
  digitalWrite(PIN_EN, LOW); float ypos = currentStepsY / STEPS_PER_MM_Y;
  while(true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Y Axis Ctrl");
    display.setCursor(0, 20); displayPrintf(display, "Y: %.1f mm", ypos);
    display.setCursor(0, 50); display.print("[+/-] 10mm [Ent] Exit"); display.display();
    if (readButton(btnPlus)) { yMoveMM(10.0, true); ypos += 10.0; }
    if (readButton(btnMinus)) { yMoveMM(10.0, false); ypos -= 10.0; }
    if (readButton(btnEnter)) { digitalWrite(PIN_EN, HIGH); return; }
    delay(10);
  }
}

void menuCalib() { 
  int step = 0;
  while(true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Calibration");
    display.setCursor(0, 20); displayPrintf(display, "Fire LED %d", step+1);
    display.setCursor(0, 50); display.print("[+/-] Sel  [Ent] Fire"); display.display();
    if (readButton(btnPlus)) { step++; if(step > 3) step = 0; }
    if (readButton(btnMinus)) { step--; if(step < 0) step = 3; }
    if (readButton(btnEnter)) {
      uint32_t dur[4] = {0,0,0,0}; dur[step] = 1000000; 
      sendBatchToPrinthead(dur); waitForPrinthead();
    }
    if (buttonHeld(btnMinus)) { unsigned long t0=millis(); while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } }
    delay(10);
  }
}

// --- Test Sub-menus ---
void testFireLEDs() { 
  int ms = 500;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Test LEDs");
    display.setCursor(0, 20); displayPrintf(display, "Dur: %d ms", ms);
    display.setCursor(0, 50); display.print("[+/-] Time [Ent] Fire"); display.display();
    
    if (readButton(btnPlus)) ms += 100;
    if (readButton(btnMinus)) { ms -= 100; if (ms < 100) ms = 100; }
    if (readButton(btnEnter)) {
      uint32_t dur[4] = {(uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000};
      sendBatchToPrinthead(dur); waitForPrinthead();
    }
    if (buttonHeld(btnMinus)) { 
      unsigned long t0=millis(); 
      while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } 
    }
    delay(10);
  }
}

void testSerialRelay() {
  oledHeader("Serial Relay");
  display.setCursor(0, 20); display.print("Passthrough Active");
  display.setCursor(0, 35); display.print("PC <---> Nano");
  display.setCursor(0, 54); display.print("Hold [-] to exit");
  display.display();

  while(Serial.available()) Serial.read();
  while(PRINTHEAD_SERIAL.available()) PRINTHEAD_SERIAL.read();

  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    if (Serial.available()) { PRINTHEAD_SERIAL.write(Serial.read()); }
    if (PRINTHEAD_SERIAL.available()) { Serial.write(PRINTHEAD_SERIAL.read()); }
    if (buttonHeld(btnMinus)) {
      unsigned long t0 = millis();
      while (buttonHeld(btnMinus)) { delay(5); if (millis() - t0 > 600) return; }
    }
  }
}

void menuTest() {
  int sel = 0; 
  const int ITEMS = 2;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Test Menu");
    display.setCursor(0, 20); display.print(sel==0?"> ":"  "); display.print("Fire LEDs");
    display.setCursor(0, 32); display.print(sel==1?"> ":"  "); display.print("Serial Relay");
    display.setCursor(0, 54); display.print("Hold [-] to exit");
    display.display();

    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) testFireLEDs();
      if (sel == 1) testSerialRelay();
    }
    if (buttonHeld(btnMinus)) { 
      unsigned long t0=millis(); 
      while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } 
    }
    delay(10);
  }
}

void menuBrowser() {
  String picked; if (!listFilesToOLED("/", "", picked)) return; 
  oledHeader("Selected"); display.setCursor(0,20); display.print(picked); display.display(); delay(2000);
}

void menuPrint() {
  String picked; if (!listFilesToOLED("/", ".bmp", picked)) return;
  oledHeader("Print BMP"); display.setCursor(0,20); display.print(picked); 
  display.setCursor(0,40); display.print("Ready to print..."); display.display(); delay(2000);
}

void measureMatrix() {
  oledHeader("Probing Mesh..."); display.display();
  if (!SD.exists("/meshes")) { SD.mkdir("/meshes"); }
  
  float spacing_x = MESH_MAX_X / (MESH_SIZE - 1); 
  float spacing_y = MESH_MAX_Y / (MESH_SIZE - 1);

  for (int i = 0; i < MESH_SIZE; i++) {
    for (int j = 0; j < MESH_SIZE; j++) {
      if (emergencyStopTriggered) handleEmergencyStop();
      int y_idx = (i % 2 == 0) ? j : (MESH_SIZE - 1 - j);
      float target_bed_x = (i * spacing_x);
      float target_bed_y = (y_idx * spacing_y);
      float move_to_x = target_bed_x - PROBE_OFFSET_X;
      float move_to_y = target_bed_y - PROBE_OFFSET_Y;
      
      moveToPixel(move_to_x / CFG.mmPerPixel); 
      float currentY_mm = currentStepsY / STEPS_PER_MM_Y;
      yMoveMM(abs(move_to_y - currentY_mm), move_to_y > currentY_mm);
      
      z_mesh[i][y_idx] = probeZ();
      
      display.fillRect(0, 20, 128, 44, SSD1306_BLACK);
      display.setCursor(0, 30); 
      displayPrintf(display, "Pt %d,%d Z:%.2f", i, y_idx, z_mesh[i][y_idx]); 
      display.display();
    }
  }

  int fileIndex = 0; String filename;
  do { filename = "/meshes/m" + String(fileIndex) + ".txt"; fileIndex++; } while (SD.exists(filename));
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    for (int i = 0; i < MESH_SIZE; i++) {
      for (int j = 0; j < MESH_SIZE; j++) { f.print(z_mesh[i][j], 3); if (j < MESH_SIZE - 1) f.print(","); }
      f.println();
    }
    f.close(); mesh_active = true;
    oledHeader("Mesh Saved"); display.setCursor(0, 20); display.print(filename); display.display(); delay(2000);
  }
}

void loadMatrixFile() { 
  String picked; if (!listFilesToOLED("/meshes", ".txt", picked)) return;
  File f = SD.open(picked); if (!f) return;
  oledHeader("Loading Mesh"); display.display();
  for (int i = 0; i < MESH_SIZE; i++) {
    String line = f.readStringUntil('\n'); int start = 0;
    for (int j = 0; j < MESH_SIZE; j++) {
      int comma = line.indexOf(',', start);
      String val = (comma == -1) ? line.substring(start) : line.substring(start, comma);
      z_mesh[i][j] = val.toFloat(); start = comma + 1;
    }
  }
  f.close(); mesh_active = true;
  display.setCursor(0, 30); display.print("Mesh Active!"); display.display(); delay(1500);
}

void zeroXHere() { currentSteps = 0; currentStepsY = 0; currentStepsZ = 0; oledHeader("Zeroed"); display.display(); delay(800); }

void menuUtilities() {
  int sel = 0; const int ITEMS = 4;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Utilities");
    display.setCursor(0, 16); display.print(sel==0?"> ":"  "); display.print("Zero Pos Here");
    display.setCursor(0, 26); display.print(sel==1?"> ":"  "); display.print("Reload Config");
    display.setCursor(0, 36); display.print(sel==2?"> ":"  "); display.print("Measure Bed Mesh");
    display.setCursor(0, 46); display.print(sel==3?"> ":"  "); display.print("Load Bed Mesh");
    display.display();
    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) { zeroXHere(); return; }
      if (sel == 1) { loadConfig(); return; }
      if (sel == 2) { measureMatrix(); return; }
      if (sel == 3) { loadMatrixFile(); return; }
    }
    if (buttonHeld(btnMinus)) { unsigned long t0=millis(); while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } }
    delay(10);
  }
}

void menuESP32Control() {
  oledHeader("ESP32 Mode"); display.setCursor(0, 16); display.print("Waiting for ESP32...");
  display.setCursor(0, 56); display.print("Hold [-] to exit"); display.display();

  ESP_SERIAL.println("READY"); bool exitScreen = false;

  while (!exitScreen) {
    if (emergencyStopTriggered) handleEmergencyStop();
    if (ESP_SERIAL.available()) {
      String cmd = ESP_SERIAL.readStringUntil('\n'); cmd.trim();
      if (cmd.startsWith("P:")) {
        int comma = cmd.indexOf(','); int colon = cmd.indexOf(':', comma);
        if (comma > 0 && colon > 0) {
          int w = cmd.substring(2, comma).toInt(); int h = cmd.substring(comma + 1, colon).toInt();
          String hexData = cmd.substring(colon + 1);

          oledHeader("ESP32 Print"); display.setCursor(0, 20); displayPrintf(display, "W:%d H:%d", w, h); display.display();

          const int totalSlices = (h + 3) / 4; 
          for (int slice = 0; slice < totalSlices; ++slice) {
            Image4x img; img.width = min(MAX_IMG_WIDTH, w);
            for (int r = 0; r < 4; r++) for (int c = 0; c < MAX_IMG_WIDTH; c++) img.row[r][c] = 0;

            int visualStart = slice * 4;
            for (int vr = 0; vr < 4; ++vr) {
              int rowIdx = visualStart + vr; if (rowIdx >= h) break;
              for (int col = 0; col < w; ++col) {
                int strIdx = (rowIdx * w) + col;
                if (strIdx < (int)hexData.length()) img.row[vr][col] = hexCharToVal(hexData[strIdx]) * 17; 
              }
            }
            doPrintBMPSliceSerpentine(img, (slice % 2 == 0), false);
            if (slice < totalSlices - 1) yMoveMM(CFG.yBandAdvanceMM, true);
          }
          ESP_SERIAL.println("finish");
          oledHeader("ESP32 Mode"); display.setCursor(0, 16); display.print("Waiting..."); display.display();
        } else { ESP_SERIAL.println("error"); }
      }
    }
    if (buttonHeld(btnMinus)) {
      unsigned long t0 = millis();
      while (buttonHeld(btnMinus)) { delay(5); if (millis() - t0 > 600) { ESP_SERIAL.println("EXIT"); exitScreen = true; break; } }
    }
    delay(5);
  }
}

enum MainMenu { MM_CALIB = 0, MM_PRINT = 1, MM_TEST = 2, MM_MOTOR = 3, MM_BROWSER = 4, MM_YCTRL = 5, MM_UTILS = 6, MM_ESP32 = 7 };

void menuMain() {
  static int mainSel = 0; 
  
  static unsigned long lastPing = 0;
  static unsigned long lastResp = millis(); 
  static String nanoStatus = "Wait";

  if (millis() - lastPing > 500) {
    PRINTHEAD_SERIAL.print("S\n");
    lastPing = millis();
  }

  while (PRINTHEAD_SERIAL.available()) {
    static String rBuf = "";
    char c = PRINTHEAD_SERIAL.read();
    if (c == '\n') {
      rBuf.trim();
      if (rBuf.startsWith("S:")) {
        nanoStatus = (rBuf == "S:0") ? "Ready" : "Busy";
        lastResp = millis();
      }
      rBuf = "";
    } else {
      rBuf += c;
    }
  }

  if (millis() - lastResp > 2000) {
    nanoStatus = "Offline";
  }

  oledHeader("Main Menu");
  display.setCursor(70, 0);
  display.print(nanoStatus);

  display.setCursor(0, 14); display.print(mainSel==0?"> ":"  "); display.print("Calib");
  display.setCursor(0, 26); display.print(mainSel==1?"> ":"  "); display.print("Print");
  display.setCursor(0, 38); display.print(mainSel==2?"> ":"  "); display.print("Test");
  display.setCursor(0, 50); display.print(mainSel==3?"> ":"  "); display.print("S Ctrl");
  display.setCursor(58, 14); display.print(mainSel==4?"> ":"  "); display.print("Browser");
  display.setCursor(58, 26); display.print(mainSel==5?"> ":"  "); display.print("Y Ctrl");
  display.setCursor(58, 38); display.print(mainSel==6?"> ":"  "); display.print("Utils");
  display.setCursor(58, 50); display.print(mainSel==7?"> ":"  "); display.print("ESP32");
  display.display();

  if (readButton(btnPlus))  mainSel = (mainSel + 1) % 8;
  if (readButton(btnMinus)) mainSel = (mainSel - 1 + 8) % 8;
  if (readButton(btnEnter)) {
    if      (mainSel == MM_CALIB)   menuCalib();
    else if (mainSel == MM_PRINT)   menuPrint();
    else if (mainSel == MM_TEST)    menuTest();
    else if (mainSel == MM_MOTOR)   menuMotor();
    else if (mainSel == MM_BROWSER) menuBrowser();
    else if (mainSel == MM_YCTRL)   menuYCtrl();
    else if (mainSel == MM_UTILS)   menuUtilities();
    else if (mainSel == MM_ESP32)   menuESP32Control();
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  ESP_SERIAL.begin(ESP_BAUD);
  PRINTHEAD_SERIAL.begin(PRINTHEAD_BAUD);
  
  pinMode(PIN_STEP, OUTPUT); pinMode(PIN_DIR,  OUTPUT);
  pinMode(PIN_EN,   OUTPUT); digitalWrite(PIN_EN, HIGH);
  pinMode(PIN_YL_STEP, OUTPUT); pinMode(PIN_YR_STEP, OUTPUT);
  pinMode(PIN_Y_DIR,   OUTPUT); pinMode(PIN_YR_DIR,   OUTPUT);
  
  pinMode(PIN_Z_STEP, OUTPUT); pinMode(PIN_Z_DIR, OUTPUT);
  pinMode(PIN_Z_CONTACT, INPUT_PULLUP);
  attachPCINT(digitalPinToPCINT(PIN_Z_CONTACT), contactInterruptHandler, FALLING);

  pinMode(PIN_BTN_PLUS,  INPUT_PULLUP); pinMode(PIN_BTN_MINUS, INPUT_PULLUP); pinMode(PIN_BTN_ENTER, INPUT_PULLUP); 

  STEPS_PER_MM_X  = (MOTOR_STEPS_PER_REV * MICROSTEPS_X) / LEAD_MM_X;
  STEPS_PER_MM_Y  = (MOTOR_STEPS_PER_REV * MICROSTEPS_Y) / LEAD_MM_Y;
  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));

  Wire.begin(); display.begin(SSD1306_SWITCHCAPVCC, 0x3C); display.clearDisplay(); display.display();
  SPI.begin(); SD.begin(PIN_SD_CS);
  loadConfig(); 
}

void loop() {
  if (emergencyStopTriggered) handleEmergencyStop();
  menuMain();
  delay(10);
}