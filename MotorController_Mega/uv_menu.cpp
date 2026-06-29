#include "uv_menu.h"
#include "uv_print.h" // Since menus call motion/print functions
#include "uv_sd.h"    // Since menus call SD loaders

void displayPrintf(Adafruit_SSD1306 &disp, const char *fmt, ...) {
  char buf[64]; va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
  disp.print(buf);
}

bool readButton(Btn &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.last && (millis() - b.lastChange) > 35) { // DEBOUNCE_MS
    b.last = raw; b.lastChange = millis();
    if ((b.activeHigh && raw==HIGH) || (!b.activeHigh && raw==LOW)) b.pressedEvent = true;
  }
  if (b.pressedEvent) { b.pressedEvent = false; return true; }
  return false;
}

uint8_t hexCharToVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a'); return 0;
}

bool buttonHeld(const Btn &b) { 
  bool raw = digitalRead(b.pin);
  return b.activeHigh ? (raw==HIGH) : (raw==LOW); 
}

void oledHeader(const char* title) {
  display.clearDisplay(); display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE); display.setCursor(0,0);
  display.print(title); display.drawLine(0,10,127,10,SSD1306_WHITE);
}

void drawStatusRow(const bool ledOn[4], const String &rightText) {
  display.fillRect(0, 52, 128, 12, SSD1306_BLACK); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  int x = 0, y = 54;
  for (int i=0; i<4; ++i) {
    if (ledOn[i]) { display.fillRect(x, y-1, 14, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK); } 
    else { display.drawRect(x, y-1, 14, 10, SSD1306_WHITE); }
    display.setCursor(x+3, y); display.print(i+1);
    display.setTextColor(SSD1306_WHITE); x += 16;
  }
  int16_t bx,by; uint16_t bw,bh; display.getTextBounds(rightText, 0, y, &bx, &by, &bw, &bh);
  int rx = 128 - (int)bw - 2; if (rx < 68) rx = 68;
  display.setCursor(rx, y); display.print(rightText); display.display();
}

bool listFilesToOLED(const char* folder, const char* extFilter, String &picked) {
  File dir = SD.open(folder);
  if (!dir || !dir.isDirectory()) {
    oledHeader("SD error");
    display.setCursor(0, 20); display.print("Missing folder:");
    display.setCursor(0, 30); display.print(folder);
    display.display(); delay(1500);
    if (dir) dir.close();
    return false;
  }
  char _filename[64];
  const int MAX_FILES = 64;
  String files[MAX_FILES]; int count = 0;

  File f;
  while ( (f = dir.openNextFile()) ) {
    if (!f.isDirectory()) {
      f.getName(_filename, sizeof(_filename));
      String name = String(_filename);
      String low = name; low.toLowerCase();
      String ext = String(extFilter); ext.toLowerCase();
      if (ext.length() == 0 || low.endsWith(ext)) {
        if (count < MAX_FILES) files[count++] = name;
      }
    }
    f.close();
  }
  dir.close();

  if (count == 0) {
    oledHeader("No files");
    display.setCursor(0, 20); display.print("Folder:");
    display.setCursor(0, 30); display.print(folder);
    display.setCursor(0, 45); display.print("No matching files.");
    display.display(); delay(1500);
    return false;
  }

  int idx = 0;
  while (true) {
    oledHeader("Select file");
    display.setCursor(0, 20); display.print(folder);
    display.setCursor(0, 35); display.print(files[idx]);
    display.setCursor(0, 55); display.print("[+] next  [-] prev  [Enter] ok");
    display.display();

    if (readButton(btnPlus))  { idx = (idx + 1) % count; }
    if (readButton(btnMinus)) { idx = (idx - 1 + count) % count; }
    if (readButton(btnEnter)) { picked = String(folder) + "/" + files[idx]; return true; }
    delay(10);
  }
}

/* MotorController_Mega_Menu.ino
 * Menu system and user interaction for the Mega sketch.
 */

