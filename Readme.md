## I. System Architecture

The system operates across three distinct tiers, separating the user interface, the motion control, and the real-time hardware execution.

**TIER 1: THE NETWORK HUB (ESP32)**

- **Input:** Receives HTTP POST (JSON) from the User's Web Browser.
- **Core Functions:** 
  - Broadcasts a standalone Wi-Fi Access Point.
  - Hosts the `index.html` and `app.js` frontend files.
  - Converts the JSON pixel arrays into hexadecimal strings.
- **Output:** Sends data down to Tier 2 via Hardware Serial2 (115200 Baud).

**TIER 2: THE MAIN BRAIN (Arduino Mega 2560)**

- **Hardware Attached:** CNC Shield V3 (X, Y, Z Steppers), SSD1306 OLED Screen, SPI SD Card Module, and physical UI buttons.
- **Core Functions:** 
  - Parses the hex image data from the ESP32.
  - Calculates the Mesh Bed Leveling (MBL) interpolation.
  - Orchestrates the X/Y physical motion path.
- **Output:** Sends real-time hardware triggers down to Tier 3 via Hardware Serial1 (115200 Baud).

**TIER 3: THE PRINTHEAD MCU (Arduino Nano Custom PCB)**

- **Hardware Attached:** 4x UV LEDs (D10-D13), CR Touch (D3 Servo, D7 Signal), 6x TMP36 Temp Sensors (A0-A5), and a NeoPixel Status LED (D8).
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

## III. Protocol: Mega to Printhead (The Hardware UART Dictionary)

The Arduino Nano operates as a strict, non-blocking peripheral. It sits on Serial1 at 115200 baud, listening for newline-terminated ASCII commands. It immediately executes them and responds with a status string and a NeoPixel color flash.

**Command Reference Table**

| Command | Action | Expected Return | NeoPixel Visual |
|---|---|---|---|
| `S\n` | Status Poll: Asks the Nano which LEDs are currently firing. | `S:[mask]` (e.g., `S:15` for all, `S:0` for none) | Dim White → Green |
| `F:m,d0,d1,d2,d3\n` | Fire LEDs: Sends the bitmask (m) and microsecond durations (dX) for all 4 LEDs. | *(None, must poll with S to check when finished)* | Purple → Green |
| `D\n` | Deploy Probe: Commands the CR Touch servo to drop the pin. | *(None)* | Cyan → Green |
| `U\n` | Stow Probe: Commands the CR Touch servo to pull the pin up. | *(None)* | Cyan → Green |
| `X\n` | Reset Probe: Commands CR Touch to clear red alarm states. | *(None)* | Cyan → Green |
| `P\n` | Probe State: Checks if the CR Touch signal pin is triggered. | `P:1` (Triggered) or `P:0` (Open) | Cyan → Green |
| `T\n` | Thermal Poll: Requests background temperature readings from the 6 sensors. | `T:v0,v1,v2,v3,v4,v5` *(Values in Celsius)* | Dim White → Green |
| `R\n` | Emergency Reset: Immediately kills all active UV LEDs and resets timers. | *(None)* | Orange → Green |
