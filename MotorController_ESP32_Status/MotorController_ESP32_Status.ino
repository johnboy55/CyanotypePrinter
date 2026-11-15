/* ============================================================
 * UV Printhead - Status Page (ESP8266/ESP32, station mode)
 * Firmware variant: V1.0 Standard -- see /VERSIONS.md at repo root.
 * - Joins an existing Wi-Fi network (does NOT host its own AP)
 * - Hosts a read-only status page: Idle/Printing, row progress,
 *   and the printhead LED temperature
 * - Listens to the same Mega->ESP wire protocol already used by
 *   the pixel-art sketch (STATE:/PROGRESS:), plus a new TEMP: line
 * - Accepts a new exposure LUT over Wi-Fi (POST /lut, or the upload
 *   box on the status page) and hands it to the Mega, which saves it to
 *   /calibrations on the SD card and loads it -- see sendLutToMega()
 * - Sibling of MotorController_ESP32/ (the OS-1.0 OpenSauce variant's
 *   pixel-art design tool), kept in its own directory so that project
 *   is untouched and can be reflashed later -- only one of the two
 *   runs on the board at a time.
 * ============================================================ */

#define FW_VARIANT "V1.0-Standard"

#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#else
#include <WiFi.h>
#include <WebServer.h>
#endif
#include <ArduinoJson.h>

// --- Network Settings ---
// This sketch joins your existing Wi-Fi network (station mode), unlike the pixel-art
// sketch which hosts its own AP. Credentials live in secrets.h, which is git-ignored --
// copy secrets.h.example to secrets.h and fill it in before flashing.
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// --- Serial Connection to Mega ---
// Same physical UART link as the pixel-art sketch: on the Mega+WiFi board,
// the integrated ESP8266 talks to the ATmega2560 through the Mega's Serial3
// interface (pins 14 RX3 and 15 TX3). Use the ESP8266's main UART for this.
#if defined(ARDUINO_ARCH_ESP8266)
#define MEGA_SERIAL Serial
#else
#define MEGA_SERIAL Serial2
#endif
#define MEGA_BAUD 115200

#if defined(ARDUINO_ARCH_ESP8266)
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

// --- Mega link state ---
// Simplified compared to the pixel-art sketch's PrinterState: this page has
// no attendant-gated "online" concept (it never accepts a print submission),
// so STATE:ONLINE and STATE:OFFLINE both just mean "not printing".
enum PrinterState { PS_IDLE, PS_PRINTING };
PrinterState printerState = PS_IDLE;
int printProgressRow = 0;
int printProgressTotal = 0;
int ledTempC = -1;              // -1 = no reading yet
unsigned long lastTempMs = 0;   // millis() a TEMP: line last arrived, 0 = never
String megaInBuf = "";
unsigned long lastIpBroadcast = 0;
const unsigned long IP_BROADCAST_MS = 2000;

// Reply to the LUT-upload handshake (LUTACK / LUTOK:<file> / LUTERR:<why>), captured by
// handleMegaLine() while sendLutToMega() is waiting on it.
String lutReply = "";
bool lutReplyReady = false;

const char* stateName(PrinterState s) {
  return (s == PS_PRINTING) ? "PRINTING" : "IDLE";
}

void sendIpToMega() {
  MEGA_SERIAL.print("IP:");
  MEGA_SERIAL.println(WiFi.localIP());
}

void handleMegaLine(String line) {
  line.trim();
  if (line == "STATE:ONLINE" || line == "STATE:OFFLINE") {
    printerState = PS_IDLE;
  } else if (line == "STATE:PRINTING") {
    printerState = PS_PRINTING;
  } else if (line.startsWith("PROGRESS:")) {
    int comma = line.indexOf(',');
    if (comma > 0) {
      printProgressRow = line.substring(9, comma).toInt();
      printProgressTotal = line.substring(comma + 1).toInt();
    }
  } else if (line.startsWith("TEMP:")) {
    ledTempC = line.substring(5).toInt();
    lastTempMs = millis();
  } else if (line == "LUTACK" || line.startsWith("LUTOK:") || line.startsWith("LUTERR:")) {
    lutReply = line;
    lutReplyReady = true;
  }
}