void menuXCtrl() {
  const float INC_MM = 5.0f;
  float distanceMM = 5.0f;
  bool exitScreen = false;

  while (!exitScreen) {
    bool redraw = true;
    while (true) {
      if (redraw) {
        oledHeader("X Ctrl");
        display.setCursor(0, 18); display.print("Distance: ");
        display.print(distanceMM, 1); display.print(" mm");
        display.setCursor(0, 34); display.print("[+/-] \xC2\xB1 5mm");
        display.setCursor(0, 46); display.print("[Enter] Direction   Hold [-] back");
        display.display();
        redraw = false;
      }

      if (readButton(btnPlus))  { distanceMM += INC_MM; if (distanceMM > 10000.0f) distanceMM = 10000.0f; redraw = true; }
      if (readButton(btnMinus)) {
        unsigned long t0 = millis();
        while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) { exitScreen = true; break; } }
        if (exitScreen) break;
        distanceMM -= INC_MM; if (distanceMM < 0.0f) distanceMM = 0.0f;
        redraw = true;
      }
      if (readButton(btnEnter)) break;
      delay(10);
    }
    if (exitScreen) break;

    bool forward = true;
    redraw = true;
    while (true) {
      if (redraw) {
        oledHeader("X Direction");
        display.setCursor(0, 18); display.print("Move: ");
        display.print(forward ? "Forward" : "Backward");
        display.setCursor(0, 34); display.print("[+/-] Toggle");
        display.setCursor(0, 46); display.print("[Enter] Go      Hold [-] back");
        display.display();
        redraw = false;
      }

      if (readButton(btnPlus) || readButton(btnMinus)) { forward = !forward; redraw = true; }
      if (readButton(btnEnter)) {
        if (distanceMM > 0) {
          digitalWrite(PIN_EN, LOW);
          bool none[4] = {false,false,false,false};
          String rt = String("Move ") + (forward ? "+" : "-") + String(distanceMM,1) + "mm";
          drawStatusRow(none, rt);
          xMoveMM(distanceMM, forward);
          digitalWrite(PIN_EN, HIGH);
        }
        oledHeader("X Ctrl");
        display.setCursor(0, 30); display.print("Done: ");
        display.print(forward ? "+" : "-");
        display.print(distanceMM,1); display.print(" mm");
        bool none2[4] = {false,false,false,false};
        drawStatusRow(none2, "Ready");
        display.display();
        delay(800);
        break;
      }
      if (buttonHeld(btnMinus)) {
        unsigned long t0 = millis();
        while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) { exitScreen = true; break; } }
        if (exitScreen) break;
      }
      delay(10);
    }
  }
}

void menuZCtrl() {
  const float INC_MM = 0.1f;
  float distanceMM = 0.1f;
  bool exitScreen = false;

  while (!exitScreen) {
    bool redraw = true;
    while (true) {
      if (redraw) {
        oledHeader("Z Ctrl");
        display.setCursor(0, 18); display.print("Distance: ");
        display.print(distanceMM, 1); display.print(" mm");
        display.setCursor(0, 34); display.print("[+/-] \xC2\xB1 0.1mm");
        display.setCursor(0, 46); display.print("[Enter] Direction   Hold [-] back");
        display.display();
        redraw = false;
      }

      if (readButton(btnPlus))  { distanceMM += INC_MM; if (distanceMM > 100.0f) distanceMM = 100.0f; redraw = true; }
      if (readButton(btnMinus)) {
        unsigned long t0 = millis();
        while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) { exitScreen = true; break; } }
        if (exitScreen) break;
        distanceMM -= INC_MM; if (distanceMM < 0.1f) distanceMM = 0.1f;
        redraw = true;
      }
      if (readButton(btnEnter)) break;
      delay(10);
    }
    if (exitScreen) return;

    bool down = true;
    redraw = true;
    while (true) {
      if (redraw) {
        oledHeader("Z Direction");
        display.setCursor(0, 18); display.print("Move: ");
        display.print(down ? "Down" : "Up");
        display.setCursor(0, 34); display.print("Distance: ");
        display.print(distanceMM, 1); display.print(" mm");
        display.setCursor(0, 46); display.print("[+/-] Toggle   [Enter] Go");
        display.display();
        redraw = false;
      }

      if (readButton(btnPlus) || readButton(btnMinus)) { down = !down; redraw = true; }
      if (readButton(btnEnter)) {
        long steps = lround_double(distanceMM * STEPS_PER_MM_Z);
        if (steps > 0) {
          digitalWrite(PIN_EN, LOW);
          bool none[4] = {false,false,false,false};
          String rt = String("Z ") + (down ? "+" : "-") + String(distanceMM,1) + "mm : " + String(steps) + " s";
          drawStatusRow(none, rt);
          moveZStepsRelative(down ? steps : -steps);
          digitalWrite(PIN_EN, HIGH);
        }
        oledHeader("Z Ctrl");
        display.setCursor(0, 30); display.print("Done");
        bool none2[4] = {false,false,false,false};
        drawStatusRow(none2, "Ready");
        display.display();
        delay(800);
        return;
      }
      if (buttonHeld(btnMinus)) {
        unsigned long t0 = millis();
        while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) return; }
      }
      delay(10);
    }
  }
}

