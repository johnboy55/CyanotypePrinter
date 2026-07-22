/* ============================================================
 * MotorController - dumb serial motion interface (X/Y only)
 * - Simple Arduino + CNC Shield V3
 * - Owns X/Y limit switches directly (handled locally, faults
 *   reported up to the UX controller -- see plan §6)
 * - Z axis lives on PrintHead, not here
 *
 * See plan: i-was-able-to-mellow-rocket.md, §4 (protocol) / §6 (faults)
 *
 * --- Wire protocol (Serial, 115200, newline-terminated ASCII) ---
 * One command in flight at a time -- the UX controller sends a line
 * and blocks for the single ACK it produces, so there's no need for
 * request IDs the way a polling link would. Async FAULT lines are
 * tagged separately so they can never be mistaken for an ACK -- that
 * ambiguity is exactly what caused the old Mega<->Nano desync bug
 * (commit 2be9a86).
 *
 *   MOVE:X,<mm>   -> ACK:OK  | ACK:ERR:LIMIT
 *   MOVE:Y,<mm>   -> ACK:OK  | ACK:ERR:LIMIT
 *   HOME:X        -> ACK:OK  | ACK:ERR:TIMEOUT
 *   HOME:Y        -> ACK:OK  | ACK:ERR:TIMEOUT
 *   STATUS        -> STATUS:X=<mm>,Y=<mm>,LIMX=<0/1>,LIMY=<0/1>
 *   '!'           -> (single byte, checked mid-move) immediate stop
 *   FAULT:X / FAULT:Y  (unsolicited -- limit tripped while idle)
 *
 * This is a bench-test scaffold: dummy motion first, no LEDs, no
 * web layer. Pin map is the standard CNC Shield V3 / Uno GRBL
 * pinout -- TODO confirm against your actual wiring before flashing.
 * ============================================================ */

#include <Arduino.h>

// --- Pins (stock CNC Shield V3 + Uno/Nano GRBL pinout) ---
const int PIN_X_STEP = 2;
const int PIN_X_DIR  = 5;
const int PIN_Y_STEP = 3;
const int PIN_Y_DIR  = 6;
const int PIN_ENABLE = 8;
const int PIN_X_LIMIT = 9;   // INPUT_PULLUP, active LOW
const int PIN_Y_LIMIT = 10;  // INPUT_PULLUP, active LOW

// --- Motion tuning (placeholders -- unvalidated against real hardware) ---
const long STEPS_PER_REV     = 200;
const int  MICROSTEPS        = 16;
const float LEAD_MM          = 8.0f;
const float STEPS_PER_MM     = (STEPS_PER_REV * MICROSTEPS) / LEAD_MM;
const int  STEP_PULSE_US     = 5;
const int  STEP_PERIOD_US    = 1000;
const float MAX_HOMING_TRAVEL_MM = 500.0f; // safety bound if a limit switch never trips

float posXmm = 0.0f;
float posYmm = 0.0f;

// Edge-triggered so FAULT is only sent once per new trip, not spammed every loop.
bool lastLimitXAtRest = false;
bool lastLimitYAtRest = false;
bool moveInProgress = false;

String inBuffer;

// ============================================================
// --- Helpers ---
// ============================================================

inline bool limitXTriggered() { return digitalRead(PIN_X_LIMIT) == LOW; }
inline bool limitYTriggered() { return digitalRead(PIN_Y_LIMIT) == LOW; }

// Checked between every single step pulse -- '!' is a real-time byte (GRBL
// convention), not a queued line, so it can interrupt a move already underway.
bool stopRequested() {
  if (Serial.available() && Serial.peek() == '!') {
    Serial.read();
    return true;
  }
  return false;
}

void stepPulse(int stepPin) {
  digitalWrite(stepPin, HIGH);
  delayMicroseconds(STEP_PULSE_US);
  digitalWrite(stepPin, LOW);
  delayMicroseconds(max(0, STEP_PERIOD_US - STEP_PULSE_US));
}

// ============================================================
// --- Motion primitives ---
// ============================================================

