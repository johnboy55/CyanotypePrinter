/* ============================================================
 * MotorController - dumb serial motion interface (X/Y only)
 * - Simple Arduino + CNC Shield V3
 * - Owns X/Y limit switches directly (handled locally, reported
 *   up to the UX controller -- see plan §6)
 * - Z axis lives on PrintHead, not here
 *
 * See plan: i-was-able-to-mellow-rocket.md
 * Protocol to/from the UX controller (ESP32-S3) is not designed
 * yet -- see plan §4. This is a scaffold, not a working sketch.
 * ============================================================ */

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
}
