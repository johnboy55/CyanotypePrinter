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
    if (checkEStop() || checkCancel()) return;
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
    // Don't classify/query the Nano from inside this wait -- that would overlap two
    // outstanding requests on the same UART. Just bail out; the caller's stepping loop
    // calls checkEStop() on its next iteration, once this exchange has finished.
    if (emergencyStopTriggered) break;
  }
  if (PRINTHEAD_SERIAL.available()) {
    String response = PRINTHEAD_SERIAL.readStringUntil('\n'); response.trim();
    if (response == "P:1") return true;
  }
  return false;
}

bool isZMaxSwitchPressed() {
  // PIN_PROBE_SIG on the Nano IS the Z limit switch (same signal isProbeTriggered() reads);
  // there's no separate Mega-side pin for it any more, so just reuse that query.
  return isProbeTriggered();
}

// ---------- Emergency Stop ----------
// emergencyStopTriggered (volatile, ISR-set) is a raw, unclassified signal: the Nano ORs
// its Z limit, Y limit, and thermal cutoff onto one line, so a rising/falling edge alone
// doesn't say which one fired, or whether it was an *expected* touch (e.g. Z homing).
// eStopLatched is the classified, operator-visible result: once true, all motion stops
// until clearEmergencyStop() is called from the menu.
static bool eStopAnnounced = false;

int queryNanoLimitMask() {
  PRINTHEAD_SERIAL.print("L\n");
  unsigned long timeout = millis();
  while (!PRINTHEAD_SERIAL.available() && millis() - timeout < 100) {}
  if (PRINTHEAD_SERIAL.available()) {
    String resp = PRINTHEAD_SERIAL.readStringUntil('\n'); resp.trim();
    if (resp.startsWith("L:")) return resp.substring(2).toInt();
  }
  return -1; // Nano not responding -- never assume this means "safe".
}

// T[0] from the Nano's "T:t0,t1,t2,t3,t4,t5" reply is the printhead LED temperature (C).
int queryNanoLEDTempC() {
  PRINTHEAD_SERIAL.print("T\n");
  unsigned long timeout = millis();
  while (!PRINTHEAD_SERIAL.available() && millis() - timeout < 100) {}
  if (PRINTHEAD_SERIAL.available()) {
    String resp = PRINTHEAD_SERIAL.readStringUntil('\n'); resp.trim();
    if (resp.startsWith("T:")) {
      String rest = resp.substring(2);
      int comma = rest.indexOf(',');
      return (comma > 0 ? rest.substring(0, comma) : rest).toInt();
    }
  }
  return -100; // Nano not responding -- caller should render this as "--" rather than a real reading.
}

static String describeEStopCause(int mask) {
  if (mask < 0) return "NANO OFFLINE";
  if (mask == 0) return "UNKNOWN";
  String s;
  if (mask & 0x1) s += "Z ";
  if (mask & 0x2) s += "Y ";
  if (mask & 0x4) s += "OVERTEMP ";
  return s;
}

void handleEmergencyStop() {
  digitalWrite(PIN_EN, HIGH); // Always keep drivers disabled while a stop condition is live.

  if (eStopLatched) return; // Already latched; callers check eStopLatched/checkEStop() to abort.

  int mask = queryNanoLimitMask();
  bool zOnly = (mask == 0x1);
  bool expectedTouch = zHomingActive && zOnly;

  emergencyStopTriggered = false; // Consumed either way; classification below decides the outcome.

  if (expectedTouch) return; // Intentional Z contact during homing/probing -- not a real stop.

  eStopLatched = true;
  eStopCause = describeEStopCause(mask);

  if (!eStopAnnounced) {
    eStopAnnounced = true;
    Serial.print(F("E-STOP LATCHED: ")); Serial.println(eStopCause);
  }
}

bool checkEStop() {
  if (emergencyStopTriggered) handleEmergencyStop();
  return eStopLatched;
}

