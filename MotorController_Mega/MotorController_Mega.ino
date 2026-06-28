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
// Z homing is done using the printhead Nano probe limit signal via Serial1.
const float Z_HOME_SWITCH_TO_PAPER_MM = 8.0f; // Distance from CR-touch trigger to paper surface. Measure and adjust.
const float Z_MAX_TO_CR_TOUCH_APPROACH_MM = 12.0f; // Distance from Z max stop down toward CR touch before starting slow probe.

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
volatile bool zHomingActive = false;

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

struct Entry {
  String name;
  bool isDir;
  uint32_t size;
};

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
static bool parseIntKey(const String &line, const char* key, int &out) {
  String k(key); k += "="; if (!line.startsWith(k)) return false;
  out = trimBoth(line.substring(k.length())).toInt(); return true;
}
static bool parseFloatCSV4(const String &line, const char* key, float out[4]) {
  String k(key); k += "="; if (!line.startsWith(k)) return false;
  String rest = line.substring(k.length());
  int idx = 0, start = 0;
  Serial.println("Parsing");
  Serial.println(line);
  while (idx < 4) {
    int comma = rest.indexOf(',', start);
    String tok = (comma < 0) ? rest.substring(start) : rest.substring(start, comma);
    tok.trim();
    Serial.println(tok);
    if (!tok.length()) break;
    Serial.println(tok.toFloat());
    out[idx++] = tok.toFloat();
    if (comma < 0) break;
    start = comma + 1;
  }
  return (idx == 4);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  ESP_SERIAL.begin(ESP_BAUD);
  PRINTHEAD_SERIAL.begin(PRINTHEAD_BAUD);
  
  pinMode(PIN_STEP, OUTPUT); 
  pinMode(PIN_DIR,  OUTPUT);
  pinMode(PIN_EN,   OUTPUT); 
  digitalWrite(PIN_EN, HIGH);

  pinMode(PIN_YL_STEP, OUTPUT);
  pinMode(PIN_YR_STEP, OUTPUT);
  pinMode(PIN_Y_DIR,   OUTPUT);
  pinMode(PIN_YR_DIR,   OUTPUT);
  
  pinMode(PIN_Z_STEP, OUTPUT); 
  pinMode(PIN_Z_DIR, OUTPUT);
  pinMode(PIN_Z_CONTACT, INPUT_PULLUP);
  attachPCINT(digitalPinToPCINT(PIN_Z_CONTACT), contactInterruptHandler, FALLING);

  pinMode(PIN_BTN_PLUS,  INPUT_PULLUP); 
  pinMode(PIN_BTN_MINUS, INPUT_PULLUP); 
  pinMode(PIN_BTN_ENTER, INPUT_PULLUP); 

  STEPS_PER_MM_X  = (MOTOR_STEPS_PER_REV * MICROSTEPS_X) / LEAD_MM_X;
  STEPS_PER_MM_Y  = (MOTOR_STEPS_PER_REV * MICROSTEPS_Y) / LEAD_MM_Y;
  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));

  Wire.begin(); 
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C); 
  display.clearDisplay(); 
  display.display();

  // SD
  SPI.begin();
  bool sdok = SD.begin(PIN_SD_CS);

  // Config
  if (sdok) {
    Serial.println("Reading config");
    writeDefaultConfigIfMissing(CONFIG_FILE_PATH);
    if (loadConfigFromSD(CONFIG_FILE_PATH)) applyConfig();
    else {
      // Derive with built-ins (manual rounding)
      STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
      OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));
      Serial.println(F("[CFG] No config; using built-ins"));
    }
  } else {
    Serial.println(F("[CFG] SD not available; using built-ins"));
    STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
    OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));
  }
}

void loop() {
  if (emergencyStopTriggered) handleEmergencyStop();
  menuMain();
  delay(10);
}
