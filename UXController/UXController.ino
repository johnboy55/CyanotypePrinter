/* ============================================================
 * UXController - ESP32-S3: web print API, live status, and the
 * print "brain" (image + LUT exposure + print-engine orchestration)
 * - Holds the image in PSRAM, drives MotorController (X/Y) and
 *   PrintHead (LED fire + Z + temp) over two independent serial
 *   links (see plan §4)
 * - Owns the OLED/buttons/SD-or-flash job storage moved off the
 *   old Mega (see plan Context, §3)
 * - Serves /print and /status (see plan §9)
 *
 * See plan: i-was-able-to-mellow-rocket.md
 * Board-link protocols are not designed yet -- see plan §4/§5/§8.
 * This is a scaffold, not a working sketch. MotorController_ESP32/
 * is left untouched as reference for the existing web-server code.
 * ============================================================ */

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
}
