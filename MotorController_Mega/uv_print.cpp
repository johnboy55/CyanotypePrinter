#include "uv_print.h"
#include "uv_menu.h" // For UI hooks (oledHeader, drawStatusRow, etc.) during printing
#include "uv_sd.h"

// --- Internal BMP Structures ---
#pragma pack(push,1)
struct BMPFILEHEADER {
  uint16_t bfType;
  uint32_t bfSize;
  uint16_t bfReserved1;
  uint16_t bfReserved2;
  uint32_t bfOffBits;
};

struct BMPINFOHEADER {
  uint32_t biSize;
  int32_t  biWidth;
  int32_t  biHeight;
  uint16_t biPlanes;
  uint16_t biBitCount;
  uint32_t biCompression;
  uint32_t biSizeImage;
  int32_t  biXPelsPerMeter;
  int32_t  biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;
};
#pragma pack(pop)

struct BMPMeta {
  BMPFILEHEADER fh;
  BMPINFOHEADER ih;
  bool bottomUp;
  int H;
  int W;
  int rowStride;
};

const int MAX_ROWBUF = 256;

// ---------- UART API (Printhead) ----------
void sendBatchToPrinthead(uint32_t durations[4]) {
  uint8_t mask = 0;
  for (int i = 0; i < 4; i++) { if (durations[i] > 0) bitSet(mask, i); }
  if (mask == 0) return;
  String cmd = "F:" + String(mask) + "," + String(durations[0]) + "," + 
               String(durations[1]) + "," + String(durations[2]) + "," + String(durations[3]) + "\n";
  PRINTHEAD_SERIAL.print(cmd);
}

void waitForPrinthead() {
  bool isBusy = true;
  while (isBusy) {
    if (emergencyStopTriggered) handleEmergencyStop();
    PRINTHEAD_SERIAL.print("S\n");
    unsigned long timeout = millis();
    while (!PRINTHEAD_SERIAL.available() && millis() - timeout < 10) {}
    if (PRINTHEAD_SERIAL.available()) {
      String response = PRINTHEAD_SERIAL.readStringUntil('\n');
      response.trim();
      if (response == "S:0") isBusy = false;
    }
  }
}

bool isProbeTriggered() {
  PRINTHEAD_SERIAL.print("P\n");
  unsigned long timeout = millis();
  while (!PRINTHEAD_SERIAL.available() && millis() - timeout < 100) {
    if (emergencyStopTriggered && !zHomingActive) handleEmergencyStop();
  }
  if (PRINTHEAD_SERIAL.available()) {
    String response = PRINTHEAD_SERIAL.readStringUntil('\n'); response.trim();
    if (response == "P:1") return true;
  }
  return false;
}

bool isZMaxSwitchPressed() {
  return digitalRead(PIN_Z_CONTACT) == LOW;
}

void moveZStepsRelative(long deltaSteps) {
  if (deltaSteps == 0) return;
  bool dir = deltaSteps > 0;
  long steps = abs(deltaSteps);
  for (long i = 0; i < steps; ++i) {
    if (emergencyStopTriggered && !zHomingActive) handleEmergencyStop();
    zPulse(dir);
    delayMicroseconds(1000);
  }
}

void deployProbe() {
  PRINTHEAD_SERIAL.print("D\n");
  delay(500);
}

void stowProbe() {
  PRINTHEAD_SERIAL.print("U\n");
  delay(500);
}

void moveZSteps(long targetStepsZ) {
  long delta = targetStepsZ - currentStepsZ;
  if (delta == 0) return;
  bool dir = delta > 0;
  long steps = abs(delta);
  for (long i = 0; i < steps; ++i) {
    zPulse(dir);
    delayMicroseconds(1000);
  }
}

void homeZ() {
  zHomingActive = true;

  while (!isZMaxSwitchPressed()) {
    if (emergencyStopTriggered) {
      zHomingActive = false;
      handleEmergencyStop();
    }
    zPulse(false);
    delayMicroseconds(1000);
  }

  currentStepsZ = 0;

  long approachSteps = lround_double(Z_MAX_TO_CR_TOUCH_APPROACH_MM * STEPS_PER_MM_Z);
  moveZStepsRelative(approachSteps);

  deployProbe();
  while (!isProbeTriggered()) {
    if (emergencyStopTriggered) {
      zHomingActive = false;
      handleEmergencyStop();
    }
    zPulse(true);
    delayMicroseconds(1000);
  }

  currentStepsZ = 0;
  stowProbe();
  zHomingActive = false;
}

