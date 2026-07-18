#include "uv_menu.h"
#include "uv_print.h" // Since menus call motion/print functions
#include "uv_sd.h"    // Since menus call SD loaders

void displayPrintf(OledDisplay &disp, const char *fmt, ...) {
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
  display.clearBuffer(); display.setFont(u8g2_font_6x10_tf);
  display.setDrawColor(1); display.setCursor(0,0);
  display.print(title); display.drawLine(0,10,127,10);
}

void drawStatusRow(const bool ledOn[4], const String &rightText) {
  display.setDrawColor(0); display.drawBox(0, 52, 128, 12); display.setDrawColor(1);
  int x = 0, y = 54;
  for (int i=0; i<4; ++i) {
    if (ledOn[i]) { display.drawBox(x, y-1, 14, 10);
    display.setDrawColor(0); }
    else { display.drawFrame(x, y-1, 14, 10); }
    display.setCursor(x+3, y); display.print(i+1);
    display.setDrawColor(1); x += 16;
  }
  uint16_t bw = display.getStrWidth(rightText.c_str());
  int rx = 128 - (int)bw - 2; if (rx < 68) rx = 68;
  display.setCursor(rx, y); display.print(rightText); display.sendBuffer();
}

// Human-readable software-tracked carriage position, e.g. "X:12.3 Y:45.6". Deliberately
// built with String(float,1) rather than snprintf/displayPrintf's %f: this build isn't
// linked with -lprintf_flt (no such flag in platformio.ini), so avr-libc's vsnprintf
// silently renders %f as a literal '?' instead of a number rather than failing loudly.
// Arduino's String class formats floats itself via dtostrf, independent of libc printf,
// so it's the safe way to get a float onto the OLED here.
String xyPositionString() {
  return "X:" + String(currentSteps / STEPS_PER_MM_X, 1) + " Y:" + String(currentStepsY / STEPS_PER_MM_Y, 1);
}

// Walks folder once, invoking visit(name, matchIndex) for each entry matching extLower.
// Kept to a single small stack frame (one File, one name buffer) rather than caching
// every matching name in a stack-resident array -- with only ~1.2KB free for stack+heap
// combined on the Mega, a 64-entry String[] here was enough to collide with the heap and
// corrupt SD state by the time the operator navigated to this screen (boot-time listing
// worked fine since it ran before anything else had fragmented that headroom).
template <typename Visitor>
static void forEachMatchingFile(const char* folder, const String &extLower, Visitor visit) {
  File dir = SD.open(folder);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  char nameBuf[64];
  int matchIndex = 0;
  File f;
  while ( (f = dir.openNextFile()) ) {
    if (!f.isDirectory()) {
      f.getName(nameBuf, sizeof(nameBuf));
      String low(nameBuf); low.toLowerCase();
      if (extLower.length() == 0 || low.endsWith(extLower)) {
        if (!visit(nameBuf, matchIndex)) { f.close(); break; }
        matchIndex++;
      }
    }
    f.close();
  }
  dir.close();
}

bool listFilesToOLED(const char* folder, const char* extFilter, String &picked) {
  String extLower(extFilter); extLower.toLowerCase();

  {
    File dir = SD.open(folder);
    bool isDir = dir && dir.isDirectory();
    if (dir) dir.close();
    if (!isDir) {
      oledHeader("SD error");
      display.setCursor(0, 20); display.print("Missing folder:");
      display.setCursor(0, 30); display.print(folder);
      display.sendBuffer(); delay(1500);
      return false;
    }
  }

  int count = 0;
  forEachMatchingFile(folder, extLower, [&](const char*, int) { count++; return true; });

  if (count == 0) {
    oledHeader("No files");
    display.setCursor(0, 20); display.print("Folder:");
    display.setCursor(0, 30); display.print(folder);
    display.setCursor(0, 45); display.print("No matching files.");
    display.sendBuffer(); delay(1500);
    return false;
  }

  int idx = 0;
  char selected[64];
  while (true) {
    forEachMatchingFile(folder, extLower, [&](const char* name, int i) {
      if (i == idx) { strncpy(selected, name, sizeof(selected)); selected[sizeof(selected)-1] = 0; return false; }
      return true;
    });

    oledHeader("Select file");
    display.setCursor(0, 20); display.print(folder);
    display.setCursor(0, 35); display.print(selected);
    display.setCursor(0, 55); display.print("[+/-] move [Ent] ok [Back] x");
    display.sendBuffer();

    if (readButton(btnPlus))  { idx = (idx + 1) % count; }
    if (readButton(btnMinus)) { idx = (idx - 1 + count) % count; }
    if (readButton(btnEnter)) { picked = String(folder) + "/" + selected; return true; }
    if (readButton(btnBack))  { return false; }
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
        display.setCursor(0, 46); display.print("[Enter] Direction   [Back] exit");
        display.sendBuffer();
        redraw = false;
      }

      if (readButton(btnPlus))  { distanceMM += INC_MM; if (distanceMM > 10000.0f) distanceMM = 10000.0f; redraw = true; }
      if (readButton(btnMinus)) { distanceMM -= INC_MM; if (distanceMM < 0.0f) distanceMM = 0.0f; redraw = true; }
      if (readButton(btnBack))  { exitScreen = true; break; }
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
        display.setCursor(0, 46); display.print("[Enter] Go      [Back] exit");
        display.sendBuffer();
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
          cancelRequested = false;
        }
        oledHeader("X Ctrl");
        display.setCursor(0, 30); display.print("Done: ");
        display.print(forward ? "+" : "-");
        display.print(distanceMM,1); display.print(" mm");
        bool none2[4] = {false,false,false,false};
        drawStatusRow(none2, "Ready");
        display.sendBuffer();
        delay(800);
        break;
      }
      if (readButton(btnBack)) { exitScreen = true; break; }
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
        display.setCursor(0, 46); display.print("[Enter] Direction   [Back] exit");
        display.sendBuffer();
        redraw = false;
      }

      if (readButton(btnPlus))  { distanceMM += INC_MM; if (distanceMM > 100.0f) distanceMM = 100.0f; redraw = true; }
      if (readButton(btnMinus)) { distanceMM -= INC_MM; if (distanceMM < 0.1f) distanceMM = 0.1f; redraw = true; }
      if (readButton(btnBack))  { exitScreen = true; break; }
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
        display.setCursor(0, 46); display.print("[+/-] Toggle [Ent] Go [Back] x");
        display.sendBuffer();
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
          cancelRequested = false;
        }
        oledHeader("Z Ctrl");
        display.setCursor(0, 30); display.print("Done");
        bool none2[4] = {false,false,false,false};
        drawStatusRow(none2, "Ready");
        display.sendBuffer();
        delay(800);
        return;
      }
      if (readButton(btnBack)) return;
      delay(10);
    }
  }
}

