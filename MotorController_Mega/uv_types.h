#ifndef UV_TYPES_H
#define UV_TYPES_H

#include <Arduino.h>

// --- Memory Allocation ---
// Defines the maximum width of a printable slice. 
// Set to 32 to safely accommodate the "8x32" web UI preset.
#define MAX_IMG_WIDTH 32 

// --- Button Debounce Structure ---
struct Btn {
  uint8_t pin;
  bool activeHigh;
  uint32_t lastChange;
  bool last;
  bool pressedEvent;
};

// --- Image Slice Buffer Structure ---
// Holds a 4-row chunk of the incoming pixel data to be printed 
// in one physical serpentine pass.
struct Image4x {
  int width;
  uint8_t row[4][MAX_IMG_WIDTH];
};

// --- System Configuration Structure ---
// Holds the default physical layout constants. These are automatically 
// overwritten if a "config.txt" file is found on the SD card during boot.
struct Config {
  float yBandAdvanceMM = 4.0f;
  float mmPerPixel = 2.0f;
  float ledSpacingMM = 20.0f;
  bool invertImage = false;
};

// --- Global Configuration Object ---
// Instantiates the CFG object used throughout the Mega's logic
Config CFG;

#endif