bool setZDistanceFromPaper(float paperGapMM) {
  if (paperGapMM < 0.0f) return false;
  long targetSteps = lround_double((Z_HOME_SWITCH_TO_PAPER_MM - paperGapMM) * STEPS_PER_MM_Z);
  if (targetSteps < 0) return false;
  moveZSteps(targetSteps);
  return true;
}

float getInterpolatedZ(float x_mm, float y_mm) {
  if (!mesh_active) return 0.0f;
  x_mm = constrain(x_mm, 0.0f, MESH_MAX_X); y_mm = constrain(y_mm, 0.0f, MESH_MAX_Y);
  float spacing_x = MESH_MAX_X / (MESH_SIZE - 1);
  float spacing_y = MESH_MAX_Y / (MESH_SIZE - 1);
  int i = min((int)(x_mm / spacing_x), MESH_SIZE - 2);
  int j = min((int)(y_mm / spacing_y), MESH_SIZE - 2);
  float tx = (x_mm - (i * spacing_x)) / spacing_x;
  float ty = (y_mm - (j * spacing_y)) / spacing_y;
  float z00 = z_mesh[i][j], z10 = z_mesh[i+1][j], z01 = z_mesh[i][j+1], z11 = z_mesh[i+1][j+1];
  return (1 - tx)*(1 - ty)*z00 + tx*(1 - ty)*z10 + (1 - tx)*ty*z01 + tx*ty*z11;
}

void zPulse(bool dir) {
  digitalWrite(PIN_Z_DIR, dir ? HIGH : LOW); digitalWrite(PIN_Z_STEP, HIGH);
  delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_Z_STEP, LOW);
  currentStepsZ += dir ? 1 : -1;
}

float probeZ() {
  deployProbe();
  while (!isProbeTriggered()) {
    zPulse(true);
    delayMicroseconds(2000);
  }
  float trigger_height = currentStepsZ / STEPS_PER_MM_Z;
  stowProbe();
  for (int i = 0; i < (2.0f * STEPS_PER_MM_Z); i++) {
    zPulse(false);
    delayMicroseconds(1000);
  }
  return trigger_height;
}

void stepPulseOnceX() { digitalWrite(PIN_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_STEP, LOW); }
inline void yPulseLeft()  { digitalWrite(PIN_YL_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_YL_STEP, LOW); }
inline void yPulseRight() { digitalWrite(PIN_YR_STEP, HIGH); delayMicroseconds(STEP_PULSE_US); digitalWrite(PIN_YR_STEP, LOW); }

void stepN(long n, bool dir) {
  digitalWrite(PIN_DIR, dir ? HIGH : LOW);
  float currentY_mm = currentStepsY / STEPS_PER_MM_Y;
  float targetX_mm = (currentSteps + (dir ? n : -n)) / STEPS_PER_MM_X;
  long startZ = currentStepsZ;
  long targetZ = lround_double(getInterpolatedZ(targetX_mm, currentY_mm) * STEPS_PER_MM_Z);
  long dz = targetZ - startZ; bool zDir = (dz > 0);
  dz = abs(dz);
  long err = n / 2;

  for (long i = 0; i < n; ++i) {
    if (emergencyStopTriggered) handleEmergencyStop();
    stepPulseOnceX(); currentSteps += dir ? 1 : -1;
    if (mesh_active && dz > 0) { err -= dz;
      if (err < 0) { zPulse(zDir); err += n; } }
    delayMicroseconds(max(0, STEP_PERIOD_US - STEP_PULSE_US));
  }
}

