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
  float ledSpacingMM = 20.0f;
  bool invertImage = false;
  float ledScale[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // Added missing array
  int stepPeriodUs = 1000;                      // Added missing var
  int stepPulseUs = 5;                          // Added missing var
};

extern Config CFG; // Declared as extern to prevent duplicate definitions

#endif