void menuZCalib() {
  while (true) {
    if (checkEStop()) return;
    oledHeader("Z Calibration");
    display.setCursor(0, 18); display.print("> Home Z");
    display.setCursor(0, 46); display.print("[Back] to exit");
    display.sendBuffer();

    if (readButton(btnEnter)) {
      oledHeader("Z Homing");
      display.setCursor(0, 30); display.print("Please wait...");
      display.sendBuffer();
      homeZ();
      cancelRequested = false;
      oledHeader("Z Homed");
      display.setCursor(0, 30); display.print("Done");
      display.sendBuffer();
      delay(800);
      return;
    }
    if (readButton(btnBack)) return;
    delay(10);
  }
}

// Runs the same homeXY() used automatically after a print, but from the Motor Control
// menu on demand -- lets the operator diagnose homing behavior (does it reach 0,0? does
// it stop short on one axis?) without having to run a whole print first.
void menuHomeXY() {
  homeXY();
  if (eStopLatched) return; // loop() will show the e-stop screen instead of "Done".
  cancelRequested = false;
  oledHeader("Home X/Y");
  display.setCursor(0, 20); display.print("Done");
  display.setCursor(0, 32); display.print(xyPositionString() + " mm");
  display.sendBuffer();
  delay(1000);
}

void menuMotor() {
  int sel = 0;
  const int ITEMS = 5;

  while (true) {
    if (checkEStop()) return;
    oledHeader("Motor Control");
    display.setCursor(0, 14); display.print(sel==0?"> ":"  "); display.print("X Move");
    display.setCursor(0, 24); display.print(sel==1?"> ":"  "); display.print("Y Move");
    display.setCursor(0, 34); display.print(sel==2?"> ":"  "); display.print("Z Move");
    display.setCursor(0, 44); display.print(sel==3?"> ":"  "); display.print("Z Home");
    display.setCursor(0, 54); display.print(sel==4?"> ":"  "); display.print("Home X/Y");
    display.sendBuffer();

    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) menuXCtrl();
      else if (sel == 1) menuYCtrl();
      else if (sel == 2) menuZCtrl();
      else if (sel == 3) menuZCalib();
      else if (sel == 4) menuHomeXY();
    }
    if (readButton(btnBack)) return;
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
      display.setCursor(0, 56); display.print("[Enter] Dir / [Back] exit");
      display.sendBuffer();
      redraw = false;
    }

    if (readButton(btnPlus))  { baseMM += INC_BASE; if (baseMM > 10000.0f) baseMM = 10000.0f; redraw = true; }
    if (readButton(btnMinus)) { baseMM -= INC_BASE; if (baseMM < 0.0f) baseMM = 0.0f; redraw = true; }
    if (buttonHeld(btnPlus)) { unsigned long t0 = millis(); while (buttonHeld(btnPlus)) { delay(5); if (millis()-t0>600) { trimR += INC_TRIM; redraw = true; break; } } }
    if (readButton(btnBack))  return;
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
      display.setCursor(0, 56); display.print("[+/-] Toggle [Ent] Go [Back] x");
      display.sendBuffer();
      redraw = false;
    }

    if (readButton(btnPlus) || readButton(btnMinus)) { forward = !forward; redraw = true; }
    if (readButton(btnEnter)) {
      bool none[4] = {false,false,false,false};
      String rt = String("Y ") + (forward?"+":"-") + String(baseMM,1) + " / R+" + String(trimR,1);
      drawStatusRow(none, rt);
      yMoveSkewMM(baseMM, trimR, forward);
      cancelRequested = false;
      oledHeader("Y Ctrl");
      display.setCursor(0, 30); display.print("Done");
      drawStatusRow(none, "Ready");
      display.sendBuffer();
      delay(900);
      return;
    }
    if (readButton(btnBack)) return;
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
    display.print("[+/-] Move  [Enter] Open  [Back] Up/Exit");
    display.sendBuffer();

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
        display.sendBuffer();
        while (!readButton(btnEnter)) delay(10);
      }
    }
    if (readButton(btnBack)) {
      if (cwd != "/") {
        int last = cwd.lastIndexOf('/');
        cwd = (last <= 0) ? "/" : cwd.substring(0, last);
        sel = 0;
      } else {
        return;
      }
    }
    delay(10);
  }
}