// Returns true if the move completed fully; false if it was cut short by a
// limit switch or a real-time stop byte.
bool moveAxis(int stepPin, int dirPin, bool (*limitTriggered)(), float &posMm, float deltaMm) {
  if (deltaMm == 0.0f) return true;
  bool dir = deltaMm > 0;
  digitalWrite(dirPin, dir ? HIGH : LOW);
  delayMicroseconds(5); // DIR setup time

  digitalWrite(PIN_ENABLE, LOW);
  moveInProgress = true;

  long steps = lround(fabs(deltaMm) * STEPS_PER_MM);
  bool completed = true;
  for (long i = 0; i < steps; i++) {
    if (limitTriggered() || stopRequested()) { completed = false; break; }
    stepPulse(stepPin);
    posMm += dir ? (1.0f / STEPS_PER_MM) : -(1.0f / STEPS_PER_MM);
  }

  moveInProgress = false;
  return completed;
}

// Drives toward the limit switch until it trips, zeroing position on success.
// Direction convention: homing always moves in the negative direction, same
// as this project's existing Z-homing logic.
bool homeAxis(int stepPin, int dirPin, bool (*limitTriggered)(), float &posMm) {
  digitalWrite(dirPin, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_ENABLE, LOW);
  moveInProgress = true;

  long maxSteps = lround(MAX_HOMING_TRAVEL_MM * STEPS_PER_MM);
  bool found = false;
  for (long i = 0; i < maxSteps; i++) {
    if (limitTriggered()) { found = true; break; }
    if (stopRequested()) break;
    stepPulse(stepPin);
  }

  moveInProgress = false;
  if (found) posMm = 0.0f;
  return found;
}

// ============================================================
// --- Command handling ---
// ============================================================

void handleMove(char axis, float mm) {
  bool ok;
  if (axis == 'X') ok = moveAxis(PIN_X_STEP, PIN_X_DIR, limitXTriggered, posXmm, mm);
  else             ok = moveAxis(PIN_Y_STEP, PIN_Y_DIR, limitYTriggered, posYmm, mm);
  Serial.print(ok ? F("ACK:OK\n") : F("ACK:ERR:LIMIT\n"));
}

void handleHome(char axis) {
  bool ok;
  if (axis == 'X') ok = homeAxis(PIN_X_STEP, PIN_X_DIR, limitXTriggered, posXmm);
  else             ok = homeAxis(PIN_Y_STEP, PIN_Y_DIR, limitYTriggered, posYmm);
  Serial.print(ok ? F("ACK:OK\n") : F("ACK:ERR:TIMEOUT\n"));
}

void handleStatus() {
  Serial.print(F("STATUS:X="));
  Serial.print(posXmm, 2);
  Serial.print(F(",Y="));
  Serial.print(posYmm, 2);
  Serial.print(F(",LIMX="));
  Serial.print(limitXTriggered() ? 1 : 0);
  Serial.print(F(",LIMY="));
  Serial.print(limitYTriggered() ? 1 : 0);
  Serial.print(F("\n"));
}

void processLine(String line) {
  line.trim();
  if (line.startsWith(F("MOVE:"))) {
    char axis = line.charAt(5);
    float mm = line.substring(7).toFloat();
    handleMove(axis, mm);
  } else if (line.startsWith(F("HOME:"))) {
    handleHome(line.charAt(5));
  } else if (line == F("STATUS")) {
    handleStatus();
  } else if (line.length() > 0) {
    Serial.print(F("ACK:ERR:BADCMD\n"));
  }
}

// ============================================================
// --- Setup & loop ---
// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_X_STEP, OUTPUT);
  pinMode(PIN_X_DIR, OUTPUT);
  pinMode(PIN_Y_STEP, OUTPUT);
  pinMode(PIN_Y_DIR, OUTPUT);
  pinMode(PIN_ENABLE, OUTPUT);
  digitalWrite(PIN_ENABLE, HIGH); // disabled until the first move

  pinMode(PIN_X_LIMIT, INPUT_PULLUP);
  pinMode(PIN_Y_LIMIT, INPUT_PULLUP);

  inBuffer.reserve(32);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processLine(inBuffer);
      inBuffer = "";
    } else if (c != '!') { // '!' is consumed as a real-time byte, never buffered
      inBuffer += c;
    }
  }

  // Report a limit trip the instant it happens while nothing commanded it --
  // e.g. the gantry gets bumped at rest. A trip *during* a commanded move is
  // already reported via that move's own ACK:ERR:LIMIT, not duplicated here.
  if (!moveInProgress) {
    bool limX = limitXTriggered();
    bool limY = limitYTriggered();
    if (limX && !lastLimitXAtRest) Serial.print(F("FAULT:X\n"));
    if (limY && !lastLimitYAtRest) Serial.print(F("FAULT:Y\n"));
    lastLimitXAtRest = limX;
    lastLimitYAtRest = limY;
  }
}
