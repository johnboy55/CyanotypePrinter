#include "uv_globals.h"
#include "uv_sd.h"
#include "uv_print.h"
#include "uv_menu.h"

// --- Actual Global Definitions ---
Config CFG; 
SdFat SD;
OledDisplay display(U8G2_R0, /*reset=*/ U8X8_PIN_NONE);

const int PIN_SD_CS = 53;     
const int PIN_STEP = 2;       
const int PIN_DIR  = 5;
const int PIN_EN   = 8;       
const int PIN_YL_STEP = 3;    
const int PIN_YR_STEP = 4;
const int PIN_Y_DIR   = 6;    
const int PIN_YR_DIR  = 7;    
const int PIN_Z_STEP  = 12;
const int PIN_Z_DIR   = 13;
const int PIN_NANO_ESTOP = A8; // Shared Nano e-stop line (Z limit / Y limit / overtemp), see uv_globals.h
const int PIN_BUZZER = 9;      // Piezo buzzer: alerts operator that an ESP32-triggered print is staged and waiting on [Enter]

// Spare pins on the bare header, immediately after btnBack (29): 30-37.
// TODO confirm these against actual wiring before relying on them.
const int PIN_ALERT_BUZZER = 30; // Attendant-facing "print incoming" / success chime
const int PIN_VAC_RELAY_1  = 31; // Vacuum blower fan 1 (active-LOW relay driver)
const int PIN_VAC_RELAY_2  = 32; // Vacuum blower fan 2 (active-LOW relay driver)

const float Z_HOME_SWITCH_TO_PAPER_MM = 8.0f;
const float Z_MAX_TO_CR_TOUCH_APPROACH_MM = 12.0f;

// Change these three lines in main.ino:
Btn btnPlus  = {26, LOW, 0, false, false}; // Changed HIGH to LOW
Btn btnMinus = {27, LOW, 0, false, false}; // Changed HIGH to LOW
Btn btnEnter = {28, LOW, 0, false, false}; // Changed HIGH to LOW
Btn btnBack  = {29, LOW, 0, false, false};

const int MOTOR_STEPS_PER_REV = 200;
int MICROSTEPS_X = 16;
int MICROSTEPS_Y = 16;
float LEAD_MM_X = 8.0f;
float LEAD_MM_Y = 4.0f;
float STEPS_PER_MM_X = 400.0f;
float STEPS_PER_MM_Y = 320.0f;
float STEPS_PER_MM_Z = 400.0f; 
float MM_PER_PIXEL = 2.0f;
int STEPS_PER_PIXEL = 0;              
float LED_SPACING_MM = 7.0f;
int OFFSET_PIXELS = 0;
int STEP_PULSE_US = 5;                 
int STEP_PERIOD_US = 1000;
const float PAPER_LOAD_Y_MM = 80.0f;

long currentSteps = 0;
long currentStepsY = 0;
long currentStepsZ = 0;
long printOriginStepsX = 0;



const int MESH_SIZE = 3; 
float z_mesh[3][3];
bool mesh_active = false;
const float MESH_MAX_X = 50.0f; 
const float MESH_MAX_Y = 50.0f;

volatile bool emergencyStopTriggered = false;
volatile bool zHomingActive = false;
bool eStopLatched = false;
String eStopCause = "";
bool cancelRequested = false;
bool espOnline = false;
float LUT16[16] = { 0.00, 0.10, 0.20, 0.30, 0.40, 0.55, 0.70, 0.90, 1.10, 1.30, 1.60, 2.00, 2.50, 3.20, 4.00, 5.00 };
float LED_SCALE[4] = {1.0, 1.0, 1.0, 1.0}; // Provide actual definitions
int ROW_FOR_LED[4] = {3, 2, 1, 0};

// --- Core Helper Functions ---
void serialPrintf(const char *fmt, ...) {
  char buf[128]; va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
  Serial.print(buf);
}

long lround_double(double x) { return (x >= 0.0) ? (long)(x + 0.5) : (long)(x - 0.5); }

void buzzReady() {
  tone(PIN_BUZZER, 2000, 150); delay(200);
  tone(PIN_BUZZER, 2000, 150); delay(200);
  noTone(PIN_BUZZER);
}