void menuZCalib() {
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Z Calibration");
    display.setCursor(0, 18); display.print("> Home Z");
    display.setCursor(0, 46); display.print("Hold [-] to exit");
    display.display();

    if (readButton(btnEnter)) {
      oledHeader("Z Homing");
      display.setCursor(0, 30); display.print("Please wait...");
      display.display();
      homeZ();
      oledHeader("Z Homed");
      display.setCursor(0, 30); display.print("Done");
      display.display();
      delay(800);
      return;
    }
    if (buttonHeld(btnMinus)) return;
    delay(10);
  }
}

void menuMotor() {
  int sel = 0;
  const int ITEMS = 4;

  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Motor Control");
    display.setCursor(0, 16); display.print(sel==0?"> ":"  "); display.print("X Move");
    display.setCursor(0, 26); display.print(sel==1?"> ":"  "); display.print("Y Move");
    display.setCursor(0, 36); display.print(sel==2?"> ":"  "); display.print("Z Move");
    display.setCursor(0, 46); display.print(sel==3?"> ":"  "); display.print("Z Home");
    display.setCursor(0, 56); display.print("[+/-] Select  [Enter] Go");
    display.display();

    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) menuXCtrl();
      else if (sel == 1) menuYCtrl();
      else if (sel == 2) menuZCtrl();
      else if (sel == 3) menuZCalib();
    }
    if (buttonHeld(btnMinus)) return;
    delay(10);
  }
}

void menuYCtrl() {
  float baseMM = 5.0f;
  float trimR = 0.0f;
  const float INC_BASE = 5.0f;
  const float INC_TRIM = 0.5f;
  bool redraw = true;

  while (true) {
    if (redraw) {
      oledHeader("Y Ctrl (dist)");
      display.setCursor(0, 16); display.print("Both: "); display.print(baseMM,1); display.print(" mm");
      display.setCursor(0, 28); display.print("Trim R: "); display.print(trimR,1); display.print(" mm");
      display.setCursor(0, 44); display.print("[+] +5mm / [-] -5mm");
      display.setCursor(0, 56); display.print("[Enter] Dir / Hold [-] back");
      display.display();
      redraw = false;
    }

    if (readButton(btnPlus))  { baseMM += INC_BASE; if (baseMM > 10000.0f) baseMM = 10000.0f; redraw = true; }
    if (readButton(btnMinus)) { unsigned long t0 = millis(); while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) return; } baseMM -= INC_BASE; if (baseMM < 0.0f) baseMM = 0.0f; redraw = true; }
    if (buttonHeld(btnPlus)) { unsigned long t0 = millis(); while (buttonHeld(btnPlus)) { delay(5); if (millis()-t0>600) { trimR += INC_TRIM; redraw = true; break; } } }
    if (readButton(btnEnter)) break;
    delay(10);
  }

  bool forward = true;
  redraw = true;
  while (true) {
    if (redraw) {
      oledHeader("Y Direction");
      display.setCursor(0, 16); display.print("Move: "); display.print(forward ? "Forward" : "Backward");
      display.setCursor(0, 28); display.print("Both "); display.print(baseMM,1); display.print("mm");
      display.setCursor(0, 38); display.print("TrimR "); display.print(trimR,1); display.print("mm");
      display.setCursor(0, 56); display.print("[+/-] Toggle  [Enter] Go  (-hold)Back");
      display.display();
      redraw = false;
    }

    if (readButton(btnPlus) || readButton(btnMinus)) { forward = !forward; redraw = true; }
    if (readButton(btnEnter)) {
      bool none[4] = {false,false,false,false};
      String rt = String("Y ") + (forward?"+":"-") + String(baseMM,1) + " / R+" + String(trimR,1);
      drawStatusRow(none, rt);
      yMoveSkewMM(baseMM, trimR, forward);
      oledHeader("Y Ctrl");
      display.setCursor(0, 30); display.print("Done");
      drawStatusRow(none, "Ready");
      display.display();
      delay(900);
      return;
    }
    if (buttonHeld(btnMinus)) { unsigned long t0 = millis(); while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0>600) return; } }
    delay(10);
  }
}
int readDirEntries(const String &path, Entry out[], int maxOut) {
  int n=0; File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }
  File f;
  char _filename[64];
  while ((f = dir.openNextFile())) {
    if (n >= maxOut) { f.close(); break; }
    Entry e;
    f.getName(_filename, sizeof(_filename));
    e.name = String(_filename);
    e.isDir = f.isDirectory();
    e.size = e.isDir ? 0 : (uint32_t)f.size();
    out[n++] = e;
    f.close();
  }
  dir.close();
  return n;
}



