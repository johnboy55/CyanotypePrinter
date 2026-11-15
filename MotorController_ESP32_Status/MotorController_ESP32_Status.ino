/* ============================================================
 * UV Printhead - Status Page (ESP8266/ESP32, station mode)
 * Firmware variant: V1.0 Standard -- see /VERSIONS.md at repo root.
 * - Joins an existing Wi-Fi network (does NOT host its own AP)
 * - Hosts a read-only status page: Idle/Printing, row progress,
 *   and the printhead LED temperature
 * - Listens to the same Mega->ESP wire protocol already used by
 *   the pixel-art sketch (STATE:/PROGRESS:), plus a new TEMP: line
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
  }
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
  server.begin();
  Serial.println("Web server started.");

  megaInBuf.reserve(32);
  sendIpToMega();
  lastIpBroadcast = millis();
}

void loop() {
  server.handleClient();

  // Parse status lines from the Mega (STATE:/PROGRESS:/TEMP:).
  while (MEGA_SERIAL.available()) {
    char c = MEGA_SERIAL.read();
    if (c == '\n') {
      handleMegaLine(megaInBuf);
      megaInBuf = "";
    } else {
      megaInBuf += c;
    }
  }

  // Keep the Mega's displayed IP fresh, same cadence as the pixel-art sketch.
  if (millis() - lastIpBroadcast > IP_BROADCAST_MS) {
    sendIpToMega();
    lastIpBroadcast = millis();
  }
}
