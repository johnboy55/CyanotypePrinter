#ifndef UV_GLOBALS_H
#define UV_GLOBALS_H

// Must be defined before Arduino.h/HardwareSerial.h are first pulled in (this header is
// that first touchpoint for every .cpp in the sketch). Default AVR RX buffer is only 64
// bytes with no flow control, giving a little headroom against a blocking wait elsewhere
// (e.g. queryNanoLimitMask's poll of the Nano) stalling ESP_SERIAL reads.
// IMPORTANT: keep this <=256. Past 256, HardwareSerial.h widens its ring-buffer index
// from uint8_t to uint16_t -- but only on AVR cores new enough to have that adaptive
// typedef. On an older core, a >256 buffer with 8-bit indices wraps at 256 while the
// array is actually larger, desyncing the ring buffer and losing *more* data than the
// default, not less. This is real headroom (2x default) without touching that boundary;
// the actual fix for large payloads is not blocking ESP_SERIAL reads for so long (see
// queryNanoLimitMask's timeout), not a bigger buffer.
#define SERIAL_RX_BUFFER_SIZE 128

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <U8g2lib.h>
#include <stdarg.h>
#include <stdio.h>
#include <PinChangeInterrupt.h>
#include <AccelStepper.h>
#include "uv_types.h"

// 2.42" SSD1309 128x64, hardware I2C, full-frame-buffer mode (matches the old
// Adafruit_SSD1306 "draw everything then push once" usage pattern). If the picture
// comes out mirrored/garbled on your specific panel, try the NONAME2 variant instead --
// SSD1309 clone boards commonly need one or the other depending on their init sequence.
typedef U8G2_SSD1309_128X64_NONAME0_F_HW_I2C OledDisplay;

#define FW_VERSION "UV Controller v2.2.0"

#define PRINTHEAD_SERIAL Serial1
#define PRINTHEAD_BAUD 115200
// ESP-side link on the Mega+WiFi board is routed through Serial3.
#define ESP_SERIAL Serial3
#define ESP_BAUD 115200

// --- Globals ---
extern SdFat SD;
extern OledDisplay display;

extern const int PIN_SD_CS;
extern const int PIN_STEP, PIN_DIR, PIN_EN;
extern const int PIN_YL_STEP, PIN_YR_STEP, PIN_Y_DIR, PIN_YR_DIR;
extern const int PIN_Z_STEP, PIN_Z_DIR;
// Single shared interrupt pin driven by the Nano's PIN_ESTOP_OUT: goes LOW when the
// Nano's Z limit, Y limit, or thermal cutoff fires (OR'd together on the Nano side).
extern const int PIN_NANO_ESTOP;
extern const int PIN_BUZZER;
// Spare pins on the bare header, immediately after btnBack (29): 30-37.
// TODO confirm these against actual wiring before relying on them.
extern const int PIN_ALERT_BUZZER; // 30 -- attendant-facing "print incoming" / success chime
extern const int PIN_VAC_RELAY_1;  // 31 -- vacuum blower fan 1 (active-LOW relay driver)
extern const int PIN_VAC_RELAY_2;  // 32 -- vacuum blower fan 2 (active-LOW relay driver)

// The relay board is active-LOW: pull the pin LOW to energize the relay (fan on).
// Every fan on/off toggle should go through this rather than raw digitalWrite, so the
// polarity only ever needs correcting in one place.
inline void setVacRelay(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

extern const float Z_HOME_SWITCH_TO_PAPER_MM;
extern const float Z_MAX_TO_CR_TOUCH_APPROACH_MM;

extern Btn btnPlus;
extern Btn btnMinus;
extern Btn btnEnter;
extern Btn btnBack; // Dedicated back/cancel button (pin 29)

extern const int MOTOR_STEPS_PER_REV;
extern int MICROSTEPS_X, MICROSTEPS_Y;
extern float LEAD_MM_X, LEAD_MM_Y;
extern float STEPS_PER_MM_X, STEPS_PER_MM_Y, STEPS_PER_MM_Z;
extern float MM_PER_PIXEL;
extern int STEPS_PER_PIXEL, OFFSET_PIXELS;
extern float LED_SPACING_MM;
extern int STEP_PULSE_US;
// Z lead screw pacing (bit-banged, unlike X/Y -- see uv_print.cpp's moveZSteps/homeZ/
// probeZ). Unrelated to X/Y's AccelStepper speed/accel; matches the old default X/Y
// STEP_PERIOD_US (1000us) that Z's delay had always effectively used.
extern const int Z_STEP_PERIOD_US;
extern const float PAPER_LOAD_Y_MM;

// X/Y motion. stepN()/yStepPair() drive these via .move()+.run(); Z stays bit-banged
// (see moveZSteps/homeZ/probeZ in uv_print.cpp) -- only X/Y needed the acceleration ramp.
extern AccelStepper stepperX;
extern AccelStepper stepperYL;
extern AccelStepper stepperYR;
// Applies CFG.maxSpeed{X,Y}mm/CFG.accel{X,Y}mm (in mm/s, mm/s^2) to the three steppers
// above. Called once from setup() and again from applyConfig() whenever the SD config
// reloads or the new OLED motion-speed menu saves a change.
void applyMotionConfig();

extern long currentSteps, currentStepsY, currentStepsZ;
// Raw-step X origin the print engine (doPrintBMPSliceSerpentine) sweeps from/to for each
// row, instead of hardcoded absolute 0. Set to the print-start offset just before a print
// begins and reset to 0 once it's done, so the offset holds for every row of the print,
// not just the first (X gets re-driven to an absolute target every row; Y never does, so
// it doesn't need this -- physically moving Y to the offset before printing is enough).
extern long printOriginStepsX;
extern const int MESH_SIZE; 
extern float z_mesh[3][3];
extern bool mesh_active;
extern const float MESH_MAX_X, MESH_MAX_Y;

extern volatile bool emergencyStopTriggered; // Raw ISR flag; classified/consumed by handleEmergencyStop()
extern volatile bool zHomingActive;          // True while a Z-contact touch is expected (homing/probing)
extern bool eStopLatched;                    // True once a genuine e-stop is confirmed; requires operator clear
extern String eStopCause;                    // Human-readable cause of the current latch, e.g. "Z", "Y", "OVERTEMP"
extern bool cancelRequested;                 // Operator hit Back mid-move/print; abort the current operation only
// Whether the attendant has put the printer "online" from the ESP32-mode menu. Tracked
// globally (not just local to menuESP32Control) so the print engine -- which also runs
// for plain SD-card prints, entirely outside that menu -- knows what state to report
// back to the ESP32 once a print finishes: PRINTING always reverts to this.
extern bool espOnline;
extern float LUT16[16];
extern float LED_SCALE[4];

// Physical stack inverted (top=0 .. bottom=3). With LED4 at bottom:
extern int ROW_FOR_LED[4];


// Utility functions
void serialPrintf(const char *fmt, ...);
long lround_double(double x);
void handleEmergencyStop();
void contactInterruptHandler();
void buzzReady();
void buzzPrintIncoming(); // Attendant alert: a validated print arrived, go seat paper
void buzzPrintSuccess();  // Print finished successfully

#endif