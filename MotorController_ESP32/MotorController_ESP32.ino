/* ============================================================
 * UV Printhead - TIER 1: ESP32 Web Server
 * - Broadcasts standalone Wi-Fi AP ("Cyanotype_Printer")
 * - Hosts HTML/JS UI
 * - Converts JSON pixel arrays to Hexadecimal
 * - Streams commands to Arduino Mega via Hardware Serial2
 * ============================================================ */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// --- Network Settings ---
const char* ssid = "Cyanotype_Printer";
const char* password = "printmaker123"; // Must be at least 8 chars

// --- Serial Connection to Mega ---
// Standard ESP32 Serial2 pins: RX = 16, TX = 17
#define MEGA_SERIAL Serial2
#define MEGA_BAUD 115200

WebServer server(80);

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
    </style>
</head>
<body>
    <h1>Design Your Print</h1>
    <div class="subtitle">Draw with 16 shades of UV intensity</div>
    <div class="controls">
        <div class="button-group">
            <select id="preset-select">
                <option value="">-- Load Preset --</option>
                <option value="invader">Atari: Invader</option>
                <option value="star">Nintendo: Star</option>
                <option value="c64sword">C64: Sword</option>
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
    'invader': { width: 8, height: 8, pixels: [0,0,0,15,15,0,0,0, 0,0,15,15,15,15,0,0, 0,15,15,15,15,15,15,0, 15,15,0,15,15,0,15,15, 15,15,15,15,15,15,15,15, 0,0,15,0,0,15,0,0, 0,15,15,0,0,15,15,0, 15,15,0,0,0,0,15,15] },
    'star': { width: 8, height: 8, pixels: [0,0,0,15,15,0,0,0, 0,0,15,7,7,15,0,0, 0,15,7,7,7,7,15,0, 15,15,15,15,15,15,15,15, 0,0,15,7,7,15,0,0, 0,15,15,0,0,15,15,0, 15,15,0,0,0,0,15,15, 15,0,0,0,0,0,0,15] },
    'c64sword': { width: 8, height: 8, pixels: [0,0,0,0,0,0,15,0, 0,0,0,0,0,15,15,15, 0,0,0,0,15,15,15,0, 0,0,0,15,15,15,0,0, 0,0,15,15,15,0,0,0, 0,15,15,15,0,0,0,0, 15,15,0,0,0,0,0,0, 0,0,0,0,0,0,0,0] }
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

function init() {
    buildPalette();
    initGrid();
    setupEventListeners();
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

void handlePrint() {
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

  // Fire over UART to the Mega!
  // Format exactly what Mega expects: "P:w,h:HEXDATA\n"
  MEGA_SERIAL.print("P:");
  MEGA_SERIAL.print(w);
  MEGA_SERIAL.print(",");
  MEGA_SERIAL.print(h);
  MEGA_SERIAL.print(":");
  MEGA_SERIAL.print(hexData);
  MEGA_SERIAL.print("\n");

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

  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  
  // Pass any debug messages from the Mega straight to the PC monitor
  if (MEGA_SERIAL.available()) {
    Serial.write(MEGA_SERIAL.read());
  }
}