void menuCalibPattern() {
  float maxTimeSec = 2.0f;
  const float INC_SEC = 0.25f;
  bool redraw = true;

  while (true) {
    if (redraw) {
      oledHeader("Calib Pattern");
      display.setCursor(0, 18); display.print("Max time: ");
      display.print(maxTimeSec, 2); display.print(" s");
      display.setCursor(0, 34); display.print("[+/-] \xC2\xB1 0.25s");
      display.setCursor(0, 46); display.print("[Enter] Run   [Back] exit");
      display.sendBuffer();
      redraw = false;
    }

    if (readButton(btnPlus))  { maxTimeSec += INC_SEC; if (maxTimeSec > 5.0f) maxTimeSec = 5.0f; redraw = true; }
    if (readButton(btnMinus)) { maxTimeSec -= INC_SEC; if (maxTimeSec < 0.1f) maxTimeSec = 0.1f; redraw = true; }
    if (readButton(btnBack))  return;
    if (readButton(btnEnter)) {
      oledHeader("Calib Pattern");
      display.setCursor(0, 20); display.print("Running...");
      display.sendBuffer();
      printCalibrationPattern(maxTimeSec);
      cancelRequested = false;
      oledHeader("Calib Pattern");
      display.setCursor(0, 20); display.print("Done");
      display.sendBuffer();
      delay(900);
      return;
    }
    delay(10);
  }
}

// Lets the operator capture the carriage's *current* position (jogged there beforehand
// via Motor Control > X/Y Move) as the print-start offset, so a print can be centered on
// paper that isn't aligned to the machine's absolute origin. Persisted immediately so it
// survives a power cycle -- see moveToOffset() for where it's used.
void menuOffset() {
  int sub = 0;
  const int SUB_ITEMS = 2;
  while (true) {
    if (checkEStop()) return;
    oledHeader("Offset");
    display.setCursor(0, 12); display.print("Cur: " + xyPositionString());
    display.setCursor(0, 22);
    display.print("Set: X:" + String(CFG.offsetXmm, 1) + " Y:" + String(CFG.offsetYmm, 1));
    display.setCursor(0, 38); display.print(sub == 0 ? "> " : "  "); display.print("Set X here");
    display.setCursor(0, 48); display.print(sub == 1 ? "> " : "  "); display.print("Set Y here");
    display.sendBuffer();

    if (readButton(btnPlus))  sub = (sub + 1) % SUB_ITEMS;
    if (readButton(btnMinus)) sub = (sub - 1 + SUB_ITEMS) % SUB_ITEMS;
    if (readButton(btnBack))  return;
    if (readButton(btnEnter)) {
      if (sub == 0) CFG.offsetXmm = currentSteps  / STEPS_PER_MM_X;
      else          CFG.offsetYmm = currentStepsY / STEPS_PER_MM_Y;
      bool ok = saveConfigToSD(CONFIG_FILE_PATH);
      oledHeader("Offset");
      display.setCursor(0, 20);
      display.print(String(sub == 0 ? "X" : "Y") + " set: " + String(sub == 0 ? CFG.offsetXmm : CFG.offsetYmm, 1) + "mm");
      display.setCursor(0, 32); display.print(ok ? "Saved to SD" : "SD save FAILED");
      display.sendBuffer();
      delay(1000);
    }
    delay(10);
  }
}

// Shows the active exposure LUT -- which file it came from and all 16 values -- so
// "is the right LUT actually loaded?" is a glance instead of a guess. loadCalibrationFile
// records the source filename (loadedLUTName) whenever a /calibrations file loads.
void menuViewLUT() {
  oledHeader("Active LUT");
  display.setCursor(0, 12); display.print(loadedLUTName);
  // 16 values in a 4x4 grid, 2 decimals -- String(float,N) not %f (no printf_flt).
  for (int i = 0; i < 16; i++) {
    display.setCursor((i % 4) * 32, 22 + (i / 4) * 10);
    display.print(String(LUT16[i], 2));
  }
  display.sendBuffer();
  while (true) {
    if (checkEStop()) return;
    if (readButton(btnBack) || readButton(btnEnter)) return;
    delay(10);
  }
}

