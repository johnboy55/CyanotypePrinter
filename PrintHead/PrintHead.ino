/* ============================================================
 * PrintHead - LED timing, temp monitoring, Z axis, CR Touch
 * - Expanded from PrintHead_Nano's role: gains the Z stepper and
 *   closed-loop "maintain distance from paper" behavior
 * - Owns the Z limit switch (handled locally, reported up to the
 *   UX controller -- see plan §6)
 * - Exposes exactly two Z primitives to the UX controller:
 *   go-to-distance (used with every LED fire) and
 *   report-distance (used during bed mapping) -- see plan §5
 *
 * See plan: i-was-able-to-mellow-rocket.md
 * Wire protocol to the UX controller (ESP32-S3) is not designed
 * yet -- see plan §4/§5/§8. This is a scaffold, not a working
 * sketch. PrintHead_Nano/ is left untouched as reference.
 * ============================================================ */

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
}
