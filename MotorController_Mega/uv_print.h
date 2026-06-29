#ifndef UV_PRINT_H
#define UV_PRINT_H
#include "uv_globals.h"

void sendBatchToPrinthead(uint32_t durations[4]);
void waitForPrinthead();
bool isProbeTriggered();
bool isZMaxSwitchPressed();
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
void xMoveMM(float mm, bool right);
void zMoveMM(float mm, bool down);

void printEntireBMP(const String &path);
void printEntireBMP_4by(const String &path);
void countdown_1s_tenths(const char* label);

// PROTOTYPES FOR MISSING FUNCTIONS
void doPrintBMPSliceSerpentine(Image4x &img, bool forward, bool singlePass);

#endif