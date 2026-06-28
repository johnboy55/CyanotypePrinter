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
        long steps = lround_double(distanceMM * STEPS_PER_MM_X);
        if (steps > 0) {
          digitalWrite(PIN_EN, LOW);
          bool none[4] = {false,false,false,false};
          String rt = String("Move ") + (forward ? "+" : "-") + String(distanceMM,1) + "mm";
          drawStatusRow(none, rt);
          stepN(steps, forward);
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
          String rt = String("Z ") + (down ? "+" : "-") + String(distanceMM,1) + "mm";
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

void menuPrint() { String picked; if (!listFilesToOLED("/", ".bmp", picked)) return; printEntireBMP_4by(picked); }

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