void menuFileBrowser() {
  String cwd = "/";
  const int MAX_ENTRIES = 64;
  int sel = 0;
  char _filename[64];
  while (true) {
    Entry ents[MAX_ENTRIES];
    int n = readDirEntries(cwd, ents, MAX_ENTRIES);

    oledHeader("Browser");
    display.setCursor(0, 12); display.print(cwd);
    int start = sel - (sel % 4);
    for (int i=0; i<4 && (start+i)<n; ++i) {
      int idx = start + i;
      display.setCursor(0, 22 + i*10);
      display.print(idx == sel ? ">" : " ");
      display.print(ents[idx].isDir ? "[D] " : "    ");
      
      display.print(ents[idx].name);
    }
    display.setCursor(0, 56);
    display.print("[+/-] Move  [Enter] Open  (-hold)Up  (+hold)Exit");
    display.display();

    if (readButton(btnPlus))  { sel = (sel + 1); if (sel >= n) sel = max(0, n-1); }
    if (readButton(btnMinus)) { sel = (sel - 1); if (sel < 0) sel = 0; }
    if (readButton(btnEnter) && n>0) {
      if (ents[sel].isDir) {
        if (cwd == "/") cwd = "/" + ents[sel].name;
        else            cwd = cwd + "/" + ents[sel].name;
        sel = 0;
      } else {
        oledHeader("File");
        display.setCursor(0, 20); display.print(ents[sel].name);
        display.setCursor(0, 32); display.print("Size: ");
        display.print((unsigned long)ents[sel].size); display.print(" B");
        display.setCursor(0, 46); display.print("Press Enter...");
        display.display();
        while (!readButton(btnEnter)) delay(10);
      }
    }
    if (buttonHeld(btnMinus)) {
      unsigned long t0 = millis();
      while (buttonHeld(btnMinus)) {
        delay(5);
        if (millis()-t0>600) {
          if (cwd != "/") {
            int last = cwd.lastIndexOf('/');
            cwd = (last <= 0) ? "/" : cwd.substring(0, last);
            sel = 0;
          }
          break;
        }
      }
    }
    if (buttonHeld(btnPlus)) {
      unsigned long t1 = millis();
      while (buttonHeld(btnPlus)) {
        delay(5);
        if (millis()-t1>600) return;
      }
    }
    delay(10);
  }
}


