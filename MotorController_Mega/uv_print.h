#ifndef UV_PRINT_H
#define UV_PRINT_H
#include "uv_globals.h"

void sendBatchToPrinthead(uint32_t durations[4]);
void waitForPrinthead();
bool isProbeTriggered();
bool isZMaxSwitchPressed();

// --- Emergency stop ---
int queryNanoLimitMask();   // -1 = Nano not responding, else bit0=Z bit1=Y bit2=overtemp
void reportEspIdleState();  // Tells the ESP32 STATE:ONLINE/OFFLINE per espOnline/eStopLatched
bool checkEStop();          // Services the raw ISR flag; returns true if caller should abort its loop
void clearEmergencyStop();  // Operator-initiated resume after inspecting the cause

// --- Operator cancel (Back button pressed mid-move/print) ---
// Unlike checkEStop(), this is not a hardware fault: it doesn't latch or need the Nano
// re-checked. Whichever menu function *started* the move/print is responsible for
// resetting cancelRequested back to false once the operation has returned.
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
void yStepPair(long stepsL, long stepsR, bool dir);
void moveToSteps(long target);
void moveToPixel(long p);
void yMoveMM(float mm, bool forward);
void yMoveSkewMM(float baseMM, float deltaRightMM, bool forward);
void moveYToZero();
void moveYToSteps(long target);
// Shared absolute X+Y mover used by homeXY(), moveToOffset(), and testOffset()'s return
// trip -- see its definition for why the stale-button/e-stop handling around each leg
// matters.
void moveToXYSteps(long targetX, long targetY, const char* title, const char* subtitle);
void homeXY();
void moveToOffset();
void moveClearOfBed();
void xMoveMM(float mm, bool right);
void zMoveMM(float mm, bool down);

void printEntireBMP(const String &path);
void printEntireBMP_4by(const String &path);
void countdown_1s_tenths(const char* label);

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