// Reads whatever the Mega has sent and dispatches complete lines. Called from loop() and
// from inside the LUT handshake, so status lines arriving mid-handshake still land.
void pumpMegaSerial() {
  while (MEGA_SERIAL.available()) {
    char c = MEGA_SERIAL.read();
    if (c == '\n') {
      handleMegaLine(megaInBuf);
      megaInBuf = "";
    } else {
      megaInBuf += c;
    }
  }
}

// Sends one handshake line and waits for the Mega's reply. The Mega only services LUT
// lines from its main menu / ESP32 screen, so a timeout here means it's printing or
// sitting in a submenu.
bool lutStep(const String &out, String &reply) {
  lutReplyReady = false;
  MEGA_SERIAL.println(out);
  unsigned long start = millis();
  while (millis() - start < 3000) {
    pumpMegaSerial();
    if (lutReplyReady) { reply = lutReply; return true; }
    delay(2);
  }
  return false;
}

// Pushes the 16 (already validated) values to the Mega in 4 acked blocks -- see
// handleEspLutLine() in MotorController_Mega/uv_sd.cpp for the other side. Returns ""
// on success with savedFile set, else a human-readable error.
String sendLutToMega(const String &name, const float vals[16], String &savedFile) {
  String reply;
  if (!lutStep("LUTB:" + name, reply)) {
    return "printer didn't answer -- leave it on the main menu (not printing) and try again";
  }
  if (reply != "LUTACK") return "printer refused: " + reply.substring(7);
  for (int base = 0; base < 16; base += 4) {
    String line = "LUTD:" + String(base);
    for (int i = 0; i < 4; i++) line += "," + String(vals[base + i], 2);
    if (!lutStep(line, reply)) return "printer stopped answering mid-transfer";
    if (reply != "LUTACK") return "printer refused: " + reply.substring(7);
  }
  if (!lutStep("LUTE", reply)) return "printer didn't confirm the save";
  if (!reply.startsWith("LUTOK:")) return "printer refused: " + reply.substring(7);
  savedFile = reply.substring(6);
  return "";
}