void yStepPair(long stepsL, long stepsR, bool dir) {
  if (stepsL <= 0 && stepsR <= 0) return;
  digitalWrite(PIN_Y_DIR, dir ? HIGH : LOW); digitalWrite(PIN_YR_DIR, dir ? HIGH : LOW); digitalWrite(PIN_EN, LOW);
  long N = max(stepsL, stepsR);
  float currentX_mm = currentSteps / STEPS_PER_MM_X;
  float targetY_mm = (currentStepsY + (dir ? N : -N)) / STEPS_PER_MM_Y;
  long startZ = currentStepsZ;
  long targetZ = lround_double(getInterpolatedZ(currentX_mm, targetY_mm) * STEPS_PER_MM_Z);
  long dz = targetZ - startZ;
  bool zDir = (dz > 0); dz = abs(dz);
  long err = N / 2;
  long accL = 0, accR = 0;

  for (long i = 0; i < N; ++i) {
    if (emergencyStopTriggered) handleEmergencyStop();
    accL += stepsL; accR += stepsR;
    if (accL >= N) { yPulseLeft(); accL -= N; }
    if (accR >= N) { yPulseRight(); accR -= N; }
    currentStepsY += dir ? 1 : -1;
    if (mesh_active && dz > 0) { err -= dz; if (err < 0) { zPulse(zDir); err += N; } }
    delayMicroseconds(max(0, STEP_PERIOD_US - STEP_PULSE_US));
  }
}


float lutExposureFromByte(uint8_t v) {
  if (v <= 0)   return LUT16[0];
  if (v >= 255) return LUT16[15];
  int idx = v / 17; if (idx >= 15) return LUT16[15];
  int base = idx * 17;
  float t = float(v - base) / 17.0f;
  return LUT16[idx] + t*(LUT16[idx+1] - LUT16[idx]);
}

void moveToSteps(long target) { long d = target - currentSteps; if (d) stepN(labs(d), d > 0); }
void moveToPixel(long p) { moveToSteps(p * (long)STEPS_PER_PIXEL); }
void yMoveMM(float mm, bool forward) { long s = lround_double(mm * STEPS_PER_MM_Y); yStepPair(s, s, forward); }

void yMoveSkewMM(float baseMM, float deltaRightMM, bool forward) {
  long sL = lround_double(baseMM * STEPS_PER_MM_Y);
  long sR = lround_double((baseMM + deltaRightMM) * STEPS_PER_MM_Y);
  if (sR < 0) sR = 0;
  yStepPair(sL, sR, forward);
}

void xMoveMM(float mm, bool right) { long s = lround_double(mm * STEPS_PER_MM_X); stepN(s, right); }
void zMoveMM(float mm, bool down) { long s = lround_double(mm * STEPS_PER_MM_Z); if (s == 0) return; moveZStepsRelative(down ? s : -s); }

// ---------- BMP Parsing Logic ----------
bool readFirst4RowsBMP(const String &path, Image4x &img) {
  img.width = 0;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < MAX_IMG_WIDTH; c++)
      img.row[r][c] = 0;
  File f = SD.open(path);
  if (!f) return false;

  BMPFILEHEADER fh;
  if (f.read((uint8_t*)&fh, sizeof(fh)) != sizeof(fh)) { f.close(); return false; }
  if (fh.bfType != 0x4D42) { f.close(); return false; }

  BMPINFOHEADER ih;
  if (f.read((uint8_t*)&ih, sizeof(ih)) != sizeof(ih)) { f.close(); return false; }
  if (ih.biCompression != 0 || (ih.biBitCount != 24 && ih.biBitCount != 8)) {
    f.close(); return false;
  }

  const bool bottomUp = (ih.biHeight > 0);
  const int H = abs(ih.biHeight);
  const int W = ih.biWidth;

  const int rowBytesRaw = (ih.biBitCount * W + 7) / 8;
  const int rowStride   = (rowBytesRaw + 3) & ~3;

  const int cols = min(MAX_IMG_WIDTH, W);
  img.width = cols;
  f.seek(fh.bfOffBits);

  uint8_t rowbuf[MAX_ROWBUF];
  if (rowStride > (int)sizeof(rowbuf)) { f.close(); return false; }

  for (int visual = 0; visual < 4; ++visual) {
    const int srcRow = bottomUp ? (H - 1 - visual) : visual;
    if (srcRow < 0 || srcRow >= H) break;
    const uint32_t rowOffset = fh.bfOffBits + (uint32_t)srcRow * rowStride;
    f.seek(rowOffset);
    if (f.read(rowbuf, rowStride) != (int)rowStride) { f.close(); return false; }

    if (ih.biBitCount == 24) {
      for (int c = 0; c < cols; c++) {
        const int idx = c * 3;
        const uint8_t B = rowbuf[idx + 0];
        const uint8_t G = rowbuf[idx + 1];
        const uint8_t R = rowbuf[idx + 2];
        img.row[visual][c] = (uint8_t)(0.299f*R + 0.587f*G + 0.114f*B);
      }
    } else {
      for (int c = 0; c < cols; c++) img.row[visual][c] = rowbuf[c];
    }
  }

  f.close();
  return true;
}