void menuCalib() {
  int sub = 0;
  const int SUB_ITEMS = 5;
  while (true) {
    if (checkEStop()) return;
    oledHeader("Calibration");
    display.setCursor(0, 14); display.print(sub == 0 ? "> " : "  "); display.print("Fire LED");
    display.setCursor(0, 24); display.print(sub == 1 ? "> " : "  "); display.print("Load LUT");
    display.setCursor(0, 34); display.print(sub == 2 ? "> " : "  "); display.print("Print Pattern");
    display.setCursor(0, 44); display.print(sub == 3 ? "> " : "  "); display.print("Offset");
    display.setCursor(0, 54); display.print(sub == 4 ? "> " : "  "); display.print("View LUT");
    display.sendBuffer();

    if (readButton(btnPlus))  sub = (sub + 1) % SUB_ITEMS;
    if (readButton(btnMinus)) sub = (sub - 1 + SUB_ITEMS) % SUB_ITEMS;
    if (readButton(btnBack))  return;
    if (readButton(btnEnter)) {
      if (sub == 2) { menuCalibPattern(); continue; }
      if (sub == 3) { menuOffset(); continue; }
      if (sub == 4) { menuViewLUT(); continue; }
      if (sub == 0) {
        int step = 0;
        while (true) {
          if (checkEStop()) return;
          oledHeader("Calib Fire");
          display.setCursor(0, 20); displayPrintf(display, "Fire LED %d", step + 1);
          display.setCursor(0, 50); display.print("[+/-]Sel [Ent]Fire [Back]x");
          display.sendBuffer();
          if (readButton(btnPlus)) { step++; if (step > 3) step = 0; }
          if (readButton(btnMinus)) { step--; if (step < 0) step = 3; }
          if (readButton(btnEnter)) { uint32_t dur[4] = {0,0,0,0}; dur[step] = 1000000; sendBatchToPrinthead(dur); waitForPrinthead(); cancelRequested = false; }
          if (readButton(btnBack)) return;
          delay(10);
        }
      } else {
        String picked;
        if (listFilesToOLED("/calibrations", ".txt", picked)) {
          bool ok = loadCalibrationFile(picked);
          oledHeader("Load LUT");
          display.setCursor(0, 20); display.print(ok ? "Loaded:" : "Failed:");
          display.setCursor(0, 30); display.print(picked);
          if (ok) {
            // The endpoints answer the two booth questions at a glance: does level 0
            // really mean "no exposure" (min should be 0.00), and how hot is full black?
            display.setCursor(0, 42);
            display.print("min " + String(LUT16[0], 2) + "s  max " + String(LUT16[15], 2) + "s");
            display.setCursor(0, 54); display.print("Calib > View LUT for all");
          }
          display.sendBuffer();
          delay(ok ? 1800 : 900);
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
  display.setCursor(0, 54); display.print("[Back] to exit");
  display.sendBuffer();

  // Flush any stale data sitting in the buffers
  while(Serial.available()) Serial.read();
  while(PRINTHEAD_SERIAL.available()) PRINTHEAD_SERIAL.read();

  // Tight relay loop
  while (true) {
    if (checkEStop()) return;

    // PC -> Mega -> Nano
    if (Serial.available()) {
      PRINTHEAD_SERIAL.write(Serial.read());
    }
    // Nano -> Mega -> PC
    if (PRINTHEAD_SERIAL.available()) {
      Serial.write(PRINTHEAD_SERIAL.read());
    }

    if (readButton(btnBack)) return;
  }
}
void testFireLEDs() { 
  int ms = 500;
  while (true) {
    if (checkEStop()) return;
    oledHeader("Test LEDs");
    display.setCursor(0, 20); displayPrintf(display, "Dur: %d ms", ms);
    display.setCursor(0, 50); display.print("[+/-]Time [Ent]Fire [Back]x"); display.sendBuffer();

    if (readButton(btnPlus)) ms += 100;
    if (readButton(btnMinus)) { ms -= 100; if (ms < 100) ms = 100; }
    if (readButton(btnEnter)) {
      uint32_t dur[4] = {(uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000, (uint32_t)ms*1000};
      sendBatchToPrinthead(dur); waitForPrinthead();
      cancelRequested = false;
    }
    if (readButton(btnBack)) return;
    delay(10);
  }
}

// Jogs to the configured print-start offset, fires all 4 LEDs for 1s (so the operator can
// see on paper exactly where a print will start/center), then returns to wherever the
// carriage was beforehand rather than leaving it stranded at the offset.
void testOffset() {
  long savedX = currentSteps, savedY = currentStepsY;

  moveToOffset();
  if (eStopLatched) return;

  if (!cancelRequested) {
    oledHeader("Test Offset");
    display.setCursor(0, 20); display.print("Firing 1s..."); display.sendBuffer();
    uint32_t dur[4] = {1000000, 1000000, 1000000, 1000000};
    sendBatchToPrinthead(dur);
    waitForPrinthead();
  }
  cancelRequested = false;

  moveToXYSteps(savedX, savedY, "Test Offset", "Returning...");
  if (eStopLatched) return;
  oledHeader("Test Offset");
  display.setCursor(0, 20); display.print("Done");
  display.sendBuffer();
  delay(800);
}

void menuTest() {
  int sel = 0;
  const int ITEMS = 3;
  while (true) {
    if (checkEStop()) return;
    oledHeader("Test Menu");
    display.setCursor(0, 16); display.print(sel==0?"> ":"  "); display.print("Fire LEDs");
    display.setCursor(0, 26); display.print(sel==1?"> ":"  "); display.print("Serial Relay");
    display.setCursor(0, 36); display.print(sel==2?"> ":"  "); display.print("Test Offset");
    display.sendBuffer();

    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) testFireLEDs();
      if (sel == 1) testSerialRelay();
      if (sel == 2) testOffset();
    }
    if (readButton(btnBack)) return;
    delay(10);
  }
}

void menuBrowser() { menuFileBrowser(); }

void menuPrint() { String picked; if (!listFilesToOLED("/prints", ".bmp", picked)) return; printEntireBMP_4by(picked); cancelRequested = false; }

void zeroHere() {
  currentSteps = 0;
  currentStepsY = 0;
  Serial.println(F("[UTIL] X and Y zeros set here (currentSteps=0)"));
  oledHeader("Utilities");
  display.setCursor(0, 24); display.print("Set X=0 at");
  display.setCursor(0, 36); display.print("current position");
  display.sendBuffer();
  delay(900);
}

void measureMatrix() {
  oledHeader("Probing Mesh..."); display.sendBuffer();
  if (!SD.exists("/meshes")) { SD.mkdir("/meshes"); }
  float spacing_x = MESH_MAX_X / (MESH_SIZE - 1); float spacing_y = MESH_MAX_Y / (MESH_SIZE - 1);

  for (int i = 0; i < MESH_SIZE; i++) {
    for (int j = 0; j < MESH_SIZE; j++) {
      if (checkEStop() || checkCancel()) { cancelRequested = false; return; }
      int y_idx = (i % 2 == 0) ? j : (MESH_SIZE - 1 - j);
      moveToPixel((i * spacing_x) / MM_PER_PIXEL);
      yMoveMM(abs((y_idx * spacing_y) - (currentStepsY/STEPS_PER_MM_Y)), (y_idx * spacing_y) > (currentStepsY/STEPS_PER_MM_Y));
      z_mesh[i][y_idx] = probeZ();
      display.setDrawColor(0); display.drawBox(0, 20, 128, 44); display.setDrawColor(1);
      display.setCursor(0, 30); displayPrintf(display, "Pt %d,%d Z:%.2f", i, y_idx, z_mesh[i][y_idx]); display.sendBuffer();
    }
  }
  cancelRequested = false;

  int fileIndex = 0; String filename;
  do { filename = "/meshes/m" + String(fileIndex) + ".txt"; fileIndex++; } while (SD.exists(filename));
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    for (int i = 0; i < MESH_SIZE; i++) {
      for (int j = 0; j < MESH_SIZE; j++) { f.print(z_mesh[i][j], 3); if (j < MESH_SIZE - 1) f.print(","); }
      f.println();
    }
    f.close(); mesh_active = true;
    oledHeader("Mesh Saved"); display.setCursor(0, 20); display.print(filename); display.sendBuffer(); delay(2000);
  }
}

void loadMatrixFile() { 
  String picked; if (!listFilesToOLED("/meshes", ".txt", picked)) return;
  File f = SD.open(picked); if (!f) return;
  oledHeader("Loading Mesh"); display.sendBuffer();
  for (int i = 0; i < MESH_SIZE; i++) {
    String line = f.readStringUntil('\n'); int start = 0;
    for (int j = 0; j < MESH_SIZE; j++) {
      int comma = line.indexOf(',', start);
      String val = (comma == -1) ? line.substring(start) : line.substring(start, comma);
      z_mesh[i][j] = val.toFloat(); start = comma + 1;
    }
  }
  f.close(); mesh_active = true;
  display.setCursor(0, 30); display.print("Mesh Active!"); display.sendBuffer(); delay(1500);
}

void menuUtilities() {
  int sel = 0; const int ITEMS = 4;
  while (true) {
    if (checkEStop()) return;
    oledHeader("Utilities");
    display.setCursor(0, 16); display.print(sel==0?"> ":"  "); display.print("Zero Pos Here");
    display.setCursor(0, 26); display.print(sel==1?"> ":"  "); display.print("Reload Config");
    display.setCursor(0, 36); display.print(sel==2?"> ":"  "); display.print("Measure Bed Mesh");
    display.setCursor(0, 46); display.print(sel==3?"> ":"  "); display.print("Load Bed Mesh");
    display.sendBuffer();
    if (readButton(btnPlus))  sel = (sel + 1) % ITEMS;
    if (readButton(btnMinus)) sel = (sel - 1 + ITEMS) % ITEMS;
    if (readButton(btnEnter)) {
      if (sel == 0) { zeroHere(); return; }
      if (sel == 1) { bool ok = loadConfigFromSD(CONFIG_FILE_PATH); if (ok) applyConfig(); oledHeader("Reload Config"); display.setCursor(0, 28); display.print(ok ? "Reloaded" : "Failed"); display.sendBuffer(); delay(900); return; }
      if (sel == 2) { measureMatrix(); return; }
      if (sel == 3) { loadMatrixFile(); return; }
    }
    if (readButton(btnBack)) return;
    delay(10);
  }
}

// 4x4 ordered (Bayer) dither matrix, values 0-15 -- lets a single-bit OLED approximate
// the incoming print's 16 grayscale levels by pixel density instead of a hard threshold,
// so a genuinely mid-gray source pixel doesn't look identical to solid black or white.
static const uint8_t BAYER4[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5},
};

// Renders a quick dithered thumbnail of exactly the hex payload as received -- no invert
// applied, since this is meant to show the raw data that arrived (for diagnosing a bad
// transfer/encoding), not the post-processed exposure. Scales to fit within the given
// box and centers within it; degrades gracefully (coarser dither) for larger images.
static void drawImagePreview(int w, int h, const String &hexData, int boxX, int boxY, int boxW, int boxH) {
  int blockSize = min(boxW / max(1, w), boxH / max(1, h));
  if (blockSize < 1) blockSize = 1;
  if (blockSize > 10) blockSize = 10;
  int x0 = boxX + (boxW - blockSize * w) / 2;
  int y0 = boxY + (boxH - blockSize * h) / 2;

  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      int idx = r * w + c;
      uint8_t v = (idx < (int)hexData.length()) ? (uint8_t)(hexCharToVal(hexData[idx]) * 17) : 0;
      uint8_t level = v >> 4; // 0-15
      for (int by = 0; by < blockSize; by++) {
        for (int bx = 0; bx < blockSize; bx++) {
          if (BAYER4[by % 4][bx % 4] < level) {
            display.drawPixel(x0 + c * blockSize + bx, y0 + r * blockSize + by);
          }
        }
      }
    }
  }
}