void clearEmergencyStop() {
  eStopLatched = false;
  eStopAnnounced = false;
  eStopCause = "";
  emergencyStopTriggered = false;
  digitalWrite(PIN_EN, HIGH); // Stay disabled; the next intentional move re-enables it.
}

bool checkCancel() {
  if (readButton(btnBack)) cancelRequested = true;
  return cancelRequested;
}

void moveZStepsRelative(long deltaSteps) {
  if (deltaSteps == 0) return;
  bool dir = deltaSteps > 0;
  long steps = abs(deltaSteps);
  digitalWrite(PIN_Z_DIR, dir ? HIGH : LOW);
  delayMicroseconds(5); // Setup time for DIR pin
  
  // Z-axis lead screws need to run slower than X/Y belts to avoid stalling.
  int zDelay = max(1000, STEP_PERIOD_US * 4);
  
  for (long i = 0; i < steps; ++i) {
    if (checkEStop() || checkCancel()) break;
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ += dir ? 1 : -1;
    delayMicroseconds(zDelay);
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
  digitalWrite(PIN_Z_DIR, dir ? HIGH : LOW);
  delayMicroseconds(5);
  
  int zDelay = max(1000, STEP_PERIOD_US * 4);

  for (long i = 0; i < steps; ++i) {
    if (checkEStop() || checkCancel()) break;
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ += dir ? 1 : -1;
    delayMicroseconds(zDelay);
  }
}

void homeZ() {
  zHomingActive = true;
  int zDelay = max(1000, STEP_PERIOD_US * 4);

  digitalWrite(PIN_Z_DIR, LOW);
  delayMicroseconds(5);
  while (!isZMaxSwitchPressed()) {
    if (checkEStop() || checkCancel()) { zHomingActive = false; return; }
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ -= 1;
    delayMicroseconds(zDelay);
  }

  currentStepsZ = 0;

  long approachSteps = lround_double(Z_MAX_TO_CR_TOUCH_APPROACH_MM * STEPS_PER_MM_Z);
  moveZStepsRelative(approachSteps);
  if (eStopLatched || cancelRequested) { zHomingActive = false; return; }

  deployProbe();

  digitalWrite(PIN_Z_DIR, HIGH);
  delayMicroseconds(5);
  while (!isProbeTriggered()) {
    if (checkEStop() || checkCancel()) { zHomingActive = false; return; }
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ += 1;
    delayMicroseconds(zDelay);
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
  digitalWrite(PIN_Z_DIR, dir ? HIGH : LOW); 
  delayMicroseconds(2);
  digitalWrite(PIN_Z_STEP, HIGH);
  delayMicroseconds(STEP_PULSE_US); 
  digitalWrite(PIN_Z_STEP, LOW);
  currentStepsZ += dir ? 1 : -1;
}

float probeZ() {
  deployProbe();
  int zDelay = max(1000, STEP_PERIOD_US * 4);
  digitalWrite(PIN_Z_DIR, HIGH);
  delayMicroseconds(5);
  zHomingActive = true; // The probe touch here is expected -- don't let it trip a full e-stop.
  while (!isProbeTriggered()) {
    if (checkEStop() || checkCancel()) { zHomingActive = false; stowProbe(); return 0.0f; }
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ += 1;
    delayMicroseconds(zDelay);
  }
  zHomingActive = false;
  float trigger_height = currentStepsZ / STEPS_PER_MM_Z;
  stowProbe();

  digitalWrite(PIN_Z_DIR, LOW);
  delayMicroseconds(5);
  for (int i = 0; i < (2.0f * STEPS_PER_MM_Z); i++) {
    if (checkEStop() || checkCancel()) return trigger_height;
    digitalWrite(PIN_Z_STEP, HIGH);
    delayMicroseconds(STEP_PULSE_US);
    digitalWrite(PIN_Z_STEP, LOW);
    currentStepsZ -= 1;
    delayMicroseconds(zDelay);
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
    if (checkEStop() || checkCancel()) break;
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
    if (checkEStop() || checkCancel()) break;
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

void moveToSteps(long target) { 
  long d = target - currentSteps; 
  if (d) {
    int oldPeriod = STEP_PERIOD_US;
    STEP_PERIOD_US = max(STEP_PULSE_US + 10, oldPeriod / 4);
    stepN(labs(d), d > 0); 
    STEP_PERIOD_US = oldPeriod;
  }
}

void moveToPixel(long p) { moveToSteps(p * (long)STEPS_PER_PIXEL); }

void yMoveMM(float mm, bool forward) { 
  long s = lround_double(mm * STEPS_PER_MM_Y); 
  int oldPeriod = STEP_PERIOD_US;
  STEP_PERIOD_US = max(STEP_PULSE_US + 10, oldPeriod / 4);
  yStepPair(s, s, forward); 
  STEP_PERIOD_US = oldPeriod;
}

void yMoveSkewMM(float baseMM, float deltaRightMM, bool forward) {
  long sL = lround_double(baseMM * STEPS_PER_MM_Y);
  long sR = lround_double((baseMM + deltaRightMM) * STEPS_PER_MM_Y);
  if (sR < 0) sR = 0;
  int oldPeriod = STEP_PERIOD_US;
  STEP_PERIOD_US = max(STEP_PULSE_US + 10, oldPeriod / 4);
  yStepPair(sL, sR, forward);
  STEP_PERIOD_US = oldPeriod;
}

// Y-axis counterpart to moveToSteps(0) for X: returns the carriage to Y=0 from wherever
// currentStepsY currently tracks. yStepPair's dir=true increases currentStepsY, so we
// move backward (dir=false) from a positive count and forward from a negative one.
void moveYToZero() {
  if (currentStepsY == 0) return;
  bool forward = (currentStepsY < 0);
  float mm = abs((float)currentStepsY / STEPS_PER_MM_Y);
  yMoveMM(mm, forward);
}

void xMoveMM(float mm, bool right) { 
  long s = lround_double(mm * STEPS_PER_MM_X); 
  int oldPeriod = STEP_PERIOD_US;
  STEP_PERIOD_US = max(STEP_PULSE_US + 10, oldPeriod / 4);
  stepN(s, right); 
  STEP_PERIOD_US = oldPeriod;
}

void zMoveMM(float mm, bool down) { 
  long s = lround_double(mm * STEPS_PER_MM_Z); 
  if (s == 0) return; 
  moveZStepsRelative(down ? s : -s); 
}

// ---------- BMP Parsing Logic ----------

/**
 * @brief Reads the first 4 visual rows of a BMP image file into an Image4x buffer.
 * 
 * Opens a BMP file from the SD card, parses the file and info headers to validate 
 * the format (supports uncompressed 8-bit grayscale and 24-bit RGB), and extracts 
 * the first 4 visual rows. Automatically handles bottom-up and top-down BMP row 
 * orientations and converts 24-bit RGB data into 8-bit grayscale on the fly.
 * 
 * @param path The absolute path to the BMP file on the SD card.
 * @param img  Reference to an Image4x struct where the extracted width and pixel 
 *             row data will be stored.
 * @return true if the file was successfully read and parsed, false otherwise.
 */
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
  for (int r = 0; r < 4; r++) {
    img.valid[r] = false;
    for (int c = 0; c < MAX_IMG_WIDTH; c++)
      img.row[r][c] = 0;
  }
  File f = SD.open(path);
  if (!f) return false;

  uint8_t rowbuf[MAX_ROWBUF];
  if (m.rowStride > (int)sizeof(rowbuf)) { f.close(); return false; }

  for (int vr = 0; vr < 4; ++vr) {
    const int visualRow = visualStart + vr;
    if (visualRow >= m.H) break;
    img.valid[vr] = true;

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

/**
 * @brief Reads a rolling window of 4 visual rows from a BMP file into an Image4x buffer.
 * 
 * Unlike readFourRowsAt (which reads forward starting from visualStart), this function 
 * reads 4 rows *ending* at visualStart (specifically rows: visualStart-3, visualStart-2, 
 * visualStart-1, and visualStart). This is used for staggered/interlaced printing passes
 * where physical printhead LEDs are offset by rows. Any row indices that fall below 0 
 * are left as blank (0). Automatically converts 24-bit RGB data into 8-bit grayscale.
 * 
 * @param path        The absolute path to the BMP file on the SD card.
 * @param m           Pre-loaded BMP metadata (dimensions, stride, orientation).
 * @param visualStart The visual row index corresponding to the final row in the 4-row window.
 * @param img         Reference to an Image4x struct where the extracted pixel data is stored.
 * @return true if the file was successfully read and parsed, false on file/read errors.
 */
// ---------- Row source abstraction ----------
// Lets the 4-pass print engine (printFromRowSource, below) pull rows from either an SD
// BMP file or the ESP32's in-memory hex string, without buffering more than a few rows
// at a time -- the Mega's 8KB SRAM can't hold a full print image either way.
class RowSource {
public:
  int width = 0;
  int height = 0;
  // Whether CFG.invertImage should apply to this source. BMP files use it (their
  // grayscale convention needs inverting for correct UV exposure); the ESP32's hex
  // payload already encodes intensity directly -- higher value the operator drew
  // darker, which should mean *more* exposure -- so it must never be inverted.
  bool wantsInvert = true;
  virtual bool readRow(int row, uint8_t *out, int cols) = 0; // fills out[0..cols-1], cols <= width
  virtual ~RowSource() {}
};

class BMPFileRowSource : public RowSource {
  File f;
  BMPFILEHEADER fh;
  BMPINFOHEADER ih;
  bool bottomUp = false;
  int rowStride = 0;
public:
  bool open(const String &path) {
    f = SD.open(path);
    if (!f) return false;
    if (f.read((uint8_t*)&fh, sizeof(fh)) != sizeof(fh) || fh.bfType != 0x4D42) { f.close(); return false; }
    if (f.read((uint8_t*)&ih, sizeof(ih)) != sizeof(ih)) { f.close(); return false; }
    if (ih.biCompression != 0 || (ih.biBitCount != 24 && ih.biBitCount != 8)) { f.close(); return false; }

    bottomUp = (ih.biHeight > 0);
    height   = abs(ih.biHeight);
    width    = ih.biWidth;
    const int rowBytesRaw = (ih.biBitCount * width + 7) / 8;
    rowStride = (rowBytesRaw + 3) & ~3;
    if (rowStride > MAX_ROWBUF) { f.close(); return false; }
    return true;
  }

  bool readRow(int row, uint8_t *out, int cols) override {
    if (!f) return false;
    const int srcRow = bottomUp ? (height - 1 - row) : row;
    const uint32_t rowOffset = fh.bfOffBits + (uint32_t)srcRow * rowStride;
    uint8_t rowbuf[MAX_ROWBUF];
    f.seek(rowOffset);
    if (f.read(rowbuf, rowStride) != rowStride) return false;
    if (ih.biBitCount == 24) {
      for (int c = 0; c < cols; c++) {
        const int idx = c * 3;
        const uint8_t B = rowbuf[idx + 0], G = rowbuf[idx + 1], R = rowbuf[idx + 2];
        out[c] = (uint8_t)(0.299f*R + 0.587f*G + 0.114f*B);
      }
    } else {
      for (int c = 0; c < cols; c++) out[c] = rowbuf[c];
    }
    return true;
  }

  ~BMPFileRowSource() override { if (f) f.close(); }
};

class HexRowSource : public RowSource {
  const String *hexData;
public:
  void init(int w, int h, const String &hex) { width = w; height = h; hexData = &hex; wantsInvert = false; }
  bool readRow(int row, uint8_t *out, int cols) override {
    for (int c = 0; c < cols; c++) {
      const int idx = row * width + c;
      out[c] = (idx < (int)hexData->length()) ? (uint8_t)(hexCharToVal((*hexData)[idx]) * 17) : 0;
    }
    return true;
  }
};

bool readFourRowsBy(RowSource &src, int visualStart, Image4x &img) {
  img.width = min(MAX_IMG_WIDTH, src.width);
  for (int r = 0; r < 4; r++) {
    img.valid[r] = false;
    for (int c = 0; c < MAX_IMG_WIDTH; c++)
      img.row[r][c] = 0;
  }

  for (int vr = 0; vr < 4; ++vr) {
    const int visualRow = visualStart - (3 - vr);
    // Outside the image (before row 0, or past the last row while the trailing
    // slices finish exposing it): leave this slot blank/invalid, not an error.
    if (visualRow < 0 || visualRow >= src.height) continue;
    if (!src.readRow(visualRow, img.row[vr], img.width)) return false;
    img.valid[vr] = true;
  }
  return true;
}

void countdown_1s_tenths(const char* label) {
  oledHeader(label);
  display.setCursor(0, 20); display.print("Starting in:");
  display.sendBuffer();
  for (int i = 10; i >= 0; --i) {
    display.setDrawColor(0); display.drawBox(0, 35, 128, 20); display.setDrawColor(1);
    display.setCursor(20, 35);
    // Closest u8g2 stand-in for Adafruit GFX's setTextSize(2); may need retuning to fit
    // the 20px-tall cleared box once you see it on the actual panel.
    display.setFont(u8g2_font_ncenB14_tr);
    displayPrintf(display, "%d.%d s", i/10, i%10);
    display.setFont(u8g2_font_6x10_tf);
    display.sendBuffer();
    delay(100);
  }
}

// ---------- Print Execution ----------

/**
 * @brief Prints a BMP image by processing it in non-overlapping horizontal bands (4 rows per band).
 * 
 * Displays a UI prompt to start, then iterates through the image in slices of 4 rows.
 * The printhead moves in a serpentine pattern (alternating left-to-right and right-to-left)
 * to save travel time. After each band, the Y-axis advances by the configured band width.
 * 
 * @param path The absolute path to the BMP file on the SD card.
 */
void printEntireBMP(const String &path) {
  BMPMeta meta;
  if (!loadBMPMeta(path, meta)) {
    oledHeader("BMP"); display.setCursor(0,20); display.print("Header error");
    display.setCursor(0,32); display.print(path); display.sendBuffer(); delay(1200);
    return;
  }

  const int totalSlices = (meta.H + 3) / 4;

  oledHeader("Print (Serpentine)");
  display.setCursor(0, 16); display.print(path);
  display.setCursor(0, 28);
  displayPrintf(display,"W=%d  H=%d", meta.W, meta.H);
  display.setCursor(0, 40); displayPrintf(display,"%d slices (4 rows ea.)", totalSlices);
  display.setCursor(0, 54); display.print("[Enter] Start  [-] Back");
  display.sendBuffer();

  while (true) {
    if (checkEStop()) return;
    if (readButton(btnEnter)) break;
    if (readButton(btnMinus)) return;
    delay(10);
  }

  countdown_1s_tenths("Starting Print");
  for (int slice = 0; slice < totalSlices; ++slice) {
    if (eStopLatched || cancelRequested) return;
    const int visualStart = slice * 4;
    Image4x img;
    if (!readFourRowsAt(path, meta, visualStart, img)) {
      oledHeader("BMP");
      display.setCursor(0, 20);
      display.print("Read error at slice ");
      display.print(slice+1); display.sendBuffer(); delay(1200);
      return;
    }

    oledHeader("Printing...");
    display.setCursor(0, 18); display.print(path);
    display.setCursor(0, 30);
    displayPrintf(display,"Slice %d/%d", slice+1, totalSlices);
    display.setCursor(0, 42); displayPrintf(display,"Rows %d..%d", visualStart, min(meta.H-1, visualStart+3));
    display.sendBuffer();
    const bool forward = (slice % 2 == 0);
    doPrintBMPSliceSerpentine(img, forward, false, true);
    if (slice < totalSlices - 1) {
      bool none[4] = {false,false,false,false};
      char buf[24];
      snprintf(buf, sizeof(buf), "Y +%.2fmm", CFG.yBandAdvanceMM);

/**
 * @brief Prints a BMP image using an interlaced/staggered approach, advancing one row at a time.
 * 
 * Unlike the banded approach, this function advances the Y-axis by exactly one pixel (CFG.mmPerPixel)
 * after each X-axis pass. It uses a rolling window of 4 rows to accommodate the physical staggered 
 * layout of the UV LEDs. Each physical pixel receives 1/4th of its required exposure per pass, 
 * totaling full exposure over 4 passes. Includes visual debugging checkpoints on the OLED.
 * 
 * @param path The absolute path to the BMP file on the SD card.
 */
      drawStatusRow(none, String(buf));
      yMoveMM(CFG.yBandAdvanceMM, true);
    }
  }

  oledHeader("Print");
  display.setCursor(0, 22); display.print("Entire image done.");
  display.sendBuffer();
  delay(1200);
}
// The validated 4-pass interlaced print sequence -- shared by SD-file prints and
// ESP32-triggered prints, driven purely off the RowSource interface so it doesn't care
// which one it's got.
static void printFromRowSource(RowSource &src, const String &label, bool skipStartPrompt) {
  if (src.width <= 0 || src.height <= 0) {
    oledHeader("Print Error"); display.setCursor(0, 20);
    display.print("Bad image"); display.sendBuffer(); delay(2000);
    return;
  }

  // The 4-row rolling window needs 3 extra trailing slices past the last real row so
  // that row also accumulates its full 4x0.25 exposure (otherwise the last 3 rows of
  // every print fade out, only receiving 75%/50%/25% of their dose).
  const int totalSlices = src.height + 3;

  if (!skipStartPrompt) {
    oledHeader("Print (Four Passes)");
    display.setCursor(0, 16); display.print(label);
    display.setCursor(0, 28);
    displayPrintf(display,"W=%d  H=%d", src.width, src.height);
    display.setCursor(0, 40); displayPrintf(display,"%d passes (1 rows ea.)", totalSlices);
    display.setCursor(0, 54); display.print("[Enter] Start  [-] Back");
    display.sendBuffer();

    while (true) {
      if (checkEStop()) return;
      if (readButton(btnEnter)) break;
      if (readButton(btnMinus)) return;
      delay(10);
    }
  }

  countdown_1s_tenths("Starting Print");

  for (int slice = 0; slice < totalSlices; ++slice) {
    if (eStopLatched || cancelRequested) return;
    const int visualStart = slice;
    Image4x img;

    if (!readFourRowsBy(src, visualStart, img)) {
      oledHeader("Print Error");
      display.setCursor(0, 16); display.print("Read failed");
      display.setCursor(0, 28); display.print("Slice: "); display.print(slice);
      display.sendBuffer(); delay(2000);
      return;
    }

    // Row/temp are steady for the whole slice, so redraw them once here rather than
    // on every column -- doPrintBMPSliceSerpentine only touches the footer LED/col row.
    const int rowShown = min(slice + 1, src.height);
    const int tempC = queryNanoLEDTempC();
    oledHeader("Printing");
    display.setCursor(0, 12); displayPrintf(display, "Row %d/%d", rowShown, src.height);
    display.setCursor(70, 12);
    if (tempC > -100) displayPrintf(display, "T:%dC", tempC); else display.print("T:--");
    display.setCursor(0, 24); display.print(label);
    display.sendBuffer();

    const bool forward = (slice % 2 == 0);
    doPrintBMPSliceSerpentine(img, forward, true, src.wantsInvert);

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
  display.sendBuffer();
  delay(1200);
}

void printEntireBMP_4by(const String &path) {
  BMPFileRowSource src;
  if (!src.open(path)) {
    oledHeader("BMP ERROR"); display.setCursor(0,20); display.print("Header error");
    display.sendBuffer(); delay(2000);
    return;
  }
  printFromRowSource(src, path, false);
}

void printHexData_4by(int w, int h, const String &hexData, bool skipStartPrompt) {
  HexRowSource src;
  src.init(w, h, hexData);
  printFromRowSource(src, "ESP32", skipStartPrompt);
}

/**
 * @brief Executes the physical movements and UV exposures for a single X-axis pass.
 * 
 * Moves the printhead pixel-by-pixel along the X-axis. At each pixel, calculates the 
 * appropriate exposure duration for each of the 4 LEDs based on their physical X-offsets.
 * The exposure values are looked up via a LUT, optionally inverted, scaled down if 
 * interlaced printing is active (byRow), and sent to the Nano printhead via serial. 
 * The function waits for the Nano to finish exposing before stepping to the next pixel.
 * 
 * @param img         The Image4x buffer containing the pixel data for the current rows.
 * @param forward     True to sweep forward (pixel 0 to end), false for reverse.
 * @param byRow       If true, reduces exposure by a factor of 4 (used for interlaced printing).
 * @param applyInvert Whether CFG.invertImage should apply to this data (see RowSource::wantsInvert).
 */

void doPrintBMPSliceSerpentine(Image4x &img, bool forward, bool byRow, bool applyInvert) {
  digitalWrite(PIN_EN, LOW);

  const float mm_per_pixel = MM_PER_PIXEL; 
  const float led_spacing = LED_SPACING_MM;
  const float step_mm = 1.0f;
  const long steps_per_1mm = lround_double(step_mm * STEPS_PER_MM_X);

  const float max_x_mm = (img.width - 1) * mm_per_pixel + 3.0f * led_spacing;
  const int total_steps = (int)(max_x_mm / step_mm) + 1;

  if (forward) moveToSteps(0);
  else         moveToSteps(lround_double(max_x_mm * STEPS_PER_MM_X));

  int sStart = forward ? 0 : total_steps - 1;
  int sEnd   = forward ? total_steps - 1 : 0;
  int sStep  = forward ? 1 : -1;

  const float expDiv = byRow ? 0.25f : 1.0f;

  for (int s = sStart; ; s += sStep) {
    if (eStopLatched || cancelRequested) break;
    float x_carriage = s * step_mm;
    uint32_t durations[4] = {0, 0, 0, 0};
    bool willFire[4] = {false, false, false, false};
    int activeCount = 0;

    for (int k = 0; k < 4; k++) {
      // (3 - k) ensures LED 3 has 0mm offset (Leading)
      // and LED 0 has a 21mm offset (Trailing)
      float x_led = x_carriage - (3 - k) * led_spacing;
      long x_led_mm = lround_double(x_led);
      
      // A pixel t is at t * mm_per_pixel.
      // We check if x_led matches any t * mm_per_pixel.
      // Since mm_per_pixel = 2.0, we check if x_led is even and >= 0.
      if (x_led_mm >= 0 && (x_led_mm % 2 == 0)) {
        int t = x_led_mm / 2;
        if (t < img.width && img.valid[ ROW_FOR_LED[k] ]) {
          uint8_t v = img.row[ ROW_FOR_LED[k] ][t];
          if (CFG.invertImage && applyInvert) v = 255 - v;
          const float dur_s = lutExposureFromByte(v) * expDiv;
          if (dur_s > 0.0f) { 
            durations[k] = (uint32_t)(dur_s * 1000000.0f);
            willFire[k] = true; 
            ++activeCount; 
          }
        }
      }
    }

    if (activeCount > 0) {
      // LED 3 (k=3) is the leading LED with 0mm offset, so its target column is the
      // most representative "current column" for the whole sweep at this instant.
      int col = (int)(x_carriage / mm_per_pixel);
      if (col < 0) col = 0;
      if (col >= img.width) col = img.width - 1;
      char buf[32];
      snprintf(buf, sizeof(buf), "Col %d/%d", col + 1, img.width);
      drawStatusRow(willFire, String(buf));
      sendBatchToPrinthead(durations);
      waitForPrinthead();
    }

    if (s == sEnd) break;
    stepN(steps_per_1mm, forward);
  }

  digitalWrite(PIN_EN, HIGH);
}

/**
 * @brief Calibration pattern to isolate individual LEDs and reveal X-axis backlash.
 *
 * Rows 0-7: fires only LED 3 (the leading LED) at a fixed X target on alternating
 * forward/backward passes. Any offset between the forward-pass and backward-pass marks
 * is backlash. Rows 8-15: fires all 4 LEDs into 4 separate X bands (12mm apart) at
 * 33/66/100% of maxTimeSec, so each LED's exposure response can be read independently.
 *
 * @param maxTimeSec Exposure duration used as the 100% reference for the gradient test.
 */
void printCalibrationPattern(float maxTimeSec) {
  digitalWrite(PIN_EN, LOW);

  // Constants to match your machine's physical reality
  const float step_mm = 1.0f;
  const float max_x_mm = 65.0f; // Wide enough for all LEDs to clear their test zones
  const long steps_per_1mm = lround_double(step_mm * STEPS_PER_MM_X);
  const int total_steps = (int)(max_x_mm / step_mm) + 1;

  // We will print exactly 16 rows
  for (int row = 0; row < 16; ++row) {
    if (eStopLatched || cancelRequested) break;

    bool forward = (row % 2 == 0);

    if (forward) moveToSteps(0);
    else         moveToSteps(lround_double(max_x_mm * STEPS_PER_MM_X));

    int sStart = forward ? 0 : total_steps - 1;
    int sEnd   = forward ? total_steps - 1 : 0;
    int sStep  = forward ? 1 : -1;

    for (int s = sStart; ; s += sStep) {
      if (eStopLatched || cancelRequested) break;

      float x_carriage = s * step_mm;
      uint32_t durations[4] = {0, 0, 0, 0};
      bool willFire[4] = {false, false, false, false};
      int activeCount = 0;

      for (int k = 0; k < 4; k++) {
        // Using our confirmed physical X-axis mapping
        float x_led = x_carriage - (3 - k) * LED_SPACING_MM;
        long x_led_mm = lround_double(x_led);

        // ==========================================
        // TEST 1: THE BACKLASH "ZIPPER" (Rows 0-7)
        // ==========================================
        if (row < 8) {
          // We fire ONLY LED 3 (Top) at exactly X = 10mm.
          // Because we print bi-directionally, this will reveal backlash.
          if (k == 3 && x_led_mm == 10) {
            durations[k] = (uint32_t)(maxTimeSec * 1000000.0f);
            willFire[k] = true;
            activeCount++;
          }
        }
        // ==========================================
        // TEST 2: LED EXPOSURE GRADIENTS (Rows 8-15)
        // ==========================================
        else {
          // We space the LEDs out into 4 distinct blocks across the paper
          // LED 3 starts at 4mm, LED 2 at 16mm, LED 1 at 28mm, LED 0 at 40mm
          int target_base_x = 4 + (3 - k) * 12;

          // Print 3 pixels for each LED, scaling the exposure time based on maxTimeSec
          if (x_led_mm == target_base_x) {
            durations[k] = (uint32_t)(maxTimeSec * 0.33f * 1000000.0f); // 33% Density
            willFire[k] = true;
            activeCount++;
          }
          else if (x_led_mm == target_base_x + 2) {
            durations[k] = (uint32_t)(maxTimeSec * 0.66f * 1000000.0f); // 66% Density
            willFire[k] = true;
            activeCount++;
          }
          else if (x_led_mm == target_base_x + 4) {
            durations[k] = (uint32_t)(maxTimeSec * 1.00f * 1000000.0f); // 100% Density
            willFire[k] = true;
            activeCount++;
          }
        }
      }

      if (activeCount > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "R%d X%.0fmm", row, x_carriage);
        drawStatusRow(willFire, String(buf));
        sendBatchToPrinthead(durations);
        waitForPrinthead();
      }

      if (s == sEnd) break;
      stepN(steps_per_1mm, forward);
    }

    if (eStopLatched || cancelRequested) break;

    // Move down 1 pixel (2mm) for the next row
    yMoveMM(MM_PER_PIXEL, true);
  }

  digitalWrite(PIN_EN, HIGH);
}