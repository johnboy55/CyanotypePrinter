/* ============================================================
 * UV Printhead - TIER 1: ESP8266 Web Server
 * - Broadcasts standalone Wi-Fi AP ("Cyanotype_Printer")
 * - Hosts HTML/JS UI
 * - Converts JSON pixel arrays to Hexadecimal
 * - Streams commands to Arduino Mega via serial
 * ============================================================ */

#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#else
#include <WiFi.h>
#include <WebServer.h>
#endif
#include <ArduinoJson.h>

// --- Network Settings ---
const char* ssid = "Cyanotype_Printer";
const char* password = "printmaker123"; // Must be at least 8 chars

// --- Serial Connection to Mega ---
// On the Mega+WiFi board, the integrated ESP8266 talks to the ATmega2560
// through the Mega's Serial3 interface (pins 14 RX3 and 15 TX3).
// Use the ESP8266's main UART for this link.
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
// The Mega drives this from its ESP32-mode menu / print engine. OFFLINE: nobody's
// attending the printer. ONLINE: attendant is watching, idle, ready for a submission.
// PRINTING: physically mid-print -- true whether this print was triggered from the web
// UI or a plain SD-card print, so the web status bar can't go stale while the machine
// is busy either way. Only ONLINE accepts a new /print submission.
enum PrinterState { PS_OFFLINE, PS_ONLINE, PS_PRINTING };
PrinterState printerState = PS_OFFLINE;
int printProgressRow = 0;
int printProgressTotal = 0;
String megaInBuf = "";
unsigned long lastIpBroadcast = 0;
const unsigned long IP_BROADCAST_MS = 2000;

const char* stateName(PrinterState s) {
  switch (s) {
    case PS_ONLINE:   return "ONLINE";
    case PS_PRINTING: return "PRINTING";
    default:          return "OFFLINE";
  }
}

void sendIpToMega() {
  MEGA_SERIAL.print("IP:");
  MEGA_SERIAL.println(WiFi.softAPIP());
}

void handleMegaLine(String line) {
  line.trim();
  if (line == "STATE:ONLINE") {
    printerState = PS_ONLINE;
  } else if (line == "STATE:OFFLINE") {
    printerState = PS_OFFLINE;
  } else if (line == "STATE:PRINTING") {
    printerState = PS_PRINTING;
  } else if (line.startsWith("PROGRESS:")) {
    int comma = line.indexOf(',');
    if (comma > 0) {
      printProgressRow = line.substring(9, comma).toInt();
      printProgressTotal = line.substring(comma + 1).toInt();
    }
  }
  // "finish"/"error" acks are superseded by the STATE:ONLINE/OFFLINE the Mega now sends
  // right after every print -- nothing left to do with them here.
}