void menuCalib() {
  int sub = 0;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Calibration");
    display.setCursor(0, 20); display.print(sub == 0 ? "> " : "  "); display.print("Fire LED");
    display.setCursor(0, 30); display.print(sub == 1 ? "> " : "  "); display.print("Load LUT");
    display.setCursor(0, 56); display.print("[+/-] Move  [Ent] Select");
    display.display();

    if (readButton(btnPlus))  sub = (sub + 1) % 2;
    if (readButton(btnMinus)) sub = (sub - 1 + 2) % 2;
    if (readButton(btnEnter)) {
      if (sub == 0) {
        int step = 0;
        while (true) {
          if (emergencyStopTriggered) handleEmergencyStop();
          oledHeader("Calib Fire");
          display.setCursor(0, 20); displayPrintf(display, "Fire LED %d", step + 1);
          display.setCursor(0, 50); display.print("[+/-] Sel  [Ent] Fire");
          display.display();
          if (readButton(btnPlus)) { step++; if (step > 3) step = 0; }
          if (readButton(btnMinus)) { step--; if (step < 0) step = 3; }
          if (readButton(btnEnter)) { uint32_t dur[4] = {0,0,0,0}; dur[step] = 1000000; sendBatchToPrinthead(dur); waitForPrinthead(); }
          if (buttonHeld(btnMinus)) { unsigned long t0 = millis(); while (buttonHeld(btnMinus)) { delay(5); if (millis()-t0 > 600) break; } return; }
          delay(10);
        }
      } else {
        String picked;
        if (listFilesToOLED("/calibrations", ".txt", picked)) {
          bool ok = loadCalibrationFile(picked);
          oledHeader("Load LUT");
          display.setCursor(0, 20); display.print(ok ? "Loaded:" : "Failed:");
          display.setCursor(0, 30); display.print(picked);
          display.display();
          delay(900);
        }
      }
    }
    delay(10);
  }
}

// --- Sub-menu 2: Serial Passthrough ---
void testSerialRelay() {
  oledHeader("Serial Relay");
  display.setCursor(0, 20); display.print("Passthrough Active");
  display.setCursor(0, 35); display.print("PC <---> Nano");
  display.setCursor(0, 54); display.print("Hold [-] to exit");
  display.display();

  // Flush any stale data sitting in the buffers
  while(Serial.available()) Serial.read();
  while(PRINTHEAD_SERIAL.available()) PRINTHEAD_SERIAL.read();

  // Tight relay loop
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();

    // PC -> Mega -> Nano
    if (Serial.available()) {
      PRINTHEAD_SERIAL.write(Serial.read());
    }
    // Nano -> Mega -> PC
    if (PRINTHEAD_SERIAL.available()) {
      Serial.write(PRINTHEAD_SERIAL.read());
    }

    // Hold Minus to Exit
    if (buttonHeld(btnMinus)) {
      unsigned long t0 = millis();
      while (buttonHeld(btnMinus)) {
        delay(5);
        if (millis() - t0 > 600) return;
      }
    }
  }
}
void testFireLEDs() { 
  int ms = 500;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Test LEDs");
    display.setCursor(0, 20); displayPrintf(display, "Dur: %d ms", ms);
    display.setCursor(0, 50); display.print("[+/-] Time [Ent] Fire"); display.display();
    
    if (readButton(btnPlus)) ms += 100;
    if (readButton(btnMinus)) { ms -= 100; if (ms < 100) ms = 100; }
    if (readButton(btnEnter)) {
      uint32_t dur[4] = {(uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000};
      sendBatchToPrinthead(dur); waitForPrinthead();
    }
    
    // Hold Minus to Exit
    if (buttonHeld(btnMinus)) { 
      unsigned long t0=millis(); 
      while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } 
    }
    delay(10);
  }
}

void menuTest() {
  int sel = 0;
  const int ITEMS = 2;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Test Menu");
    display.setCursor(0, 20); display.print(sel==0?"> ":"  "); display.print("Fire LEDs");
    display.setCursor(0, 32); display.print(sel==1?"> ":"  "); display.print("Serial Relay");
    display.setCursor(0, 54); display.print("Hold [-] to exit");
    display.display();

    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) testFireLEDs();
      if (sel == 1) testSerialRelay();
    }
    if (buttonHeld(btnMinus)) { unsigned long t0=millis(); while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } }
    delay(10);
  }
}

void menuBrowser() { menuFileBrowser(); }

void menuPrint() { String picked; if (!listFilesToOLED("/prints", ".bmp", picked)) return; printEntireBMP_4by(picked); }

void zeroXHere() {
  currentSteps = 0;
  Serial.println(F("[UTIL] X zero set here (currentSteps=0)"));
  oledHeader("Utilities");
  display.setCursor(0, 24); display.print("Set X=0 at");
  display.setCursor(0, 36); display.print("current position");
  display.display();
  delay(900);
}

