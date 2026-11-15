# Hardware & Firmware Versions

This project has one hardware generation in active use (**V1.0**), running one of two
firmware variants. A physically different **V2.0** hardware generation is planned but
not started -- everything below describes V1.0 only. When V2.0 work begins, it should
get its own section here (or its own file) rather than overwriting this one.

## V1.0 Hardware (both variants)

Two boards, unchanged between firmware variants:

**Board 1 -- Arduino Mega + built-in ESP8266**
- An Arduino Mega 2560 board with an integrated ESP8266 co-processor (a "Mega+WiFi"
  board). The ESP8266 talks to the ATmega2560 over the Mega's `Serial3` (RX3 = pin 15,
  TX3 = pin 14) -- this is the link the PC sees as a COM port (COM3) when the board is
  plugged in for flashing/debugging.
- CNC Shield V3 on top, driving the X/Y/Z stepper drivers.
- A custom IO board handling the rest of the printer's UI/storage: SSD1306 OLED,
  SPI SD card module, and the physical menu buttons.
- Firmware: [MotorController_Mega/](MotorController_Mega/) (ATmega2560 side, same for
  both variants) plus one of the two ESP8266 sketches below (only one is ever flashed
  to the ESP8266 at a time).

**Board 2 -- Arduino Nano + custom printhead PCB**
- Arduino Nano on a custom PCB carrying: 4x UV LED drivers, 2x TMP36 temperature
  sensors (A0/A1 -- T[0] is the printhead LED temp with the thermal cutoff, T[1]
  spare/ambient), one NeoPixel status indicator, an optional I2C status OLED
  (A4/A5 SDA/SCL -- 128x32 SSD1306 by default, no-ops if not wired), and the CR
  Touch probe interface (servo + signal line).
- D4, D9, A2, and A3 are unpopulated on V1.0.
- Firmware: [PrintHead_Nano/](PrintHead_Nano/) -- identical across both variants; it
  only ever talks to the Mega over UART and has no notion of which ESP8266 sketch is
  running.
- Z-axis stepper control does **not** live here on V1.0 -- the Mega's CNC Shield drives
  Z directly. [PrintHead_Nano/Z_AXIS_WIRING.md](PrintHead_Nano/Z_AXIS_WIRING.md) is
  forward-looking wiring notes for **V2.0**, where the Nano (or its V2.0 replacement
  MCU) takes over the Z stepper itself; it doesn't describe current V1.0 firmware and
  isn't implemented in `PrintHead_Nano.ino`.

The full inter-board protocol (ESP<->Mega JSON/hex payload, Mega<->Nano UART command
table) is documented in [Readme.md](Readme.md).

## Firmware variants

Same hardware, two interchangeable ESP8266 sketches. Flash whichever one matches how
the printer is meant to be used; the Mega and Nano firmware don't change either way.

| | **V1.0 (Standard)** | **OS-1.0 (OpenSauce)** |
|---|---|---|
| ESP8266 sketch | [MotorController_ESP32_Status/](MotorController_ESP32_Status/) | [MotorController_ESP32/](MotorController_ESP32/) |
| Wi-Fi role | Joins an existing network (station mode) | Hosts its own Access Point (`Cyanotype_Printer`) |
| What it serves | Read-only status page: idle/printing, row progress, printhead LED temp | Pixel-art design webapp (`/`, `/app.js`) that posts prints directly (`/print`) |
| Intended use | Normal operation: attendant drives prints from the Mega's own menu/SD card; the web page is just a monitor | Kiosk-style operation: walk-up users design and submit their own small print from a phone/tablet, no attendant-driven SD workflow |
| Print area | Whatever the Mega/paper setup allows | Constrained to a 50mm x 50mm bed (`MESH_MAX_X`/`MESH_MAX_Y` in the Mega firmware), tuned for many quick individual prints |

Both sketches share the same `STATE:`/`PROGRESS:`/`IP:` wire protocol back to the Mega;
the Standard variant adds `TEMP:` (printhead LED temperature) since that's the one
thing its status page shows that the OpenSauce variant's webapp doesn't.

### Naming note

"OS-1.0" here is short for **OpenSauce**, not "operating system" -- if that reads as
ambiguous, an alternative would be renaming the pair to something like
`V1.0-Standard` / `V1.0-OpenSauce` (and the folders to match, e.g.
`MotorController_ESP8266_Status/` / `MotorController_ESP8266_OpenSauce/`, since both
sketches actually target the ESP8266 -- the `ESP32` in the current folder names is
legacy from early bring-up on ESP32 dev boards before the Mega+WiFi board's built-in
ESP8266 became the real target. Not renamed yet since that's a bigger structural change
(Arduino requires a sketch's folder and `.ino` file to share a name); flag if you'd
like that cleanup done.

## What's next (V2.0)

V2.0 is a planned hardware redesign, not yet started. Keep V1.0/OS-1.0 firmware changes
isolated from it -- don't let V2.0 experiments leak into these directories until it has
its own home. Known V2.0 direction so far:

- The Nano (or a different MCU running the same printhead functions) takes over Z-axis
  stepper control itself, instead of the Mega's CNC Shield driving it directly -- see
  [PrintHead_Nano/Z_AXIS_WIRING.md](PrintHead_Nano/Z_AXIS_WIRING.md).
