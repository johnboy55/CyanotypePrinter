#ifndef UV_PRINT_H
#define UV_PRINT_H
#include "uv_globals.h"

void sendBatchToPrinthead(uint32_t durations[4]);
void waitForPrinthead();
bool isProbeTriggered();
bool isZMaxSwitchPressed();

void reportEspIdleState();  // Tells the ESP32 STATE:ONLINE/OFFLINE per espOnline

// --- Printhead LED temperature (status page) ---
int queryNanoLedTemp();     // -1 = Nano not responding, else printhead LED temp in Celsius
void reportLedTempToEsp();  // Queries the Nano and forwards TEMP:<celsius> to the ESP32/ESP8266
void reportLedTemp(int t);  // Records + forwards an already-known reading (no round-trip)

// --- Operator cancel (Back button pressed mid-move/print) ---
// Whichever menu function *started* the move/print is responsible for resetting
// cancelRequested back to false once the operation has returned.
bool checkCancel();

void moveZStepsRelative(long deltaSteps);
void deployProbe();
void stowProbe();
void moveZSteps(long targetStepsZ);
void homeZ();
bool setZDistanceFromPaper(float paperGapMM);
float getInterpolatedZ(float x_mm, float y_mm);
void zPulse(bool dir);
float probeZ();
void stepPulseOnceX();
void yPulseLeft();
void yPulseRight();
void stepN(long n, bool dir);
void yStepPair(long steps, bool dir);
void moveToSteps(long target);
void moveToPixel(long p);
void yMoveMM(float mm, bool forward);
void moveYToZero();
void moveYToSteps(long target);
// Shared absolute X+Y mover used by homeXY(), moveToOffset(), and testOffset()'s return
// trip -- see its definition for why the stale-button/e-stop handling around each leg
// matters.
void moveToXYSteps(long targetX, long targetY, const __FlashStringHelper* title, const __FlashStringHelper* subtitle);
void homeXY();
void moveToOffset();
void moveClearOfBed();
void xMoveMM(float mm, bool right);
void zMoveMM(float mm, bool down);

void printEntireBMP(const String &path);
void printEntireBMP_4by(const String &path);
void countdown_1s_tenths(const __FlashStringHelper* label);

// Runs the exact same validated 4-pass print engine as printEntireBMP_4by(), but reads
// pixels straight from the in-memory hex string the ESP32 sent (no SD round-trip).
// skipStartPrompt: true when the caller (the ESP32 online flow) already got an explicit
// operator confirmation via its own vacuum/paper-seat screen, so the engine's own
// "[Enter] Start" gate would just be a redundant second confirmation.
void printHexData_4by(int w, int h, const String &hexData, bool skipStartPrompt = false);

// PROTOTYPES FOR MISSING FUNCTIONS
// applyInvert: whether CFG.invertImage should apply to this data -- true for BMPs,
// false for ESP32 hex data (which already encodes intensity directly).
void doPrintBMPSliceSerpentine(Image4x &img, bool forward, bool singlePass, bool applyInvert = true);
void printCalibrationPattern(float maxTimeSec);

#endif