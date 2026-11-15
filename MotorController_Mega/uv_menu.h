#ifndef UV_MENU_H
#define UV_MENU_H
#include "uv_globals.h"

void displayPrintf(OledDisplay &disp, const char *fmt, ...);
void displayPrintf(OledDisplay &disp, const __FlashStringHelper *fmt, ...);
bool readButton(Btn &b);
bool buttonHeld(const Btn &b);
void oledHeader(const char* title);
void oledHeader(const __FlashStringHelper* title);
void drawStatusRow(const bool ledOn[4], const String &rightText);
void drawTempReadout(int tempC);
String xyPositionString();
void setLastEspIp(const String &ip);

void menuMain();
void menuXCtrl();
void menuZCtrl();
void menuZCalib();
void menuMotor();
void menuHomeXY();
void menuYCtrl();
void menuCalib();
void menuViewLUT();
void menuOffset();
void menuMotionSpeed();
void menuTest();
void menuBrowser();
void menuPrint();
void menuUtilities();
void menuESP32Control();

// PROTOTYPES FOR MISSING FUNCTIONS
bool listFilesToOLED(const char* dir, const char* ext, String &picked);
void testFireLEDs();
void testOffset();
void testSerialRelay();
void menuFileBrowser();
void zeroXHere();
void measureMatrix();
void loadMatrixFile();
uint8_t  hexCharToVal(char c);

#endif