bool loadBMPMeta(const String &path, BMPMeta &m) {
  File f = SD.open(path);
  if (!f) return false;

  if (f.read((uint8_t*)&m.fh, sizeof(m.fh)) != sizeof(m.fh)) { f.close(); return false; }
  if (m.fh.bfType != 0x4D42) { f.close(); return false; }
  if (f.read((uint8_t*)&m.ih, sizeof(m.ih)) != sizeof(m.ih)) { f.close(); return false; }
  if (m.ih.biCompression != 0 || (m.ih.biBitCount != 24 && m.ih.biBitCount != 8)) {
    f.close(); return false;
  }

  m.bottomUp  = (m.ih.biHeight > 0);
  m.H         = abs(m.ih.biHeight);
  m.W         = m.ih.biWidth;
  const int rowBytesRaw = (m.ih.biBitCount * m.W + 7) / 8;
  m.rowStride = (rowBytesRaw + 3) & ~3;

  f.close();
  return true;
}

bool readFourRowsAt(const String &path, const BMPMeta &m, int visualStart, Image4x &img) {
  img.width = min(MAX_IMG_WIDTH, m.W);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < MAX_IMG_WIDTH; c++)
      img.row[r][c] = 0;
  File f = SD.open(path);
  if (!f) return false;

  uint8_t rowbuf[MAX_ROWBUF];
  if (m.rowStride > (int)sizeof(rowbuf)) { f.close(); return false; }

  for (int vr = 0; vr < 4; ++vr) {
    const int visualRow = visualStart + vr;
    if (visualRow >= m.H) break;

    const int srcRow = m.bottomUp ? (m.H - 1 - visualRow) : visualRow;
    const uint32_t rowOffset = m.fh.bfOffBits + (uint32_t)srcRow * m.rowStride;

    f.seek(rowOffset);
    if (f.read(rowbuf, m.rowStride) != (int)m.rowStride) { f.close(); return false; }

    if (m.ih.biBitCount == 24) {
      for (int c = 0; c < img.width; ++c) {
        const int idx = c * 3;
        const uint8_t B = rowbuf[idx + 0], G = rowbuf[idx + 1], R = rowbuf[idx + 2];
        img.row[vr][c] = (uint8_t)(0.299f*R + 0.587f*G + 0.114f*B);
      }
    } else {
      for (int c = 0; c < img.width; ++c) img.row[vr][c] = rowbuf[c];
    }
  }

  f.close();
  return true;
}

bool readFourRowsBy(const String &path, const BMPMeta &m, int visualStart, Image4x &img) {
  img.width = min(MAX_IMG_WIDTH, m.W);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < MAX_IMG_WIDTH; c++)
      img.row[r][c] = 0;
  File f = SD.open(path);
  if (!f) return false;

  uint8_t rowbuf[MAX_ROWBUF];
  if (m.rowStride > (int)sizeof(rowbuf)) { f.close(); return false; }

  for (int vr = 0; vr < 4; ++vr) {
    const int visualRow = visualStart - (3 - vr);
    if (visualRow < 0) break;

    const int srcRow = m.bottomUp ? (m.H - 1 - visualRow) : visualRow;
    const uint32_t rowOffset = m.fh.bfOffBits + (uint32_t)srcRow * m.rowStride;

    f.seek(rowOffset);
    if (f.read(rowbuf, m.rowStride) != (int)m.rowStride) { f.close(); return false; }

    if (m.ih.biBitCount == 24) {
      for (int c = 0; c < img.width; ++c) {
        const int idx = c * 3;
        const uint8_t B = rowbuf[idx + 0], G = rowbuf[idx + 1], R = rowbuf[idx + 2];
        img.row[vr][c] = (uint8_t)(0.299f*R + 0.587f*G + 0.114f*B);
      }
    } else {
      for (int c = 0; c < img.width; ++c) img.row[vr][c] = rowbuf[c];
    }
  }

  f.close();
  return true;
}

