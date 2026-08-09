#include "uv_sd.h"

const char CONFIG_FILE_PATH[] = "/config/uv.cfg";

char loadedLUTName[24] = "(default)";

static String trimBoth(const String &s) {
  int a = 0, b = (int)s.length();
  while (a < b && isspace((unsigned char)s[a])) ++a;
  while (b > a && isspace((unsigned char)s[b - 1])) --b;
  return s.substring(a, b);
}

static bool parseFloatKey(const String &line, const __FlashStringHelper* key, float &out) {
  String k(key); k += F("=");
  if (!line.startsWith(k)) return false;
  out = trimBoth(line.substring(k.length())).toFloat(); return true;
}

static bool parseIntKey(const String &line, const __FlashStringHelper* key, int &out) {
  String k(key); k += F("=");
  if (!line.startsWith(k)) return false;
  out = trimBoth(line.substring(k.length())).toInt(); return true;
}

static bool parseFloatCSV4(const String &line, const __FlashStringHelper* key, float out[4]) {
  String k(key); k += F("=");
  if (!line.startsWith(k)) return false;
  String rest = line.substring(k.length());
  int idx = 0, start = 0;
  while (idx < 4) {
    int comma = rest.indexOf(',', start);
    String tok = (comma < 0) ? rest.substring(start) : rest.substring(start, comma);
    tok.trim();
    if (!tok.length()) break;
    out[idx++] = tok.toFloat();
    if (comma < 0) break;
    start = comma + 1;
  }
  return (idx == 4);
}

bool loadConfigFromSD(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  String line, low;
  // Tracks which keys this file actually contained, so keys added to the format after a
  // given SD card's config was first written (like the offsets below) get backfilled into
  // the file rather than silently defaulting every boot with no record of it on disk.
  bool sawOffsetX = false, sawOffsetY = false;
  while (f.available()) {
    line = f.readStringUntil('\n'); line.replace(F("\r"), F(""));
    line = trimBoth(line);
    if (!line.length()) continue;
    low = line;
    low.toLowerCase();
    if (low.startsWith(F("#")) || low.startsWith(F(";"))) continue;

    // parseXxxKey() itself checks the "key=" prefix, so there's no need for a separate
    // startsWith() gate duplicating each key string before it -- one copy of each name,
    // in flash via F(), instead of two.
    if (parseFloatKey(low, F("y_band_mm"), CFG.yBandAdvanceMM)) continue;
    if (parseFloatKey(low, F("mm_per_pixel"), CFG.mmPerPixel)) continue;
    if (parseFloatKey(low, F("led_spacing_mm"), CFG.ledSpacingMM)) continue;
    if (parseIntKey  (low, F("step_pulse_us"), CFG.stepPulseUs)) continue;
    if (parseFloatKey(low, F("offset_x_mm"), CFG.offsetXmm)) { sawOffsetX = true; continue; }
    if (parseFloatKey(low, F("offset_y_mm"), CFG.offsetYmm)) { sawOffsetY = true; continue; }
    if (parseFloatKey(low, F("max_speed_x_mm"), CFG.maxSpeedXmm)) continue;
    if (parseFloatKey(low, F("accel_x_mm"), CFG.accelXmm)) continue;
    if (parseFloatKey(low, F("max_speed_y_mm"), CFG.maxSpeedYmm)) continue;
    if (parseFloatKey(low, F("accel_y_mm"), CFG.accelYmm)) continue;

    if (parseFloatCSV4(low, F("led_scales"), CFG.ledScale)) continue;
    if (parseFloatKey(low, F("led1_scale"), CFG.ledScale[0])) continue;
    if (parseFloatKey(low, F("led2_scale"), CFG.ledScale[1])) continue;
    if (parseFloatKey(low, F("led3_scale"), CFG.ledScale[2])) continue;
    if (parseFloatKey(low, F("led4_scale"), CFG.ledScale[3])) continue;

    if (low.startsWith(F("invert_image="))) {
      String k(F("invert_image=")); String v = trimBoth(low.substring(k.length())); v.toLowerCase();
      CFG.invertImage = (v==F("1") || v==F("true") || v==F("yes") || v==F("on")); continue;
    }
  }
  f.close();

  if (!sawOffsetX || !sawOffsetY) saveConfigToSD(path); // Backfill: write the full config
  // (offsets included, at whatever default/loaded value they now hold) so this file has
  // them explicitly from here on.
  return true;
}

