#ifndef UV_TYPES_H
#define UV_TYPES_H

#include <Arduino.h>

#define MAX_IMG_WIDTH 32 

struct Btn {
  uint8_t pin;
  bool activeHigh;
  uint32_t lastChange;
  bool last;
  bool pressedEvent;
};

struct Image4x {
  int width;
  uint8_t row[4][MAX_IMG_WIDTH];
  // True for rows filled with real source data; false for rolling-window padding
  // rows (before the first row / past the last row of the image) that must never
  // be exposed, regardless of CFG.invertImage.
  bool valid[4];
};

// SD browser entry for the OLED file browser
struct Entry {
  String   name;
  bool     isDir;
  uint32_t size;
};

struct Config {
  float yBandAdvanceMM = 4.0f;
  float mmPerPixel = 2.0f;
  float ledSpacingMM = 7.0f;
  bool invertImage = false;
  float ledScale[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // Added missing array
  int stepPeriodUs = 1000;                      // Added missing var
  int stepPulseUs = 5;                          // Added missing var
  // Where the printhead moves to before a print starts (instead of true X=0/Y=0), so the
  // print can be centered on paper that isn't aligned to the machine's absolute origin.
  float offsetXmm = 0.0f;
  float offsetYmm = 0.0f;
};

extern Config CFG; // Declared as extern to prevent duplicate definitions

#endif