void countdown_1s_tenths(const char* label) {
  oledHeader(label);
  display.setCursor(0, 20); display.print("Starting in:");
  display.display();
  for (int i = 10; i >= 0; --i) {
    display.fillRect(0, 35, 128, 20, SSD1306_BLACK);
    display.setCursor(20, 35);
    display.setTextSize(2);
    displayPrintf(display, "%d.%d s", i/10, i%10);
    display.setTextSize(1);
    display.display();
    delay(100);
  }
}

// ---------- Print Execution ----------
void printEntireBMP(const String &path) {
  BMPMeta meta;
  if (!loadBMPMeta(path, meta)) {
    oledHeader("BMP"); display.setCursor(0,20); display.print("Header error");
    display.setCursor(0,32); display.print(path); display.display(); delay(1200);
    return;
  }

  const int totalSlices = (meta.H + 3) / 4;

  oledHeader("Print (Serpentine)");
  display.setCursor(0, 16); display.print(path);
  display.setCursor(0, 28);
  displayPrintf(display,"W=%d  H=%d", meta.W, meta.H);
  display.setCursor(0, 40); displayPrintf(display,"%d slices (4 rows ea.)", totalSlices);
  display.setCursor(0, 54); display.print("[Enter] Start  [-] Back");
  display.display();

  while (true) {
    if (readButton(btnEnter)) break;
    if (readButton(btnMinus)) return;
    delay(10);
  }

  countdown_1s_tenths("Starting Print");
  for (int slice = 0; slice < totalSlices; ++slice) {
    const int visualStart = slice * 4;
    Image4x img;
    if (!readFourRowsAt(path, meta, visualStart, img)) {
      oledHeader("BMP");
      display.setCursor(0, 20);
      display.print("Read error at slice ");
      display.print(slice+1); display.display(); delay(1200);
      return;
    }

    oledHeader("Printing...");
    display.setCursor(0, 18); display.print(path);
    display.setCursor(0, 30);
    displayPrintf(display,"Slice %d/%d", slice+1, totalSlices);
    display.setCursor(0, 42); displayPrintf(display,"Rows %d..%d", visualStart, min(meta.H-1, visualStart+3));
    display.display();
    const bool forward = (slice % 2 == 0);
    doPrintBMPSliceSerpentine(img, forward, false);
    if (slice < totalSlices - 1) {
      bool none[4] = {false,false,false,false};
      char buf[24];
      snprintf(buf, sizeof(buf), "Y +%.2fmm", CFG.yBandAdvanceMM);
      drawStatusRow(none, String(buf));
      yMoveMM(CFG.yBandAdvanceMM, true);
    }
  }

  oledHeader("Print");
  display.setCursor(0, 22); display.print("Entire image done.");
  display.display();
  delay(1200);
}
void printEntireBMP_4by(const String &path) {
  BMPMeta meta;
  if (!loadBMPMeta(path, meta)) {
    oledHeader("BMP ERROR"); display.setCursor(0,20); display.print("Header error");
    display.display(); delay(4000); // 4 second pause to ensure you see it
    return;
  }

  const int totalSlices = meta.H;

  oledHeader("Print (Four Passes)");
  display.setCursor(0, 16); display.print(path);
  display.setCursor(0, 28);
  displayPrintf(display,"W=%d  H=%d", meta.W, meta.H);
  display.setCursor(0, 40); displayPrintf(display,"%d passes (1 rows ea.)", totalSlices);
  display.setCursor(0, 54); display.print("[Enter] Start  [-] Back");
  display.display();

  while (true) {
    if (readButton(btnEnter)) break;
    if (readButton(btnMinus)) return;
    delay(10);
  }

  countdown_1s_tenths("Starting Print");

  // ==========================================
  // DEBUG CHECKPOINT 1: Did we survive the countdown?
  // ==========================================
  oledHeader("DEBUG 1"); display.setCursor(0, 20);
  display.print("Countdown finished."); display.display(); delay(2000);

  if (totalSlices <= 0) {
     oledHeader("DEBUG ERROR"); display.setCursor(0, 20);
     display.print("totalSlices is 0!"); display.display(); delay(4000);
     return;
  }

  for (int slice = 0; slice < totalSlices; ++slice) {
    const int visualStart = slice;
    Image4x img;
    
    // ==========================================
    // DEBUG CHECKPOINT 2: Can we read the file?
    // ==========================================
    if (!readFourRowsBy(path, meta, visualStart, img)) {
      oledHeader("DEBUG ERROR");
      display.setCursor(0, 16); display.print("readFourRowsBy FAILED");
      display.setCursor(0, 28); display.print("Slice: "); display.print(slice);
      display.display(); delay(4000);
      return;
    }

    oledHeader("DEBUG 3"); display.setCursor(0, 20);
    display.print("File read OK. Moving..."); display.display(); delay(1500);

    const bool forward = (slice % 2 == 0);
    
    // ==========================================
    // THE DANGER ZONE: This enables motors & prints
    // ==========================================
    doPrintBMPSliceSerpentine(img, forward, true);

    // ==========================================
    // DEBUG CHECKPOINT 4: Did we survive the row?
    // ==========================================
    oledHeader("DEBUG 4"); display.setCursor(0, 20);
    display.print("Row printed!"); display.display(); delay(1500);

    if (slice < totalSlices - 1) {
      bool none[4] = {false,false,false,false};
      char buf[24];
      snprintf(buf, sizeof(buf), "Y +%.2fmm", CFG.mmPerPixel);
      drawStatusRow(none, String(buf));
      yMoveMM(CFG.mmPerPixel, true);
    }
  }

  oledHeader("Print");
  display.setCursor(0, 22); display.print("Entire image done.");
  display.display();
  delay(3000);
}