void applyConfig() {
  MM_PER_PIXEL   = CFG.mmPerPixel;
  LED_SPACING_MM = CFG.ledSpacingMM;
  STEP_PULSE_US  = CFG.stepPulseUs;
  for (int i = 0; i < 4; i++) LED_SCALE[i] = CFG.ledScale[i];

  STEPS_PER_PIXEL = max(1, (int)(STEPS_PER_MM_X * MM_PER_PIXEL + 0.5f));
  OFFSET_PIXELS   = max(1, (int)(LED_SPACING_MM / MM_PER_PIXEL + 0.5f));

  applyMotionConfig();
}

void writeDefaultConfigIfMissing(const char* path) {
  if (SD.exists(path)) return;
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.println(F("[CFG] Could not create default config"));
    return;
  }
  f.println(F("# UV Printer config (key=value). Comments start with # or ;"));
  f.println(F("y_band_mm=8.2"));
  f.println(F("mm_per_pixel=2.0"));
  f.println(F("led_spacing_mm=7.0"));
  f.println(F("step_pulse_us=5"));
  f.println(F("led_scales=1.000,1.000,1.000,1.000"));
  f.println(F("invert_image=1"));
  f.println(F("offset_x_mm=0.0"));
  f.println(F("offset_y_mm=0.0"));
  f.println(F("max_speed_x_mm=10.0"));
  f.println(F("accel_x_mm=20.0"));
  f.println(F("max_speed_y_mm=10.0"));
  f.println(F("accel_y_mm=20.0"));
  f.close();
}

bool saveConfigToSD(const char* path) {
  CFG.mmPerPixel     = MM_PER_PIXEL;
  CFG.ledSpacingMM   = LED_SPACING_MM;
  CFG.stepPulseUs    = STEP_PULSE_US;
  for (int i = 0; i < 4; i++) CFG.ledScale[i] = LED_SCALE[i];

  // FILE_WRITE opens for append, not overwrite -- without removing the old file first,
  // every save would pile another full copy of these keys onto the end rather than
  // replacing the previous values (which the "last one wins" parsing above would then
  // read back correctly, but the file would grow forever and get harder to hand-edit).
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  f.println(F("# UV Printer config (generated by device)"));
  f.print(F("y_band_mm="));      f.println(String(CFG.yBandAdvanceMM, 3));
  f.print(F("mm_per_pixel="));   f.println(String(CFG.mmPerPixel, 4));
  f.print(F("led_spacing_mm=")); f.println(String(CFG.ledSpacingMM, 3));
  f.print(F("step_pulse_us=")); f.println(CFG.stepPulseUs);
  f.print(F("offset_x_mm="));    f.println(String(CFG.offsetXmm, 3));
  f.print(F("offset_y_mm="));    f.println(String(CFG.offsetYmm, 3));
  f.print(F("max_speed_x_mm=")); f.println(String(CFG.maxSpeedXmm, 2));
  f.print(F("accel_x_mm="));     f.println(String(CFG.accelXmm, 2));
  f.print(F("max_speed_y_mm=")); f.println(String(CFG.maxSpeedYmm, 2));
  f.print(F("accel_y_mm="));     f.println(String(CFG.accelYmm, 2));
  f.print(F("led_scales="));
  f.print(String(LED_SCALE[0],3)); f.print(F(","));
  f.print(String(LED_SCALE[1],3)); f.print(F(","));
  f.print(String(LED_SCALE[2],3)); f.print(F(","));
  f.println(String(LED_SCALE[3],3));
  f.print(F("invert_image=")); f.println(CFG.invertImage ? F("1") : F("0"));
  f.close();
  return true;
}

bool loadCalibrationFile(const String &path) {
  File f = SD.open(path);
  if (!f) return false;
  String all;
  while (f.available()) all += (char)f.read();
  f.close();

  int n = 0, start = 0;
  while (n < 16) {
    int comma = all.indexOf(',', start);
    String tok = (comma < 0) ? all.substring(start) : all.substring(start, comma);
    tok.trim();
    if (tok.length() == 0) break;
    LUT16[n++] = tok.toFloat();
    if (comma < 0) break;
    start = comma + 1;
  }
  if (n == 16) {
    // Record where the active LUT came from (basename only -- the OLED is 21 chars wide).
    int slash = path.lastIndexOf('/');
    String base = (slash >= 0) ? path.substring(slash + 1) : path;
    base.toCharArray(loadedLUTName, sizeof(loadedLUTName));
  }
  return (n == 16);
}