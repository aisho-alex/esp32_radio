/*
  Pure radio logic, shared by firmware (src/main.cpp) and unit tests (test/).
  No hardware dependencies: compiles on host (native) and on device.
*/

#ifndef RADIO_LOGIC_H
#define RADIO_LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

#define MAX_PRESETS 20
#define FREQ_MIN 8750
#define FREQ_MAX 10800

struct Preset {
  char name[21];
  uint16_t frequency;  // in 100kHz units (e.g., 10150 = 101.5 MHz)
};

// FM band check: 87.5 - 108.0 MHz
inline bool isValidFrequency(uint16_t freq) {
  return freq >= FREQ_MIN && freq <= FREQ_MAX;
}

// Appends a preset with name truncated to 20 chars.
// Returns index of the new preset or -1 if the list is full.
inline int presetAdd(Preset* presets, int& presetCount, const char* name, uint16_t frequency) {
  if (presetCount >= MAX_PRESETS) {
    return -1;
  }
  strncpy(presets[presetCount].name, name ? name : "", 20);
  presets[presetCount].name[20] = '\0';
  presets[presetCount].frequency = frequency;
  return presetCount++;
}

// Removes preset at index, shifting the rest left.
// Returns false for an out-of-range index.
inline bool presetDelete(Preset* presets, int& presetCount, int index) {
  if (index < 0 || index >= presetCount) {
    return false;
  }
  for (int i = index; i < presetCount - 1; i++) {
    presets[i] = presets[i + 1];
  }
  presetCount--;
  return true;
}

// Serializes presets as {"presets":[{"name":..., "frequency":...}, ...]}
// - this is the /api/presets contract the web UI relies on.
inline void presetsToJson(const Preset* presets, int presetCount, JsonDocument& doc) {
  JsonArray arr = doc.createNestedArray("presets");
  for (int i = 0; i < presetCount; i++) {
    JsonObject p = arr.createNestedObject();
    p["name"] = presets[i].name;
    p["frequency"] = presets[i].frequency;
  }
}

// Serializes /api/status: {"frequency","volume","muted","stereo","rssi"}
// - this is the /api/status contract the web UI polls every 2 seconds.
inline void statusToJson(uint16_t frequency, int volume, bool muted, bool stereo, int rssi, JsonDocument& doc) {
  doc["frequency"] = frequency;
  doc["volume"] = volume;
  doc["muted"] = muted;
  doc["stereo"] = stereo;
  doc["rssi"] = rssi;
}

// ---------- LCD line formatting (16x2 / 20x4 character display) ----------

// Line 1: "FM 101.50 MHz ST" - frequency right-aligned in 13 cols, stereo flag right-aligned.
// Total 16 chars (mono pads the ST field with spaces).
inline void formatFreqLine(char* buf, size_t n, uint16_t freq, bool stereo) {
  snprintf(buf, n, "FM %3u.%02u MHz %s",
           (unsigned)(freq / 100), (unsigned)(freq % 100), stereo ? "ST" : "  ");
}

// Line 2: " V08 R24 12:34" - mute flag (M), volume, RSSI, clock HH:MM.
// Muted volume shows as "--", no RTC time shows as "--:--". Total 14 chars.
inline void formatStatusLine(char* buf, size_t n, int volume, bool muted, int rssi,
                             uint8_t hour, uint8_t minute, bool timeValid) {
  char volField[3];
  if (muted) {
    strncpy(volField, "--", sizeof(volField));
  } else {
    snprintf(volField, sizeof(volField), "%02u", (unsigned)volume);
  }
  char timeField[6];
  if (timeValid) {
    snprintf(timeField, sizeof(timeField), "%02u:%02u", (unsigned)hour, (unsigned)minute);
  } else {
    strncpy(timeField, "--:--", sizeof(timeField));
  }
  snprintf(buf, n, "%sV%s R%02u %s", muted ? "M" : " ", volField, (unsigned)rssi, timeField);
}

// Pads buf with trailing spaces up to width (clears leftovers on the LCD).
// Truncates if the string is longer than width or if width exceeds bufSize.
inline void padTo(char* buf, size_t bufSize, size_t width) {
  if (bufSize == 0) {
    return;
  }
  if (width >= bufSize) {
    width = bufSize - 1;
  }
  size_t len = strlen(buf);
  if (len > width) {
    buf[width] = '\0';
    return;
  }
  for (size_t i = len; i < width; i++) {
    buf[i] = ' ';
  }
  buf[width] = '\0';
}

// ---------- Rotary encoder (KY-040) gestures ----------
// Rotation = volume; short press = seek; long press = mute; hold+rotate = frequency.

#define ENCODER_LONG_PRESS_MS 600
#define ENCODER_DETENT_STEPS 10  // in 100kHz units: 10 = 0.1 MHz

enum EncoderPress {
  PRESS_NONE = 0,
  PRESS_SHORT = 1,
  PRESS_LONG = 2
};

// Volume +/- per detent, clamped to 0..15
inline int encoderStepVolume(int volume, int steps) {
  volume += steps;
  if (volume < 0) {
    volume = 0;
  }
  if (volume > 15) {
    volume = 15;
  }
  return volume;
}

// Frequency +/- per detent (0.1 MHz step), clamped to the FM band
inline uint16_t encoderStepFrequency(uint16_t freq, int steps) {
  int32_t value = (int32_t)freq + steps * ENCODER_DETENT_STEPS;
  if (value < FREQ_MIN) {
    value = FREQ_MIN;
  }
  if (value > FREQ_MAX) {
    value = FREQ_MAX;
  }
  return (uint16_t)value;
}

// Classifies a finished/ongoing press by its duration in ms
inline EncoderPress classifyEncoderPress(unsigned long heldMs) {
  if (heldMs >= ENCODER_LONG_PRESS_MS) {
    return PRESS_LONG;
  }
  if (heldMs > 0) {
    return PRESS_SHORT;
  }
  return PRESS_NONE;
}

#endif // RADIO_LOGIC_H