// ---------------------------------------------------------
// PLACEHOLDER: This function was called in your code but 
// missing from the raw text provided. Fill in your logic!
// ---------------------------------------------------------
void doPrintBMPSliceSerpentine(Image4x &img, bool forward, bool byRow) {
  digitalWrite(PIN_EN, LOW);

  const int lastP = img.width - 1 + 3 * OFFSET_PIXELS;
  const float expDiv = byRow ? 0.25f : 1.0f;

  if (forward) moveToPixel(0);
  else         moveToPixel(lastP);

  int pStart = forward ? 0     : lastP;
  int pEnd   = forward ? lastP : 0;
  int pStep  = forward ? +1    : -1;

  for (int p = pStart; ; p += pStep) {
    uint32_t durations[4] = {0, 0, 0, 0};
    bool willFire[4] = {false, false, false, false};
    int activeCount = 0;

    for (int k = 0; k < 4; k++) {
      const int t = p - k * OFFSET_PIXELS;
      if (t >= 0 && t < img.width) {
        uint8_t v = img.row[ ROW_FOR_LED[k] ][t];
        if (CFG.invertImage) v = 255 - v;
        
        // Removed LED_SCALE reference
        const float s = lutExposureFromByte(v) * expDiv;
        
        if (s > 0.0f) { 
          // Convert seconds to microseconds for the Nano serial payload
          durations[k] = (uint32_t)(s * 1000000.0f);
          willFire[k] = true; 
          ++activeCount; 
        }
      }
    }

    const String right = String("Pos ") + String(p) + "/" + String(lastP);

    if (activeCount > 0) {
      // Update OLED to show which LEDs are firing
      drawStatusRow(willFire, right);
      
      // Offload the timing and hardware execution to the Nano printhead
      sendBatchToPrinthead(durations);
      waitForPrinthead();
    } else {
      bool none[4] = {false, false, false, false};
      drawStatusRow(none, right);
    }

    if (p == pEnd) break;
    stepN(STEPS_PER_PIXEL, forward);
  }

  digitalWrite(PIN_EN, HIGH);
}