void measureMatrix() {
  oledHeader("Probing Mesh..."); display.display();
  if (!SD.exists("/meshes")) { SD.mkdir("/meshes"); }
  float spacing_x = MESH_MAX_X / (MESH_SIZE - 1); float spacing_y = MESH_MAX_Y / (MESH_SIZE - 1);

  for (int i = 0; i < MESH_SIZE; i++) {
    for (int j = 0; j < MESH_SIZE; j++) {
      if (emergencyStopTriggered) handleEmergencyStop();
      int y_idx = (i % 2 == 0) ? j : (MESH_SIZE - 1 - j);
      moveToPixel((i * spacing_x) / MM_PER_PIXEL);
      yMoveMM(abs((y_idx * spacing_y) - (currentStepsY/STEPS_PER_MM_Y)), (y_idx * spacing_y) > (currentStepsY/STEPS_PER_MM_Y));
      z_mesh[i][y_idx] = probeZ();
      display.fillRect(0, 20, 128, 44, SSD1306_BLACK);
      display.setCursor(0, 30); displayPrintf(display, "Pt %d,%d Z:%.2f", i, y_idx, z_mesh[i][y_idx]); display.display();
    }
  }

  int fileIndex = 0; String filename;
  do { filename = "/meshes/m" + String(fileIndex) + ".txt"; fileIndex++; } while (SD.exists(filename));
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    for (int i = 0; i < MESH_SIZE; i++) {
      for (int j = 0; j < MESH_SIZE; j++) { f.print(z_mesh[i][j], 3); if (j < MESH_SIZE - 1) f.print(","); }
      f.println();
    }
    f.close(); mesh_active = true;
    oledHeader("Mesh Saved"); display.setCursor(0, 20); display.print(filename); display.display(); delay(2000);
  }
}

void loadMatrixFile() { 
  String picked; if (!listFilesToOLED("/meshes", ".txt", picked)) return;
  File f = SD.open(picked); if (!f) return;
  oledHeader("Loading Mesh"); display.display();
  for (int i = 0; i < MESH_SIZE; i++) {
    String line = f.readStringUntil('\n'); int start = 0;
    for (int j = 0; j < MESH_SIZE; j++) {
      int comma = line.indexOf(',', start);
      String val = (comma == -1) ? line.substring(start) : line.substring(start, comma);
      z_mesh[i][j] = val.toFloat(); start = comma + 1;
    }
  }
  f.close(); mesh_active = true;
  display.setCursor(0, 30); display.print("Mesh Active!"); display.display(); delay(1500);
}

void menuUtilities() {
  int sel = 0; const int ITEMS = 4;
  while (true) {
    if (emergencyStopTriggered) handleEmergencyStop();
    oledHeader("Utilities");
    display.setCursor(0, 16); display.print(sel==0?"> ":"  "); display.print("Zero Pos Here");
    display.setCursor(0, 26); display.print(sel==1?"> ":"  "); display.print("Reload Config");
    display.setCursor(0, 36); display.print(sel==2?"> ":"  "); display.print("Measure Bed Mesh");
    display.setCursor(0, 46); display.print(sel==3?"> ":"  "); display.print("Load Bed Mesh");
    display.display();
    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) { zeroXHere(); return; }
      if (sel == 1) { bool ok = loadConfigFromSD(CONFIG_FILE_PATH); if (ok) applyConfig(); oledHeader("Reload Config"); display.setCursor(0, 28); display.print(ok ? "Reloaded" : "Failed"); display.display(); delay(900); return; }
      if (sel == 2) { measureMatrix(); return; }
      if (sel == 3) { loadMatrixFile(); return; }
    }
    if (buttonHeld(btnMinus)) { unsigned long t0=millis(); while(buttonHeld(btnMinus)){ delay(5); if(millis()-t0>600) return; } }
    delay(10);
  }
}