// Runs once a validated "P:w,h:hexdata" arrives while online: alerts the attendant,
// clears the gantry so paper can go on, seats it with vacuum once confirmed, prints,
// and returns the carriage home in X and Y (Z is never touched automatically -- only
// by manual operator adjustment). Returns to the idle online screen on cancel/e-stop too.
static void runIncomingPrint(int w, int h, const String &hexData) {
  buzzPrintIncoming();
  oledHeader("Print Incoming");
  // W/H/Len as actually parsed off the wire -- the fastest way to tell a bad transfer
  // (wrong dimensions, truncated/oversized hex string) apart from a bad source image.
  display.setCursor(0, 12); displayPrintf(display, "W=%d H=%d Len=%d", w, h, (int)hexData.length());
  drawImagePreview(w, h, hexData, 0, 22, 128, 30);
  display.setCursor(0, 54); display.print("[Ent]Print [Back]x");
  display.sendBuffer();
  while (true) {
    if (checkEStop()) return;
    if (readButton(btnEnter)) break;
    if (readButton(btnBack)) return;
    delay(10);
  }

  // Clear the bed so the attendant can place paper -- vacuum stays off here since
  // suction fights positioning the sheet; +/- lets them pre-activate a fan if it helps
  // hold an edge while placing it.
  oledHeader("Load Paper"); display.setCursor(0, 20); display.print("Moving clear..."); display.sendBuffer();
  yMoveMM(PAPER_LOAD_Y_MM, true);

  bool fan1On = false, fan2On = false;
  bool redraw = true;
  while (true) {
    if (checkEStop()) { setVacRelay(PIN_VAC_RELAY_1, false); setVacRelay(PIN_VAC_RELAY_2, false); return; }
    if (redraw) {
      oledHeader("Place Paper");
      display.setCursor(0, 14); display.print("Fan 1: "); display.print(fan1On ? "ON" : "off");
      display.setCursor(0, 24); display.print("Fan 2: "); display.print(fan2On ? "ON" : "off");
      display.setCursor(0, 40); display.print("[+/-] Fans  [Ent] Placed");
      display.setCursor(0, 50); display.print("[Back] Cancel print");
      display.sendBuffer();
      redraw = false;
    }
    if (readButton(btnMinus)) {
      if (fan2On)      { fan2On = false; setVacRelay(PIN_VAC_RELAY_2, false); redraw = true; }
      else if (fan1On) { fan1On = false; setVacRelay(PIN_VAC_RELAY_1, false); redraw = true; }
    }
    if (readButton(btnPlus)) {
      if (!fan1On)      { fan1On = true; setVacRelay(PIN_VAC_RELAY_1, true); redraw = true; }
      else if (!fan2On) { fan2On = true; setVacRelay(PIN_VAC_RELAY_2, true); redraw = true; }
    }
    if (readButton(btnEnter)) break;
    if (readButton(btnBack)) {
      setVacRelay(PIN_VAC_RELAY_1, false); setVacRelay(PIN_VAC_RELAY_2, false);
      moveYToZero();
      return;
    }
    delay(10);
  }

  // Paper's placed and confirmed: bring the vacuum up fully (the print itself needs full
  // suction regardless of whatever was toggled while positioning the sheet) and move to
  // the configured print-start offset (Calib > Offset) instead of true home, so the print
  // lands centered on paper that isn't aligned to the machine's absolute origin.
  setVacRelay(PIN_VAC_RELAY_1, true);
  setVacRelay(PIN_VAC_RELAY_2, true);
  moveToOffset();
  printOriginStepsX = lround_double(CFG.offsetXmm * STEPS_PER_MM_X);

  // The attendant already confirmed above, so skip the print engine's own start prompt.
  printHexData_4by(w, h, hexData, true);
  printOriginStepsX = 0;

  setVacRelay(PIN_VAC_RELAY_1, false);
  setVacRelay(PIN_VAC_RELAY_2, false);
  if (eStopLatched) return; // Let loop() show the e-stop screen instead of homing.

  bool completedOk = !cancelRequested;
  cancelRequested = false;
  if (completedOk) buzzPrintSuccess();

  // X and Y always return home after a print; Z is never moved automatically.
  homeXY();
}