// ============================================================
// --- FRONTEND FILES (Injected as Raw String Literals) ---
// ============================================================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Cyanotype Printer</title>
    <style>
        body { font-family: sans-serif; text-align: center; background: #222; color: #fff; margin: 0; padding: 20px; touch-action: none; }
        h1 { margin-bottom: 5px; }
        .subtitle { color: #aaa; margin-bottom: 20px; font-size: 14px; }
        
        .controls { margin-bottom: 20px; display: flex; flex-direction: column; align-items: center; gap: 10px; }
        .button-group { display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; }
        select, button { padding: 10px 20px; font-size: 16px; border-radius: 5px; border: none; cursor: pointer; }
        button.print-btn { background: #007bff; color: white; font-weight: bold; }
        
        #palette { display: flex; justify-content: center; flex-wrap: wrap; max-width: 400px; margin: 0 auto 20px auto; gap: 5px; }
        .swatch { width: 40px; height: 40px; border-radius: 50%; cursor: pointer; border: 2px solid #222; transition: transform 0.1s; }
        .swatch.active { border: 2px solid #00ff00; transform: scale(1.1); }
        
        #canvas-container { display: flex; justify-content: center; margin-bottom: 20px; }
        #grid { display: grid; background: #fff; border: 2px solid #555; touch-action: none; }
        .cell { width: 30px; height: 30px; border: 1px solid #ccc; box-sizing: border-box; }
        
        #status { font-size: 18px; font-weight: bold; color: #00ff00; min-height: 25px; }

        #printer-status-bar { max-width: 400px; margin: 0 auto 20px auto; padding: 8px 14px; border-radius: 6px; font-size: 14px; font-weight: bold; text-align: left; }
        #printer-status-bar.state-offline  { background: #442222; color: #ff8888; }
        #printer-status-bar.state-online   { background: #1e3a1e; color: #7CFC7C; }
        #printer-status-bar.state-printing { background: #24284d; color: #9db4ff; }
        #printer-progress-track { display: none; margin-top: 6px; height: 8px; background: #111; border-radius: 4px; overflow: hidden; }
        #printer-progress-fill { height: 100%; width: 0%; background: #4d79ff; transition: width 0.3s ease; }
        button:disabled { opacity: 0.5; cursor: not-allowed; }
    </style>
</head>
<body>
    <h1>Design Your Print</h1>
    <div class="subtitle">Draw with 16 shades of UV intensity</div>
    <div id="printer-status-bar" class="state-offline">
        <span id="printer-state-text">Checking printer...</span>
        <div id="printer-progress-track"><div id="printer-progress-fill"></div></div>
    </div>
    <div class="controls">
        <div class="button-group">
            <select id="preset-select">
                <option value="">-- Load Preset --</option>
                <option value="Invader">Atari: Invader</option>
                <option value="Mario">Nintendo: Mario</option>
                <option value="Link">Nintento:Link</option>
                <option value="Ghost">Atari: Ghost</option>
            </select>
            <select id="size-select">
                <option value="8x8">8 x 8</option>
                <option value="8x16">8 x 16</option>
                <option value="16x16">16 x 16</option>
                <option value="8x32">8 x 32</option>
            </select>
        </div>
        <div class="button-group">
            <button class="print-btn" id="print-btn">PRINT DESIGN</button>
            <button id="clear-btn">Clear Canvas</button>
        </div>
    </div>
    <div id="palette"></div>
    <div id="canvas-container">
        <div id="grid"></div>
    </div>
    <div id="status"></div>
    <script src="app.js"></script>
</body>
</html>
)rawliteral";

const char app_js[] PROGMEM = R"rawliteral(
// --- State Variables ---
let currentColor = 15; 
let isDrawing = false;
let gridData = [];
let currentW = 8;
let currentH = 8;

const presets = {
    'Invader': { width: 8, height: 8, pixels: [0,0,0,15,15,0,0,0, 0,0,15,15,15,15,0,0, 0,15,15,15,15,15,15,0, 15,15,0,15,15,0,15,15, 15,15,15,15,15,15,15,15, 0,0,15,0,0,15,0,0, 0,15,15,0,0,15,15,0, 15,15,0,0,0,0,15,15] },
    'Mario': { width: 16, height: 16, pixels: [2,2,2,2,2,15,15,15,15,15,2,2,2,2,2,2,2,2,2,2,15,15,15,15,15,15,15,15,15,2,2,2,2,2,2,2,13,13,13,0,0,13,0,2,2,2,2,2,2,2,2,13,0,13,0,0,0,13,0,0,0,2,2,2,2,2,2,13,0,13,13,0,0,0,13,0,0,0,2,2,2,2,2,13,13,0,0,0,0,13,13,13,13,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,13,13,15,13,13,13,2,2,2,2,2,2,2,2,2,13,13,13,15,13,13,15,13,13,13,2,2,2,2,2,13,13,13,13,15,15,15,15,13,13,13,13,2,2,2,2,0,0,13,15,0,15,15,0,15,13,0,0,2,2,2,2,0,0,0,15,15,15,15,15,15,0,0,0,2,2,2,2,0,0,15,15,15,15,15,15,15,15,0,0,2,2,2,2,2,2,15,15,15,2,2,15,15,15,2,2,2,2,2,2,2,13,13,13,2,2,2,2,13,13,13,2,2,2,2,2,13,13,13,13,2,2,2,2,13,13,13,13,2,2] },
    'Link': { width: 16, height: 16, pixels: [13,13,13,13,13,2,2,2,2,2,2,13,13,13,13,13,13,13,13,13,2,2,2,2,2,2,2,2,13,13,13,13,13,13,0,13,2,15,15,15,15,15,15,2,13,0,13,13,13,13,0,13,15,15,15,15,15,15,15,15,13,0,13,13,13,13,0,0,15,0,2,0,0,2,0,15,0,0,13,13,13,13,0,0,15,0,15,0,0,15,0,15,0,0,13,13,13,13,13,0,0,0,0,0,0,0,0,0,0,15,13,13,13,13,13,2,2,0,0,15,15,0,0,2,2,15,13,13,13,15,15,15,15,15,0,0,0,0,2,2,15,15,15,13,15,15,0,15,15,15,15,2,2,2,2,2,0,15,15,13,15,0,0,0,15,15,0,15,15,2,2,0,0,0,15,13,15,15,0,15,15,15,0,2,15,15,15,15,0,0,0,13,15,15,0,15,15,15,0,15,15,2,2,2,2,0,13,13,15,15,15,15,15,15,0,2,2,2,2,2,13,13,13,13,13,0,0,0,0,0,15,13,13,15,15,15,13,13,13,13,13,13,13,13,15,15,15,13,13,13,13,13,13,13,13,13] },
    'Ghost': { width: 8, height: 8, pixels: [0,0,15,15,15,15,0,0, 0,15,15,15,15,15,15,0, 15,0,0,15,15,0,0,15, 15,0,15,15,15,0,15,15, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,0,15,0,15,0,15,0] }
};

const urlParams = new URLSearchParams(window.location.search);
const isSimulated = urlParams.has('sim') && urlParams.get('sim') !== 'false';
const paletteContainer = document.getElementById('palette');
const gridContainer = document.getElementById('grid');
const sizeSelect = document.getElementById('size-select');
const presetSelect = document.getElementById('preset-select');
const printBtn = document.getElementById('print-btn');
const clearBtn = document.getElementById('clear-btn');
const statusDiv = document.getElementById('status');
const printerStatusBar = document.getElementById('printer-status-bar');
const printerStateText = document.getElementById('printer-state-text');
const printerProgressTrack = document.getElementById('printer-progress-track');
const printerProgressFill = document.getElementById('printer-progress-fill');

function init() {
    buildPalette();
    initGrid();
    setupEventListeners();
    if (!isSimulated) {
        pollPrinterStatus();
        setInterval(pollPrinterStatus, 1500);
    }
}

// Polls the Mega's live state (relayed through the ESP32) so the page reflects
// Online/Offline/Printing plus row progress without the operator refreshing anything.
function pollPrinterStatus() {
    fetch('/status')
        .then(r => r.json())
        .then(data => {
            printerStatusBar.className = 'state-' + data.state.toLowerCase();
            if (data.state === 'PRINTING') {
                const pct = data.total > 0 ? Math.round((data.row / data.total) * 100) : 0;
                printerStateText.textContent = `Printing: row ${data.row} of ${data.total} (${pct}%)`;
                printerProgressTrack.style.display = 'block';
                printerProgressFill.style.width = pct + '%';
                printBtn.disabled = true;
            } else if (data.state === 'ONLINE') {
                printerStateText.textContent = 'Printer online -- ready for a print';
                printerProgressTrack.style.display = 'none';
                printBtn.disabled = false;
            } else {
                printerStateText.textContent = 'Printer offline -- attendant is not watching it';
                printerProgressTrack.style.display = 'none';
                printBtn.disabled = true;
            }
        })
        .catch(() => {
            printerStatusBar.className = 'state-offline';
            printerStateText.textContent = 'Cannot reach printer';
            printerProgressTrack.style.display = 'none';
        });
}

function buildPalette() {
    paletteContainer.innerHTML = '';
    for(let i = 0; i <= 15; i++) {
        let swatch = document.createElement('div');
        swatch.className = 'swatch';
        let rgb = Math.floor(255 - (i * (255/15))); 
        swatch.style.background = `rgb(${rgb},${rgb},${rgb})`;
        if(i === 15) swatch.classList.add('active');
        swatch.onclick = (e) => {
            document.querySelectorAll('.swatch').forEach(s => s.classList.remove('active'));
            e.target.classList.add('active');
            currentColor = i;
        };
        paletteContainer.appendChild(swatch);
    }
}

function initGrid(fillData = null) {
    if (!fillData) {
        const size = sizeSelect.value.split('x');
        currentW = parseInt(size[0]);
        currentH = parseInt(size[1]);
        gridData = new Array(currentW * currentH).fill(0);
    } else {
        gridData = [...fillData]; 
    }
    gridContainer.style.gridTemplateColumns = `repeat(${currentW}, 30px)`;
    gridContainer.style.gridTemplateRows = `repeat(${currentH}, 30px)`;
    gridContainer.innerHTML = '';

    for(let i = 0; i < currentW * currentH; i++) {
        let cell = document.createElement('div');
        cell.className = 'cell';
        cell.dataset.index = i;
        if (fillData) paintCellByValue(cell, gridData[i]);
        
        cell.addEventListener('pointerdown', (e) => {
            isDrawing = true;
            paintCell(e.target);
            e.target.releasePointerCapture(e.pointerId);
        });
        cell.addEventListener('pointerenter', (e) => {
            if(isDrawing) paintCell(e.target);
        });
        gridContainer.appendChild(cell);
    }
}

function paintCell(cell) {
    let index = cell.dataset.index;
    gridData[index] = currentColor;
    paintCellByValue(cell, currentColor);
}

function paintCellByValue(cell, value) {
    let rgb = Math.floor(255 - (value * (255/15)));
    cell.style.background = `rgb(${rgb},${rgb},${rgb})`;
    cell.style.borderColor = `rgb(${rgb},${rgb},${rgb})`; 
}

function loadPreset(presetKey) {
    if (!presets[presetKey]) return;
    const preset = presets[presetKey];
    currentW = preset.width;
    currentH = preset.height;
    sizeSelect.value = `${currentW}x${currentH}`;
    initGrid(preset.pixels);
}

function setupEventListeners() {
    sizeSelect.addEventListener('change', () => { presetSelect.value = ""; initGrid(); });
    presetSelect.addEventListener('change', (e) => { if(e.target.value !== "") loadPreset(e.target.value); });
    clearBtn.addEventListener('click', () => initGrid());
    printBtn.addEventListener('click', sendToPrinter);
    document.addEventListener('pointerup', () => { isDrawing = false; });
    
    gridContainer.addEventListener('touchmove', (e) => {
        e.preventDefault(); 
        if(!isDrawing) return;
        let touch = e.touches[0];
        let element = document.elementFromPoint(touch.clientX, touch.clientY);
        if(element && element.classList.contains('cell')) paintCell(element);
    }, {passive: false});
}

function sendToPrinter() {
    const payload = { width: currentW, height: currentH, pixels: gridData };
    if (isSimulated) return;

    statusDiv.innerText = "Sending...";
    statusDiv.style.color = "#ffff00"; 

    fetch('/print', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(response => {
        if(response.ok) {
            statusDiv.innerText = "Print Started!";
            statusDiv.style.color = "#00ff00";
            setTimeout(() => { statusDiv.innerText = ""; }, 3000);
        } else {
            throw new Error('Printer busy or error code: ' + response.status);
        }
    })
    .catch(error => {
        console.error("Fetch Error:", error);
        statusDiv.innerText = "Connection Error.";
        statusDiv.style.color = "#ff0000";
    });
}
init();
)rawliteral";

// ============================================================
// --- Web Server Endpoints ---
// ============================================================

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleJS() {
  server.send(200, "application/javascript", app_js);
}

void handleStatus() {
  StaticJsonDocument<128> doc;
  doc["state"] = stateName(printerState);
  if (printerState == PS_PRINTING) {
    doc["row"] = printProgressRow;
    doc["total"] = printProgressTotal;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- Chunked stop-and-wait transfer to the Mega ---
// Sending the whole hex payload in one burst risks overrunning the Mega's small hardware
// serial buffer if it's blocked elsewhere (no flow control) when the burst lands. Sending
// <=32 hex chars at a time and waiting for an explicit ack before the next chunk means no
// single burst can ever be more than one small chunk, regardless of how long the Mega
// takes to get back to us.
const int MEGA_CHUNK_SIZE = 32;

// Blocks (with periodic yield() so the WiFi stack doesn't trip a watchdog reset) until a
// full line arrives from the Mega or timeoutMs elapses. Returns false on timeout.
bool waitForMegaLine(String &out, unsigned long timeoutMs) {
  unsigned long start = millis();
  out = "";
  while (millis() - start < timeoutMs) {
    while (MEGA_SERIAL.available()) {
      char c = MEGA_SERIAL.read();
      if (c == '\n') { out.trim(); return true; }
      out += c;
    }
    yield();
  }
  return false;
}

// Runs the PSTART/D/PEND handshake. Returns false on any timeout or Mega-reported error;
// the caller should then treat the print as never having reached the Mega.
bool sendPrintChunked(int w, int h, const String &hexData) {
  MEGA_SERIAL.print("PSTART:"); MEGA_SERIAL.print(w); MEGA_SERIAL.print(",");
  MEGA_SERIAL.print(h); MEGA_SERIAL.print(","); MEGA_SERIAL.print(hexData.length());
  MEGA_SERIAL.print("\n");

  String resp;
  if (!waitForMegaLine(resp, 2000) || resp != "READY") return false;

  int total = hexData.length();
  for (int sent = 0; sent < total; ) {
    int n = min(MEGA_CHUNK_SIZE, total - sent);
    MEGA_SERIAL.print("D:");
    MEGA_SERIAL.print(hexData.substring(sent, sent + n));
    MEGA_SERIAL.print("\n");
    if (!waitForMegaLine(resp, 1000) || resp != "OK") return false;
    sent += n;
  }

  MEGA_SERIAL.print("PEND\n");
  return true; // Mega now owns the rest of the flow (attendant confirm, print, homing).
}

void handlePrint() {
  if (printerState == PS_OFFLINE) {
    server.send(503, "text/plain", "Printer is offline -- attendant hasn't gone online yet");
    return;
  }
  if (printerState == PS_PRINTING) {
    server.send(503, "text/plain", "Printer is busy with another print");
    return;
  }

  if (server.hasArg("plain") == false) {
    server.send(400, "text/plain", "Body not received");
    return;
  }

  // Parse JSON
  String jsonString = server.arg("plain");
  StaticJsonDocument<4096> doc; // Enough space for our grids
  DeserializationError error = deserializeJson(doc, jsonString);

  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  int w = doc["width"];
  int h = doc["height"];
  JsonArray pixels = doc["pixels"];

  if (w <= 0 || h <= 0 || pixels.isNull()) {
    server.send(400, "text/plain", "Invalid print parameters");
    return;
  }

  // Convert array to Hex String
  String hexData = "";
  hexData.reserve(w * h + 1); // Pre-allocate memory

  for (int v : pixels) {
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    hexData += String(v, HEX); // Converts 0-15 to 0-F seamlessly
  }

  // Mark PRINTING optimistically so a second submission can't sneak in during the
  // (possibly long) wait for the attendant to confirm on the Mega; the Mega always
  // reports back its definitive state once it's actually resolved this one, correcting
  // this if the attendant ends up declining the print.
  printerState = PS_PRINTING;
  printProgressRow = 0;
  printProgressTotal = 0;

  if (!sendPrintChunked(w, h, hexData)) {
    // Never reached the Mega (or it aborted mid-transfer) -- it never entered PRINTING,
    // so we're still just idle-online, not actually busy.
    printerState = PS_ONLINE;
    server.send(502, "text/plain", "Failed to transfer print to Mega");
    return;
  }

  server.send(200, "text/plain", "Print command dispatched!");
}

// ============================================================
// --- Setup & Loop ---
// ============================================================

void setup() {
  Serial.begin(115200);      // Debug to PC
  MEGA_SERIAL.begin(MEGA_BAUD); // UART to Mega
  
  // Setup Wi-Fi Access Point
  Serial.println("\nStarting Wi-Fi Access Point...");
  WiFi.softAP(ssid, password);
  
  Serial.print("Connect to Wi-Fi: ");
  Serial.println(ssid);
  Serial.print("Open Browser to: http://");
  Serial.println(WiFi.softAPIP());

  // Bind server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/app.js", HTTP_GET, handleJS);
  server.on("/print", HTTP_POST, handlePrint);
  server.on("/status", HTTP_GET, handleStatus);

  server.begin();
  Serial.println("Web server started.");

  megaInBuf.reserve(32);
  sendIpToMega();
  lastIpBroadcast = millis();
}

void loop() {
  server.handleClient();

  // Parse commands from the Mega (ONLINE/OFFLINE, finish/error acks).
  while (MEGA_SERIAL.available()) {
    char c = MEGA_SERIAL.read();
    if (c == '\n') {
      handleMegaLine(megaInBuf);
      megaInBuf = "";
    } else {
      megaInBuf += c;
    }
  }

  // Keep the Mega's displayed IP fresh even if it entered ESP32 mode after boot.
  if (millis() - lastIpBroadcast > IP_BROADCAST_MS) {
    sendIpToMega();
    lastIpBroadcast = millis();
  }
}