void menuESP32Control() {
  oledHeader("ESP32 Mode"); display.setCursor(0, 16); display.print("Waiting for ESP32...");
  display.setCursor(0, 56); display.print("Hold [-] to exit"); display.display();

  ESP_SERIAL.println("READY"); bool exitScreen = false;
  while (!exitScreen) {
    if (emergencyStopTriggered) handleEmergencyStop();
    if (ESP_SERIAL.available()) {
      String cmd = ESP_SERIAL.readStringUntil('\n'); cmd.trim();
      if (cmd.startsWith("P:")) {
        int comma = cmd.indexOf(','); int colon = cmd.indexOf(':', comma);
        if (comma > 0 && colon > 0) {
          int w = cmd.substring(2, comma).toInt(); int h = cmd.substring(comma + 1, colon).toInt();
          String hexData = cmd.substring(colon + 1);
          oledHeader("ESP32 Print"); display.setCursor(0, 20); displayPrintf(display, "W:%d H:%d", w, h); display.display();
          const int totalSlices = (h + 3) / 4;
          for (int slice = 0; slice < totalSlices; ++slice) {
            Image4x img; img.width = min(MAX_IMG_WIDTH, w);
            for (int r = 0; r < 4; r++) for (int c = 0; c < MAX_IMG_WIDTH; c++) img.row[r][c] = 0;
            int visualStart = slice * 4;
            for (int vr = 0; vr < 4; ++vr) {
              int rowIdx = visualStart + vr; if (rowIdx >= h) break;
              for (int col = 0; col < w; ++col) {
                int strIdx = (rowIdx * w) + col;
                if (strIdx < (int)hexData.length()) img.row[vr][col] = hexCharToVal(hexData[strIdx]) * 17;
              }
            }
            doPrintBMPSliceSerpentine(img, (slice % 2 == 0), false);
            if (slice < totalSlices - 1) yMoveMM(CFG.yBandAdvanceMM, true);
          }
          ESP_SERIAL.println("finish");
          oledHeader("ESP32 Mode"); display.setCursor(0, 16); display.print("Waiting..."); display.display();
        } else {
          ESP_SERIAL.println("error");
        }
      }
    }
    if (buttonHeld(btnMinus)) {
      unsigned long t0 = millis();
      while (buttonHeld(btnMinus)) { delay(5); if (millis() - t0 > 600) { ESP_SERIAL.println("EXIT"); exitScreen = true; break; } }
    }
    delay(5);
  }
}

enum MainMenu { MM_CALIB = 0, MM_PRINT = 1, MM_TEST = 2, MM_MOTOR = 3, MM_BROWSER = 4, MM_UTILS = 5, MM_ESP32 = 6 };

void menuMain() {
  static int mainSel = 0;
  static unsigned long lastPing = 0;
  static unsigned long lastResp = millis();
  static String nanoStatus = "Wait";

  if (millis() - lastPing > 500) {
    PRINTHEAD_SERIAL.print("S\n");
    lastPing = millis();
  }

  while (PRINTHEAD_SERIAL.available()) {
    static String rBuf = "";
    char c = PRINTHEAD_SERIAL.read();
    if (c == '\n') {
      rBuf.trim();
      if (rBuf.startsWith("S:")) {
        nanoStatus = (rBuf == "S:0") ? "Ready" : "Busy";
        lastResp = millis();
      }
      rBuf = "";
    } else {
      rBuf += c;
    }
  }

  if (millis() - lastResp > 2000) {
    nanoStatus = "Offline";
  }

  oledHeader("Main Menu");
  display.setCursor(70, 0); display.print(nanoStatus);
  display.setCursor(0, 14); display.print(mainSel==0?"> ":"  "); display.print("Calib");
  display.setCursor(0, 26); display.print(mainSel==1?"> ":"  "); display.print("Print");
  display.setCursor(0, 38); display.print(mainSel==2?"> ":"  "); display.print("Test");
  display.setCursor(0, 50); display.print(mainSel==3?"> ":"  "); display.print("Motor");
  display.setCursor(58, 14); display.print(mainSel==4?"> ":"  "); display.print("Browser");
  display.setCursor(58, 26); display.print(mainSel==5?"> ":"  "); display.print("Utils");
  display.setCursor(58, 38); display.print(mainSel==6?"> ":"  "); display.print("ESP32");
  display.display();

  if (readButton(btnPlus))  mainSel = (mainSel + 1) % 7;
  if (readButton(btnMinus)) mainSel = (mainSel - 1 + 7) % 7;
  if (readButton(btnEnter)) {
    if      (mainSel == MM_CALIB)   menuCalib();
    else if (mainSel == MM_PRINT)   menuPrint();
    else if (mainSel == MM_TEST)    menuTest();
    else if (mainSel == MM_MOTOR)   menuMotor();
    else if (mainSel == MM_BROWSER) menuBrowser();
    else if (mainSel == MM_UTILS)   menuUtilities();
    else if (mainSel == MM_ESP32)   menuESP32Control();
  }
}
