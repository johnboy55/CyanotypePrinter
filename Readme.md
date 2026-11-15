## 0. Hardware & Firmware Versions

This repo currently targets one hardware generation, **V1.0**, in two interchangeable
firmware variants (**V1.0 Standard** and **OS-1.0 "OpenSauce"**). A V2.0 hardware
redesign is planned but not started. See [VERSIONS.md](VERSIONS.md) for the full
breakdown of what's on each board and which firmware folder goes with which variant --
the architecture below applies to both.

## I. System Architecture

The system operates across three distinct tiers, separating the user interface, the motion control, and the real-time hardware execution.

**TIER 1: THE NETWORK HUB (ESP8266, on the Mega+WiFi board)**

- **Chip:** The ESP8266 built into the Mega+WiFi board, talking to the ATmega2560 over
  the Mega's `Serial3` (the same link exposed to a PC as COM3). Two interchangeable
  sketches exist for this chip -- only one is ever flashed at a time:
  - [MotorController_ESP32_Status/](MotorController_ESP32_Status/) -- **V1.0 Standard**: joins the
    existing Wi-Fi network and serves a read-only status page (state, print progress,
    printhead LED temp). Doesn't accept print submissions.
  - [MotorController_ESP32/](MotorController_ESP32/) -- **OS-1.0 OpenSauce**: hosts its own
    Wi-Fi Access Point and serves the pixel-art design webapp described below. The rest
    of this section (HTTP POST, AP, `/print`) describes this variant.
- **Input:** Receives HTTP POST (JSON) from the User's Web Browser.
- **Core Functions:** 
  - Broadcasts a standalone Wi-Fi Access Point.
  - Hosts the `index.html` and `app.js` frontend files.
  - Converts the JSON pixel arrays into hexadecimal strings.
- **Output:** Sends data down to Tier 2 via Hardware Serial3 (115200 Baud).

**TIER 2: THE MAIN BRAIN (Arduino Mega 2560)**

- **Hardware Attached:** CNC Shield V3 (X, Y, Z Steppers), SSD1306 OLED Screen, SPI SD Card Module, and physical UI buttons.
- **Core Functions:** 
  - Parses the hex image data from the ESP32.
  - Calculates the Mesh Bed Leveling (MBL) interpolation.
  - Orchestrates the X/Y physical motion path.
- **Output:** Sends real-time hardware triggers down to Tier 3 via Hardware Serial1 (115200 Baud).

**TIER 3: THE PRINTHEAD MCU (Arduino Nano Custom PCB)**

- **Hardware Attached:** 4x UV LEDs (D10-D13), CR Touch (D3 Servo, D7 Signal), 2x TMP36 Temp Sensors (A0-A1), a NeoPixel Status LED (D8), and an optional I2C status OLED (A4/A5 SDA/SCL). D4, D9, A2, and A3 are unpopulated on V1.0.
- **Core Functions:** 
  - Operates as a real-time peripheral device (never blocks the UART line).
  - Executes microsecond-precise UV exposure timers.
  - Manages the CR Touch deployment and background thermal polling.

## II. Protocol: Website to ESP32 (The Payload)

The frontend uses a simple, modern REST approach to send the user's design to the ESP32.

**1. The HTTP POST Request**

When the user clicks "Print," the JavaScript fetch API fires a JSON payload to the ESP32's `/print` endpoint.

- **Format:** JSON
- **Structure:**
  - **width** (int): The width of the image in pixels.
  - **height** (int): The height of the image in pixels.
  - **pixels** (array of ints): A flat, 1D array representing the pixel intensities. Each pixel is an integer from 0 (off) to 15 (max intensity).

**2. ESP32 to Mega Translation**

The ESP32 intercepts this JSON, strips out the network overhead, and compresses the pixel array into a single, continuous hexadecimal string to save RAM on the Mega. It sends this over hardware UART.

- **Format:** `P:[width],[height]:[HexData]\n`
- **Example:** `P:8,8:000F0000FF00FF00...`
- **Mechanics:** 0 stays 0, 15 becomes F. The Mega reads this string, unpacks the hex characters back into integers, and loads them into its local memory for serpentine printing.

**3. Exposure LUT upload (V1.0 Standard sketch only)**

`MotorController_ESP32_Status` accepts a new 16-entry exposure LUT over Wi-Fi and hands it to the Mega, which saves it as `/calibrations/<name>.txt` on the SD card and makes it the active LUT. The calibration web app on the Pi (`Calibration/web/`) uses this, and the status page has an upload box too. There is no authentication: it's LAN-only.

- **HTTP:** `POST /lut?name=<name>`. The body is the LUT file text: 16 comma-separated seconds. The name can be 1–19 characters, using only letters, numbers, `_` and `-`. Entry 0 must be 0, values must not decrease, and the max must be ≤ 600 s.
- **Reply:** `{"ok":true,"file":"<name>.txt"}`, or `{"ok":false,"error":"..."}` with HTTP 400 for a bad LUT, 409 while printing, or 503 if the Mega didn't answer.
- **ESP → Mega UART:** stop-and-wait, and every line is acknowledged, so no burst gets near the Mega's 128-byte RX buffer:
  - `LUTB:<name>` → `LUTACK`
  - `LUTD:<i>,<v>,<v>,<v>,<v>` for i = 0, 4, 8, 12 → `LUTACK` each
  - `LUTE` → `LUTOK:<name>.txt` once saved and loaded
  - Any step can instead answer `LUTERR:<reason>`, which abandons the transfer.
- The Mega only services these lines from the **main menu** or the **ESP32 screen**. Mid-print or deep in a submenu it doesn't read them, so the ESP times out after 3 s and returns an error saying to leave the printer on the main menu.

## III. Protocol: Mega to Printhead (The Hardware UART Dictionary)

The Arduino Nano operates as a strict, non-blocking peripheral. It sits on Serial1 at 115200 baud, listening for newline-terminated ASCII commands. It immediately executes them and responds with a status string and a NeoPixel color flash.

**Command Reference Table**

| Command | Action | Expected Return | NeoPixel Visual |
|---|---|---|---|
| `S\n` | Status Poll: Asks the Nano which LEDs are currently firing. | `S:[mask],[temp0]` (e.g., `S:15,42` for all firing at 42C, `S:0,42` for none firing) -- temp0 lets the Mega pick up the printhead LED temp off the same reply that confirms firing is done, no separate `T` needed | Dim White → Green |
| `F:m,d0,d1,d2,d3\n` | Fire LEDs: Sends the bitmask (m) and microsecond durations (dX) for all 4 LEDs. | *(None, must poll with S to check when finished)* | Purple → Green |
| `D\n` | Deploy Probe: Commands the CR Touch servo to drop the pin. | *(None)* | Cyan → Green |
| `U\n` | Stow Probe: Commands the CR Touch servo to pull the pin up. | *(None)* | Cyan → Green |
| `X\n` | Reset Probe: Commands CR Touch to clear red alarm states. | *(None)* | Cyan → Green |
| `P\n` | Probe State: Checks if the CR Touch signal pin is triggered. | `P:1` (Triggered) or `P:0` (Open) | Cyan → Green |
| `T\n` | Thermal Poll: Requests background temperature readings from the 2 sensors. | `T:v0,v1` *(Values in Celsius; v0 is the printhead LED temp, with the thermal cutoff)* | Dim White → Green |
| `R\n` | Emergency Reset: Immediately kills all active UV LEDs and resets timers. | *(None)* | Orange → Green |