void menuESP32Control() {
  String ip = "waiting...";
  espOnline = false;
  // Fixed buffer, not a String -- appending one byte at a time to a String on the Mega's
  // small, fragmentation-prone heap can silently stall (Arduino String just gives up on a
  // failed allocation) partway through a line, corrupting it. Each protocol line here is
  // short (a chunk header + <=32 hex chars), so this only needs to be modest.
  static char inBuf[80];
  int inLen = 0;
  bool redraw = true;

  // Chunked stop-and-wait transfer state (PSTART/D/PEND handling below). xferBuf
  // accumulates the hex payload across chunks; sized for images well beyond the current
  // 16x16/8x32 web UI presets.
  bool xferActive = false;
  int xferW = 0, xferH = 0, xferTotalLen = 0, xferReceived = 0;
  static char xferBuf[1200];

  ESP_SERIAL.println("STATE:OFFLINE"); // In case a previous session left the ESP32 thinking it's online.

  while (true) {
    if (checkEStop()) { if (espOnline) ESP_SERIAL.println("STATE:OFFLINE"); return; }

    // Drain lines from the ESP32: periodic "IP:..." broadcasts, and (while online) a
    // stop-and-wait chunked transfer -- PSTART:w,h,len announces one, then <=32 hex
    // chars at a time as "D:<chunk>", each acked with "OK\n"/"ERR\n" before the sender's
    // next chunk, so no single burst can ever exceed the hardware RX buffer regardless
    // of how long the Mega is blocked elsewhere when a chunk lands.
    while (ESP_SERIAL.available()) {
      char c = ESP_SERIAL.read();
      if (c == '\n') {
        inBuf[inLen] = '\0';
        String line = String(inBuf); line.trim();
        inLen = 0;
        if (line.startsWith("IP:")) {
          ip = line.substring(3);
          redraw = true;
        } else if (espOnline && line.startsWith("PSTART:")) {
          int c1 = line.indexOf(','); int c2 = line.indexOf(',', c1 + 1);
          int pw = (c1 > 0 && c2 > 0) ? line.substring(7, c1).toInt() : 0;
          int ph = (c1 > 0 && c2 > 0) ? line.substring(c1 + 1, c2).toInt() : 0;
          int plen = (c1 > 0 && c2 > 0) ? line.substring(c2 + 1).toInt() : 0;
          if (c1 > 0 && c2 > 0 && pw > 0 && ph > 0 && plen > 0 && plen < (int)sizeof(xferBuf) - 1) {
            xferW = pw; xferH = ph; xferTotalLen = plen; xferReceived = 0;
            xferActive = true;
            ESP_SERIAL.println("READY");
          } else {
            xferActive = false;
            ESP_SERIAL.println("ERR");
          }
        } else if (xferActive && line.startsWith("D:")) {
          String chunk = line.substring(2);
          int n = chunk.length();
          if (xferReceived + n <= xferTotalLen && xferReceived + n < (int)sizeof(xferBuf)) {
            for (int i = 0; i < n; i++) xferBuf[xferReceived + i] = chunk[i];
            xferReceived += n;
            ESP_SERIAL.println("OK");
          } else {
            xferActive = false; // Sender/receiver disagree on length -- abort, don't guess.
            ESP_SERIAL.println("ERR");
          }
        } else if (xferActive && line == "PEND") {
          xferActive = false;
          if (xferReceived == xferTotalLen) {
            xferBuf[xferReceived] = '\0';
            String hexData = String(xferBuf);
            runIncomingPrint(xferW, xferH, hexData);
            // Always report the definitive current state once runIncomingPrint returns --
            // it may have declined at the alert/vacuum screen (never reaching PRINTING at
            // all) or fully printed, and either way the ESP32 needs telling so a web UI
            // that optimistically marked itself "printing" on dispatch doesn't get stuck.
            reportEspIdleState();
            if (eStopLatched) return; // Let loop() show the e-stop screen instead of "Waiting...".
          } else {
            ESP_SERIAL.println("ERR");
          }
          redraw = true;
        }
      } else if (inLen < (int)sizeof(inBuf) - 1) {
        inBuf[inLen++] = c;
      }
      // else: line longer than the buffer -- drop the overflow byte rather than corrupt
      // memory. Protocol lines here are always short; this would only trip on noise.
    }

    if (redraw) {
      oledHeader(espOnline ? "ESP32: ONLINE" : "ESP32: OFFLINE");
      display.setCursor(0, 12); display.print("IP: "); display.print(ip);
      // Software-tracked position -- see the Main Menu's copy of this readout for why it
      // can drift from the true physical position; shown here too since this is the
      // screen the attendant watches for long stretches while "Waiting for print...".
      display.setCursor(0, 24); display.print(xyPositionString());
      display.setCursor(0, 40); display.print(espOnline ? "Waiting for print..." : "[Enter] Go Online");
      display.setCursor(0, 52); display.print(espOnline ? "[Back] Go Offline" : "[Back] Exit");
      display.sendBuffer();
      redraw = false;
    }

    if (readButton(btnEnter) && !espOnline) {
      espOnline = true;
      ESP_SERIAL.println("STATE:ONLINE");
      redraw = true;
    }
    if (readButton(btnBack)) {
      if (espOnline) { espOnline = false; ESP_SERIAL.println("STATE:OFFLINE"); redraw = true; }
      else return;
    }

    delay(10);
  }
}