// Three short high beeps: a print arrived and validated -- go seat paper on the vacuum bed.
void buzzPrintIncoming() {
  for (int i = 0; i < 3; i++) { tone(PIN_ALERT_BUZZER, 2500, 120); delay(180); }
  noTone(PIN_ALERT_BUZZER);
}

// Rising three-note chime: print finished successfully.
void buzzPrintSuccess() {
  tone(PIN_ALERT_BUZZER, 1500, 150); delay(180);
  tone(PIN_ALERT_BUZZER, 2000, 150); delay(180);
  tone(PIN_ALERT_BUZZER, 2500, 250); delay(280);
  noTone(PIN_ALERT_BUZZER);
}

// handleEmergencyStop() is defined in uv_print.cpp -- it needs PRINTHEAD_SERIAL to ask
// the Nano which limit fired, so it lives alongside the other Nano UART helpers.

void contactInterruptHandler() {
  // ISR: keep this minimal (no Serial/SD/delay). Just latch the raw signal; classification
  // (real e-stop vs. an expected homing touch) happens later in handleEmergencyStop().
  emergencyStopTriggered = true;
}

void setup() {
  Serial.begin(115200);

  // ESP8266 / ESP-side link on the Mega+WiFi board uses Serial3
  // (RX3 = pin 15, TX3 = pin 14) for inter-processor communication.
  ESP_SERIAL.begin(ESP_BAUD);
  PRINTHEAD_SERIAL.begin(PRINTHEAD_BAUD);
  // Bound blocking reads on the printhead link. Every readStringUntil('\n') otherwise
  // waits the Stream default (1000ms) whenever a newline is lost to noise/corruption --
  // turning one bad byte into a visible one-second stall per status poll. A complete
  // reply is ~5 bytes (<1ms at 115200), so 50ms is generous for a healthy link while
  // keeping a corrupted exchange from dominating the per-pixel cadence.
  PRINTHEAD_SERIAL.setTimeout(50);
  
  pinMode(PIN_STEP, OUTPUT); 
  pinMode(PIN_DIR, OUTPUT); 
  pinMode(PIN_EN, OUTPUT); 
  digitalWrite(PIN_EN, HIGH);
  pinMode(PIN_YL_STEP, OUTPUT); 
  pinMode(PIN_YR_STEP, OUTPUT);
  pinMode(PIN_Y_DIR, OUTPUT); 
  pinMode(PIN_YR_DIR, OUTPUT);
  
  pinMode(PIN_Z_STEP, OUTPUT);
  pinMode(PIN_Z_DIR, OUTPUT);
  pinMode(PIN_NANO_ESTOP, INPUT_PULLUP);
  attachPCINT(digitalPinToPCINT(PIN_NANO_ESTOP), contactInterruptHandler, FALLING);

  pinMode(btnPlus.pin, INPUT_PULLUP);
  pinMode(btnMinus.pin, INPUT_PULLUP);
  pinMode(btnEnter.pin, INPUT_PULLUP);
  pinMode(btnBack.pin, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_ALERT_BUZZER, OUTPUT);
  pinMode(PIN_VAC_RELAY_1, OUTPUT); setVacRelay(PIN_VAC_RELAY_1, false);
  pinMode(PIN_VAC_RELAY_2, OUTPUT); setVacRelay(PIN_VAC_RELAY_2, false);


  STEPS_PER_MM_X  = (MOTOR_STEPS_PER_REV * MICROSTEPS_X) / LEAD_MM_X;
  STEPS_PER_MM_Y  = (MOTOR_STEPS_PER_REV * MICROSTEPS_Y) / LEAD_MM_Y;
  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));

  Wire.begin();
  display.begin();
  // Match Adafruit_GFX's "y = top of glyph" convention so every existing setCursor(x,y)
  // call in the menu code keeps working unchanged instead of landing on the baseline.
  display.setFontPosTop();
  display.setFont(u8g2_font_6x10_tf);
  display.clearBuffer(); display.sendBuffer();

  SPI.begin();
  if (SD.begin(PIN_SD_CS)) {
    Serial.println("Reading config");
    writeDefaultConfigIfMissing(CONFIG_FILE_PATH);
    if (loadConfigFromSD(CONFIG_FILE_PATH)) applyConfig();
    else {
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
  if (eStopLatched) {
    renderEmergencyStopScreen();
    delay(10);
    return;
  }
  menuMain();
  delay(10);
}