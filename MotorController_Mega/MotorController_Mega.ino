#include "uv_globals.h"
#include "uv_sd.h"
#include "uv_print.h"
#include "uv_menu.h"

// --- Actual Global Definitions ---
Config CFG; 
SdFat SD;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

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
const int PIN_Z_CONTACT = A8;

const float Z_HOME_SWITCH_TO_PAPER_MM = 8.0f;
const float Z_MAX_TO_CR_TOUCH_APPROACH_MM = 12.0f;

// Change these three lines in main.ino:
Btn btnPlus  = {26, LOW, 0, false, false}; // Changed HIGH to LOW
Btn btnMinus = {27, LOW, 0, false, false}; // Changed HIGH to LOW
Btn btnEnter = {28, LOW, 0, false, false}; // Changed HIGH to LOW

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

long currentSteps = 0; 
long currentStepsY = 0;
long currentStepsZ = 0; 

const int MESH_SIZE = 3; 
float z_mesh[3][3];
bool mesh_active = false;
const float MESH_MAX_X = 50.0f; 
const float MESH_MAX_Y = 50.0f;

volatile bool emergencyStopTriggered = false;
volatile bool zHomingActive = false;
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

void handleEmergencyStop() {
  // Add your emergency stop logic here (was missing from source dump)
  Serial.println("E-STOP TRIGGERED!");
}

void contactInterruptHandler() {
  // Add your pin change interrupt logic here (was missing from source dump)
  emergencyStopTriggered = true; 
}

void setup() {
  Serial.begin(115200);
  ESP_SERIAL.begin(ESP_BAUD);
  PRINTHEAD_SERIAL.begin(PRINTHEAD_BAUD);
  
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
  pinMode(PIN_Z_CONTACT, INPUT_PULLUP);
  attachPCINT(digitalPinToPCINT(PIN_Z_CONTACT), contactInterruptHandler, FALLING);

  pinMode(btnPlus.pin, INPUT_PULLUP); 
  pinMode(btnMinus.pin, INPUT_PULLUP); 
  pinMode(btnEnter.pin, INPUT_PULLUP);


  STEPS_PER_MM_X  = (MOTOR_STEPS_PER_REV * MICROSTEPS_X) / LEAD_MM_X;
  STEPS_PER_MM_Y  = (MOTOR_STEPS_PER_REV * MICROSTEPS_Y) / LEAD_MM_Y;
  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C); 
  display.clearDisplay(); display.display();

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
  menuMain();
  delay(10);
}