enum MainMenu { MM_CALIB = 0, MM_PRINT = 1, MM_TEST = 2, MM_MOTOR = 3, MM_BROWSER = 4, MM_UTILS = 5, MM_ESP32 = 6 };

// Shown by loop() instead of the normal menu whenever eStopLatched is true. Requires a
// deliberate hold (not just a tap) to clear, and re-checks the Nano before actually
// clearing so a still-engaged switch or still-hot printhead can't be waved away.
void renderEmergencyStopScreen() {
  static unsigned long holdStart = 0;

  oledHeader("EMERGENCY STOP");
  display.setCursor(0, 16); display.print("Cause: "); display.print(eStopCause);
  display.setCursor(0, 32); display.print("Inspect machine, then");
  display.setCursor(0, 42); display.print("hold [Enter] to clear");

  if (buttonHeld(btnEnter)) {
    if (holdStart == 0) holdStart = millis();
    if (millis() - holdStart > 2000) {
      if (queryNanoLimitMask() == 0) {
        clearEmergencyStop();
      } else {
        display.setCursor(0, 54); display.print("Still triggered!");
      }
      holdStart = 0;
    }
  } else {
    holdStart = 0;
  }
  display.sendBuffer();
}

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
  display.setCursor(0, 13); display.print(mainSel==0?"> ":"  "); display.print("Calib");
  display.setCursor(0, 24); display.print(mainSel==1?"> ":"  "); display.print("Print");
  display.setCursor(0, 35); display.print(mainSel==2?"> ":"  "); display.print("Test");
  display.setCursor(0, 46); display.print(mainSel==3?"> ":"  "); display.print("Motor");
  display.setCursor(58, 13); display.print(mainSel==4?"> ":"  "); display.print("Browser");
  display.setCursor(58, 24); display.print(mainSel==5?"> ":"  "); display.print("Utils");
  display.setCursor(58, 35); display.print(mainSel==6?"> ":"  "); display.print("ESP32");
  // Software-tracked carriage position -- there's no hardware home switch on X/Y (only
  // Z has one), so this is only as accurate as the last completed home/move; it's here
  // specifically to help spot when it's drifted from where the carriage physically is.
  display.setCursor(0, 54); display.print(xyPositionString());
  display.sendBuffer();

  if (readButton(btnPlus))  mainSel = (mainSel + 1) % 7;
  if (readButton(btnMinus)) mainSel = (mainSel - 1 + 7) % 7;
  if (readButton(btnEnter)) {
    cancelRequested = false; // Fresh start for whichever submenu we're entering.
    if      (mainSel == MM_CALIB)   menuCalib();
    else if (mainSel == MM_PRINT)   menuPrint();
    else if (mainSel == MM_TEST)    menuTest();
    else if (mainSel == MM_MOTOR)   menuMotor();
    else if (mainSel == MM_BROWSER) menuBrowser();
    else if (mainSel == MM_UTILS)   menuUtilities();
    else if (mainSel == MM_ESP32)   menuESP32Control();
  }
}
