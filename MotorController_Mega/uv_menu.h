#ifndef UV_MENU_H
#define UV_MENU_H
#include "uv_globals.h"

void displayPrintf(Adafruit_SSD1306 &disp, const char *fmt, ...);
bool readButton(Btn &b);
bool buttonHeld(const Btn &b);
void oledHeader(const char* title);
void drawStatusRow(const bool ledOn[4], const String &rightText);

void menuMain();
void menuXCtrl();
void menuZCtrl();
void menuZCalib();
void menuMotor();
void menuYCtrl();
void menuCalib();
void menuTest();
void menuBrowser();
void menuPrint();
void menuUtilities();
void menuESP32Control();

// PROTOTYPES FOR MISSING FUNCTIONS
bool listFilesToOLED(const char* dir, const char* ext, String &picked);
void testFireLEDs();
void testSerialRelay();
void menuFileBrowser();
void zeroXHere();
void measureMatrix();
void loadMatrixFile();
uint8_t  hexCharToVal(char c);

#endif