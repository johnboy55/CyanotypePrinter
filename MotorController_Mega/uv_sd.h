#ifndef UV_SD_H
#define UV_SD_H
#include "uv_globals.h"

extern const char CONFIG_FILE_PATH[];
// Basename of the last successfully loaded /calibrations LUT file ("(default)" until
// one loads) -- shown by Calib > View LUT so the active LUT is a glance, not a guess.
extern char loadedLUTName[24];
bool loadConfigFromSD(const char* path);
void applyConfig();
void writeDefaultConfigIfMissing(const char* path);
bool saveConfigToSD(const char* path);
bool loadCalibrationFile(const String &path);

#endif