// ============================================================
// --- FRONTEND (Injected as a Raw String Literal) ---
// ============================================================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Cyanotype Printer - Status</title>
    <style>
        body { font-family: sans-serif; text-align: center; background: #222; color: #fff; margin: 0; padding: 20px; }
        h1 { margin-bottom: 5px; }
        .subtitle { color: #aaa; margin-bottom: 24px; font-size: 14px; }

        #status-bar { max-width: 420px; margin: 0 auto 16px auto; padding: 14px 18px; border-radius: 8px; font-size: 20px; font-weight: bold; }
        #status-bar.state-idle     { background: #1e3a1e; color: #7CFC7C; }
        #status-bar.state-printing { background: #24284d; color: #9db4ff; }
        #status-bar.state-unknown  { background: #442222; color: #ff8888; }

        #progress-track { display: none; margin: 10px auto 0 auto; max-width: 420px; height: 10px; background: #111; border-radius: 5px; overflow: hidden; }
        #progress-fill { height: 100%; width: 0%; background: #4d79ff; transition: width 0.3s ease; }
        #progress-label { margin-top: 6px; font-size: 14px; color: #aaa; }

        #temp-card { max-width: 420px; margin: 24px auto 0 auto; padding: 16px; border-radius: 8px; background: #2a2a2a; }
        #temp-value { font-size: 42px; font-weight: bold; }
        #temp-value.hot { color: #ff8888; }
        #temp-value.normal { color: #7CFC7C; }
        #temp-value.unknown { color: #888; font-size: 24px; }
        #temp-label { color: #aaa; font-size: 14px; margin-top: 4px; }

        #lut-card { max-width: 420px; margin: 24px auto 0 auto; padding: 16px; border-radius: 8px; background: #2a2a2a; text-align: left; }
        #lut-card h2 { font-size: 16px; margin: 0 0 10px 0; }
        #lut-card input[type=text] { width: 100%; box-sizing: border-box; padding: 8px; font-size: 16px; margin: 8px 0; background: #111; color: #fff; border: 1px solid #444; border-radius: 6px; }
        #lut-card button { padding: 10px 14px; font-size: 15px; border-radius: 6px; border: none; background: #4d79ff; color: #fff; }
        #lut-msg { margin-top: 10px; font-size: 14px; }
        .ok { color: #7CFC7C; } .err { color: #ff8888; }
    </style>
</head>
<body>
    <h1>Cyanotype Printer</h1>
    <div class="subtitle">Live Status</div>

    <div id="status-bar" class="state-unknown">Checking printer...</div>
    <div id="progress-track"><div id="progress-fill"></div></div>
    <div id="progress-label"></div>

    <div id="temp-card">
        <div id="temp-value" class="unknown">--</div>
        <div id="temp-label">Printhead LED Temp</div>
    </div>

    <div id="lut-card">
        <h2>Load a LUT</h2>
        <input type="file" id="lut-file" accept=".txt,text/plain">
        <input type="text" id="lut-name" maxlength="19" placeholder="name (letters, numbers, _ -)">
        <button id="lut-send">Send to printer</button>
        <div id="lut-msg"></div>
    </div>

    <script>
        const statusBar = document.getElementById('status-bar');
        const progressTrack = document.getElementById('progress-track');
        const progressFill = document.getElementById('progress-fill');
        const progressLabel = document.getElementById('progress-label');
        const tempValue = document.getElementById('temp-value');

        // Temp readings older than this are treated as stale (Mega reset, link
        // dropped, etc.) rather than trusted at face value.
        const TEMP_STALE_MS = 10000;

        function pollStatus() {
            fetch('/status')
                .then(r => r.json())
                .then(data => {
                    statusBar.className = 'state-' + data.state.toLowerCase();
                    if (data.state === 'PRINTING') {
                        statusBar.textContent = 'Printing';
                        const pct = data.total > 0 ? Math.round((data.row / data.total) * 100) : 0;
                        progressTrack.style.display = 'block';
                        progressFill.style.width = pct + '%';
                        progressLabel.textContent = `Row ${data.row} of ${data.total} (${pct}%)`;
                    } else {
                        statusBar.textContent = 'Idle';
                        progressTrack.style.display = 'none';
                        progressLabel.textContent = '';
                    }

                    if (data.temp === null || data.tempAgeMs > TEMP_STALE_MS) {
                        tempValue.textContent = '--';
                        tempValue.className = 'unknown';
                    } else {
                        tempValue.textContent = data.temp + '°C';
                        tempValue.className = data.temp >= 100 ? 'hot' : 'normal';
                    }
                })
                .catch(() => {
                    statusBar.className = 'state-unknown';
                    statusBar.textContent = 'Cannot reach printer';
                    progressTrack.style.display = 'none';
                    progressLabel.textContent = '';
                    tempValue.textContent = '--';
                    tempValue.className = 'unknown';
                });
        }

        pollStatus();
        setInterval(pollStatus, 1500);

        const lutFile = document.getElementById('lut-file');
        const lutName = document.getElementById('lut-name');
        const lutMsg = document.getElementById('lut-msg');
        lutFile.onchange = () => {
            const f = lutFile.files[0];
            if (f && !lutName.value) lutName.value = f.name.replace(/\.txt$/i, '').replace(/[^A-Za-z0-9_-]+/g, '_').slice(0, 19);
        };
        document.getElementById('lut-send').onclick = async () => {
            const f = lutFile.files[0];
            if (!f) { lutMsg.className = 'err'; lutMsg.textContent = 'Choose a LUT .txt file first.'; return; }
            lutMsg.className = ''; lutMsg.textContent = 'Sending...';
            try {
                const r = await fetch('/lut?name=' + encodeURIComponent(lutName.value), { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: await f.text() });
                const d = await r.json();
                lutMsg.className = d.ok ? 'ok' : 'err';
                lutMsg.textContent = d.ok ? ('Saved and loaded ' + d.file) : d.error;
            } catch (e) { lutMsg.className = 'err'; lutMsg.textContent = 'Request failed: ' + e; }
        };
    </script>
</body>
</html>
)rawliteral";

// ============================================================
// --- Web Server Endpoints ---
// ============================================================

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleStatus() {
  StaticJsonDocument<192> doc;
  doc["variant"] = FW_VARIANT;
  doc["state"] = stateName(printerState);
  if (printerState == PS_PRINTING) {
    doc["row"] = printProgressRow;
    doc["total"] = printProgressTotal;
  }
  if (lastTempMs == 0) {
    doc["temp"] = nullptr;
    doc["tempAgeMs"] = -1;
  } else {
    doc["temp"] = ledTempC;
    doc["tempAgeMs"] = millis() - lastTempMs;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void sendJsonError(int code, const String &msg) {
  StaticJsonDocument<192> doc;
  doc["ok"] = false;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

// POST /lut?name=<name>   body: the LUT file text (16 comma-separated seconds)
// No authentication -- this sketch is LAN-only by design.
void handleLutUpload() {
  String name = server.arg("name");
  name.trim();
  if (name.length() == 0 || name.length() > 19) { sendJsonError(400, "name must be 1-19 characters"); return; }
  for (unsigned i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') { sendJsonError(400, "name may only use letters, numbers, _ and -"); return; }
  }
  if (printerState == PS_PRINTING) { sendJsonError(409, "printer is busy printing -- try again when it's idle"); return; }

  // Same rules as the calibration tool's lut_format.py, checked here too so a bad file
  // gets a clear error in the browser instead of a terse refusal from the Mega.
  String body = server.arg("plain");
  float vals[16];
  int n = 0, start = 0;
  while (n < 17) {
    int comma = body.indexOf(',', start);
    String tok = (comma < 0) ? body.substring(start) : body.substring(start, comma);
    tok.trim();
    if (tok.length() == 0) break;
    char *end;
    float v = strtof(tok.c_str(), &end);
    if (*end != '\0') { sendJsonError(400, "LUT has a non-numeric value: " + tok); return; }
    if (n < 16) vals[n] = v;
    n++;
    if (comma < 0) break;
    start = comma + 1;
  }
  if (n != 16) { sendJsonError(400, "LUT must have exactly 16 comma-separated values, found " + String(n)); return; }
  if (fabs(vals[0]) > 0.005f) { sendJsonError(400, "LUT entry 0 must be 0"); return; }
  for (int i = 1; i < 16; i++) {
    if (vals[i] < vals[i - 1]) { sendJsonError(400, "LUT values must not decrease"); return; }
  }
  if (vals[15] <= 0.0f || vals[15] > 600.0f) { sendJsonError(400, "LUT max must be between 0 and 600 s"); return; }

  String savedFile;
  String err = sendLutToMega(name, vals, savedFile);
  if (err.length()) { sendJsonError(503, err); return; }

  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["file"] = savedFile;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ============================================================
// --- Setup & Loop ---
// ============================================================

void setup() {
  Serial.begin(115200);         // Debug to PC
  MEGA_SERIAL.begin(MEGA_BAUD);  // UART to Mega

  Serial.print("\n");
  Serial.println(FW_VARIANT);
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. Open Browser to: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/lut", HTTP_POST, handleLutUpload);
  server.begin();
  Serial.println("Web server started.");

  megaInBuf.reserve(32);
  sendIpToMega();
  lastIpBroadcast = millis();
}

void loop() {
  server.handleClient();

  // Parse status lines from the Mega (STATE:/PROGRESS:/TEMP:/LUT replies).
  pumpMegaSerial();

  // Keep the Mega's displayed IP fresh, same cadence as the pixel-art sketch.
  if (millis() - lastIpBroadcast > IP_BROADCAST_MS) {
    sendIpToMega();
    lastIpBroadcast = millis();
  }
}
