#ifndef UV_GLOBALS_H
#define UV_GLOBALS_H

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>  
#include <Adafruit_SSD1306.h>
#include <stdarg.h>
#include <stdio.h>
#include <PinChangeInterrupt.h>
#include "uv_types.h"

#define FW_VERSION "UV Controller v2.2.0"

#define PRINTHEAD_SERIAL Serial1
#define PRINTHEAD_BAUD 115200
#define ESP_SERIAL Serial3 
#define ESP_BAUD 115200

// --- Globals ---
extern SdFat SD;
extern Adafruit_SSD1306 display;

extern const int PIN_SD_CS;     
extern const int PIN_STEP, PIN_DIR, PIN_EN;       
extern const int PIN_YL_STEP, PIN_YR_STEP, PIN_Y_DIR, PIN_YR_DIR;    
extern const int PIN_Z_STEP, PIN_Z_DIR, PIN_Z_CONTACT;   

extern const float Z_HOME_SWITCH_TO_PAPER_MM;
extern const float Z_MAX_TO_CR_TOUCH_APPROACH_MM;

extern Btn btnPlus;
extern Btn btnMinus;
extern Btn btnEnter;

extern const int MOTOR_STEPS_PER_REV;
extern int MICROSTEPS_X, MICROSTEPS_Y;
extern float LEAD_MM_X, LEAD_MM_Y;
extern float STEPS_PER_MM_X, STEPS_PER_MM_Y, STEPS_PER_MM_Z;
extern float MM_PER_PIXEL;
extern int STEPS_PER_PIXEL, OFFSET_PIXELS;
extern float LED_SPACING_MM;
extern int STEP_PULSE_US, STEP_PERIOD_US;

extern long currentSteps, currentStepsY, currentStepsZ;
extern const int MESH_SIZE; 
extern float z_mesh[3][3];
extern bool mesh_active;
extern const float MESH_MAX_X, MESH_MAX_Y;

extern volatile bool emergencyStopTriggered;
extern volatile bool zHomingActive;
extern float LUT16[16];
extern float LED_SCALE[4];

// Physical stack inverted (top=0 .. bottom=3). With LED4 at bottom:
extern int ROW_FOR_LED[4];


// Utility functions
void serialPrintf(const char *fmt, ...);
long lround_double(double x);
void handleEmergencyStop();
void contactInterruptHandler();

#endif