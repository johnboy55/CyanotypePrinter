/* ============================================================
 * UXController - ESP32-S3: web print API, live status, and the
 * print "brain" (image + LUT exposure + print-engine orchestration)
 *
 * See plan: i-was-able-to-mellow-rocket.md
 *
 * --- Current scope: MotorController link bench test ONLY ---
 * This is step 1 of the plan's de-risking order (§ "Suggested
 * de-risking order", item 1): validate the MotorController wire
 * protocol (see MotorController/MotorController.ino's header) with
 * dummy motion before anything else -- no LEDs, no PrintHead link,
 * no web server yet.
 *
 * Test harness: type a MotorController command (e.g. "MOVE:X,10",
 * "HOME:X", "STATUS") into this board's USB serial monitor. It's
 * forwarded verbatim to MotorController over MOTOR_SERIAL, and
 * whatever comes back (including any unsolicited FAULT: lines) is
 * printed back to the USB monitor. No web layer needed to validate
 * the link.
 * ============================================================ */

#include <Arduino.h>

// TODO confirm these against actual wiring -- placeholder UART pins for the
// MotorController link, kept off the default UART0 (USB/debug) pins.
#define MOTOR_SERIAL Serial1
const int PIN_MOTOR_RX = 18;
const int PIN_MOTOR_TX = 17;
const unsigned long MOTOR_BAUD = 115200;

const unsigned long ACK_TIMEOUT_MS = 3000; // generous for a bench test; homing can take a while

// Reads one newline-terminated line from MotorController, or returns "" on
// timeout. Blocking (matches MotorController's own one-command-at-a-time
// design) -- fine for a bench test; the real print loop will need this same
// helper later, just called from inside the orchestration loop.
String readMotorLine(unsigned long timeoutMs) {
  unsigned long start = millis();
  String line;
  while (millis() - start < timeoutMs) {
    while (MOTOR_SERIAL.available()) {
      char c = MOTOR_SERIAL.read();
      if (c == '\n') { line.trim(); return line; }
      line += c;
    }
  }
  return "";
}

// Sends a command line and waits for its ACK/STATUS reply, transparently
// logging (but not returning) any FAULT: lines that arrive first -- these are
// unsolicited and tagged separately so they're never mistaken for the ACK
// this call is actually waiting on (see MotorController's protocol comment).
String sendMotorCommand(const String &cmd) {
  MOTOR_SERIAL.print(cmd);
  MOTOR_SERIAL.print('\n');

  unsigned long start = millis();
  while (millis() - start < ACK_TIMEOUT_MS) {
    String line = readMotorLine(ACK_TIMEOUT_MS - (millis() - start));
    if (line.length() == 0) break; // timed out inside readMotorLine
    if (line.startsWith("FAULT:")) {
      Serial.print("[MotorController async] ");
      Serial.println(line);
      continue; // keep waiting for the actual reply
    }
    return line;
  }
  return "";
}

void setup() {
  Serial.begin(115200);
  MOTOR_SERIAL.begin(MOTOR_BAUD, SERIAL_8N1, PIN_MOTOR_RX, PIN_MOTOR_TX);

  Serial.println("UXController bench test -- MotorController link only.");
  Serial.println("Type a command (MOVE:X,<mm> / MOVE:Y,<mm> / HOME:X / HOME:Y / STATUS) and press Enter.");
}

String inBuffer;

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inBuffer.length() > 0) {
        String reply = sendMotorCommand(inBuffer);
        if (reply.length() == 0) Serial.println("(timeout -- no reply from MotorController)");
        else { Serial.print("-> "); Serial.println(reply); }
        inBuffer = "";
      }
    } else {
      inBuffer += c;
    }
  }
}
