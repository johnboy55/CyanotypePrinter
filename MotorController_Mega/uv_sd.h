#ifndef UV_SD_H
#define UV_SD_H
#include "uv_globals.h"

extern const char CONFIG_FILE_PATH[];
bool loadConfigFromSD(const char* path);
void applyConfig();
void writeDefaultConfigIfMissing(const char* path);
bool saveConfigToSD(const char* path);
bool loadCalibrationFile(const String &path);

#endif