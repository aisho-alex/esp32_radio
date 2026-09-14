/*
  FM Radio Controller
  Hardware: ESP32 / ESP32-C3 / ESP8266 (NodeMCU) + RDA5807M FM Module
  Wiring (VCC = 3.3V, GND = GND):
    Board              SDA          SCL
    ESP32 DevKit       GPIO21       GPIO22
    ESP32-C3 DevKit    GPIO8        GPIO9
    ESP8266 NodeMCU    D1 (GPIO5)   D2 (GPIO4)
    All peripherals share the same I2C bus:
    - RDA5807M FM module (addr 0x10/0x11)
    - Character LCD 1602/2004 on PCF8574 backpack (addr 0x27 or 0x3F), optional
    - RTC DS3231 (addr 0x68) or DS1307 (-DUSE_DS1307), optional
    Rotary encoder KY-040 (A/B/SW, interrupt pins, GPIO set per board below):
    - Rotation = volume +/-, hold+rotate = frequency +/- 0.1 MHz
    - Short press = seek next station, long press = mute toggle

  Build via PlatformIO (see platformio.ini):
    pio run -e esp32dev | esp32-c3 | nodemcuv2
  Display options (build_flags):
    -DLCD_COLS=16 -DLCD_ROWS=2   LCD geometry (default 16x2, try 20/4)
    -DLCD_I2C_ADDR=0x27          pin backpack address (default: auto-detect)
    -DUSE_DS1307                 use DS1307 instead of DS3231

  Required libraries:
    - RDA5807 by Ricardo Lima Caratti (pu2clr)
    - ArduinoJson v6 by Benoit Blanchon
    - LiquidCrystal_I2C (marcoschwartz)
    - RTClib (Adafruit)
    - WiFi / WebServer / HTTPUpdateServer / EEPROM / mDNS (core)
*/

#include <Wire.h>
#include <RDA5807.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include "radio_logic.h"

#if defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <HTTPUpdate.h>
#include <ESPmDNS.h>

WebServer server(80);
HTTPUpdateServer httpUpdater;
#else
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266mDNS.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
#endif

// ============ CONFIGURATION ============
// WiFi mode: true = AP mode, false = STA mode (connect to existing network)
#define WIFI_AP_MODE true

// STA mode credentials (ignored if AP mode)
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// AP mode settings
#define AP_SSID "FM-Radio"
#define AP_PASSWORD "12345678"  // min 8 chars, or empty for open network

// RDA5807M I2C pins (set per board via build_flags in platformio.ini)
#ifndef I2C_SDA
#define I2C_SDA 21  // ESP32 default
#endif
#ifndef I2C_SCL
#define I2C_SCL 22  // ESP32 default
#endif

// mDNS hostname: http://fm-radio.local
#define MDNS_HOSTNAME "fm-radio"

// Character LCD (PCF8574 I2C backpack): 0 = auto-detect address (0x27 / 0x3F)
#ifndef LCD_I2C_ADDR
#define LCD_I2C_ADDR 0
#endif
#ifndef LCD_COLS
#define LCD_COLS 16
#endif
#ifndef LCD_ROWS
#define LCD_ROWS 2
#endif

// Rotary encoder (KY-040) pins, set per board via build_flags in platformio.ini
#ifndef ENC_A
#define ENC_A 32
#endif
#ifndef ENC_B
#define ENC_B 33
#endif
#ifndef ENC_SW
#define ENC_SW 25
#endif

// EEPROM settings
#define EEPROM_SIZE 512
#define EEPROM_PRESETS_ADDR 64
#define PRESET_MAGIC 0x5244  // "RD" signature

// Firmware version
#define FIRMWARE_VERSION "1.0"

// ============ GLOBALS ============
RDA5807 rx;

#if defined(USE_DS1307)
RTC_DS1307 rtc;
#else
RTC_DS3231 rtc;
#endif

LiquidCrystal_I2C* lcd = NULL;
bool lcdAvailable = false;
bool rtcAvailable = false;
char lcdLine1[LCD_COLS + 1] = "";
char lcdLine2[LCD_COLS + 1] = "";
unsigned long lastDisplayUpdate = 0;

Preset presets[MAX_PRESETS];
int presetCount = 0;
bool isMuted = false;
int currentVolume = 8;

// Checks I2C presence of the RDA5807M (full access address 0x10)
bool radioConnected() {
  Wire.beginTransmission(I2C_ADDR_FULL_ACCESS);
  return (Wire.endTransmission() == 0);
}

// ============ DISPLAY & RTC ============
bool i2cPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void initDisplay() {
  uint8_t lcdAddr = LCD_I2C_ADDR;
#if LCD_I2C_ADDR == 0
  const uint8_t scanAddrs[] = {0x27, 0x3F};
  for (uint8_t i = 0; i < sizeof(scanAddrs); i++) {
    if (i2cPresent(scanAddrs[i])) {
      lcdAddr = scanAddrs[i];
      break;
    }
  }
#endif
  if (lcdAddr == 0) {
    return;
  }
  lcd = new LiquidCrystal_I2C(lcdAddr, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  lcdAvailable = true;
}

void initRtc() {
  rtcAvailable = rtc.begin();
}

void updateDisplay() {
  if (!lcdAvailable) {
    return;
  }

  char line1[LCD_COLS + 1];
  char line2[LCD_COLS + 1];
  formatFreqLine(line1, sizeof(line1), rx.getFrequency(), rx.isStereo());

  uint8_t hour = 0, minute = 0;
  bool timeValid = false;
  if (rtcAvailable) {
    DateTime now = rtc.now();
    if (now.isValid()) {
      hour = now.hour();
      minute = now.minute();
      timeValid = true;
    }
  }
  formatStatusLine(line2, sizeof(line2), currentVolume, isMuted, rx.getRssi(), hour, minute, timeValid);

  padTo(line1, sizeof(line1), LCD_COLS);
  padTo(line2, sizeof(line2), LCD_COLS);

  // Rewrite only the lines that actually changed (avoids flicker and saves I2C traffic)
  if (strcmp(line1, lcdLine1) != 0) {
    strncpy(lcdLine1, line1, LCD_COLS);
    lcdLine1[LCD_COLS] = '\0';
    lcd->setCursor(0, 0);
    lcd->print(lcdLine1);
  }
  if (strcmp(line2, lcdLine2) != 0) {
    strncpy(lcdLine2, line2, LCD_COLS);
    lcdLine2[LCD_COLS] = '\0';
    lcd->setCursor(0, 1);
    lcd->print(lcdLine2);
  }
}

// ============ EEPROM ============
void loadPresets() {
  uint16_t magic;
  EEPROM.get(EEPROM_PRESETS_ADDR, magic);
  if (magic != PRESET_MAGIC) {
    presetCount = 0;
    return;
  }
  EEPROM.get(EEPROM_PRESETS_ADDR + 2, presetCount);
  if (presetCount < 0 || presetCount > MAX_PRESETS) {
    presetCount = 0;
    return;
  }
  for (int i = 0; i < presetCount; i++) {
    EEPROM.get(EEPROM_PRESETS_ADDR + 4 + i * sizeof(Preset), presets[i]);
  }
}

void savePresets() {
  EEPROM.put(EEPROM_PRESETS_ADDR, PRESET_MAGIC);
  EEPROM.put(EEPROM_PRESETS_ADDR + 2, presetCount);
  for (int i = 0; i < presetCount; i++) {
    EEPROM.put(EEPROM_PRESETS_ADDR + 4 + i * sizeof(Preset), presets[i]);
  }
  EEPROM.commit();
}

// ============ CORS ============
void setCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  setCORS();
  server.send(200, "text/plain", "");
}

// ============ API HANDLERS ============
void handleStatus() {
  setCORS();
  StaticJsonDocument<256> doc;
  statusToJson(rx.getFrequency(), currentVolume, isMuted, rx.isStereo(), rx.getRssi(), doc);

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleTune() {
  setCORS();
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  uint16_t freq = doc["frequency"] | 0;
  if (isValidFrequency(freq)) {
    rx.setFrequency(freq);
    delay(50);
  }

  StaticJsonDocument<128> resp;
  resp["frequency"] = rx.getFrequency();
  resp["rssi"] = rx.getRssi();
  resp["stereo"] = rx.isStereo();

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

// Blocking seek; shared by /api/seek and the encoder short press
void seekStation(bool seekUp) {
  rx.setSeekThreshold(8);
  // rx.seek() is blocking: it returns after the seek completes (STC flag)
  rx.seek(RDA_SEEK_WRAP, seekUp ? RDA_SEEK_UP : RDA_SEEK_DOWN);
}

void handleSeek() {
  setCORS();
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  String direction = "up";
  if (!error && doc["direction"]) {
    direction = doc["direction"].as<String>();
  }

  seekStation(direction == "up");

  StaticJsonDocument<128> resp;
  resp["frequency"] = rx.getFrequency();
  resp["rssi"] = rx.getRssi();
  resp["stereo"] = rx.isStereo();

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

void handleVolume() {
  setCORS();
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  int vol = doc["volume"] | -1;
  if (vol >= 0 && vol <= 15) {
    currentVolume = vol;
    rx.setVolume(vol);
  }

  StaticJsonDocument<64> resp;
  resp["volume"] = currentVolume;

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

void handleMute() {
  setCORS();
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  bool mute = !isMuted;
  if (!error && doc.containsKey("muted")) {
    mute = doc["muted"];
  }

  isMuted = mute;
  rx.setMute(isMuted);

  StaticJsonDocument<64> resp;
  resp["muted"] = isMuted;

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

void handleScan() {
  setCORS();
  StaticJsonDocument<1024> doc;
  JsonArray stations = doc.createNestedArray("stations");

  uint16_t originalFreq = rx.getFrequency();

  rx.setSeekThreshold(8);
  rx.setFrequency(8750);
  delay(100);

  for (int i = 0; i < 25; i++) {
    // rx.seek() is blocking: it returns after the seek completes (STC flag)
    rx.seek(RDA_SEEK_WRAP, RDA_SEEK_UP);

    uint16_t freq = rx.getFrequency();
    uint8_t rssi = rx.getRssi();

    if (rssi >= 12) {
      JsonObject station = stations.createNestedObject();
      station["frequency"] = freq;
      station["rssi"] = rssi;
      station["stereo"] = rx.isStereo();
    }

    // Prevent infinite loop
    if (freq >= 10750) break;
    delay(100);
  }

  // Restore original frequency
  rx.setFrequency(originalFreq);

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetPresets() {
  setCORS();
  StaticJsonDocument<1024> doc;
  presetsToJson(presets, presetCount, doc);

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleAddPreset() {
  setCORS();
  if (presetCount >= MAX_PRESETS) {
    server.send(400, "application/json", "{\"error\":\"Max presets reached\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* name = doc["name"] | "Station";
  uint16_t freq = doc["frequency"] | rx.getFrequency();

  int index = presetAdd(presets, presetCount, name, freq);
  if (index < 0) {
    server.send(400, "application/json", "{\"error\":\"Max presets reached\"}");
    return;
  }
  savePresets();

  StaticJsonDocument<128> resp;
  resp["success"] = true;
  resp["index"] = index;

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

void handleDeletePreset() {
  setCORS();
  String indexStr = server.pathArg(0);
  int index = indexStr.toInt();

  if (!presetDelete(presets, presetCount, index)) {
    server.send(400, "application/json", "{\"error\":\"Invalid index\"}");
    return;
  }
  savePresets();

  StaticJsonDocument<64> resp;
  resp["success"] = true;

  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

// ============ TIME (RTC) ============
void handleGetTime() {
  setCORS();
  StaticJsonDocument<192> doc;
  if (!rtcAvailable) {
    doc["available"] = false;
  } else {
    DateTime now = rtc.now();
    doc["available"] = true;
    doc["epoch"] = (uint32_t)now.unixtime();
    char dt[25];
    snprintf(dt, sizeof(dt), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)now.year(), (unsigned)now.month(), (unsigned)now.day(),
             (unsigned)now.hour(), (unsigned)now.minute(), (unsigned)now.second());
    doc["datetime"] = dt;
  }
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetTime() {
  setCORS();
  if (!rtcAvailable) {
    server.send(503, "application/json", "{\"error\":\"RTC not available\"}");
    return;
  }
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc.containsKey("epoch")) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON, need epoch field\"}");
    return;
  }

  uint32_t epoch = doc["epoch"] | 0u;
  if (epoch < 1000000000u || epoch > 4102444800u) {  // sane range: 2001..2100
    server.send(400, "application/json", "{\"error\":\"Epoch out of range\"}");
    return;
  }

  rtc.adjust(DateTime(epoch));
  StaticJsonDocument<64> resp;
  resp["success"] = true;
  String response;
  serializeJson(resp, response);
  server.send(200, "application/json", response);
}

// ============ ROTARY ENCODER ============
// Rotation = volume; hold+rotate = frequency; short press = seek; long press = mute.

volatile int32_t encoderDelta = 0;

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// Quadrature decoder. Transition table index = last AB state << 2 | current AB.
// One KY-040 detent generates a full 4-transition cycle => delta changes by +/-4.
void IRAM_ATTR encoderIsr() {
  static uint8_t lastAb = 3;  // idle high with pullups
  static const int8_t transition[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
  uint8_t ab = ((uint8_t)digitalRead(ENC_B) << 1) | (uint8_t)digitalRead(ENC_A);
  encoderDelta += transition[(lastAb << 2) | ab];
  lastAb = ab;
}

// Consumes whole detents, keeps the sub-detent remainder for the next pass
int32_t readEncoderDetents() {
  noInterrupts();
  int32_t detents = encoderDelta / 4;
  encoderDelta -= detents * 4;
  interrupts();
  return detents;
}

// Button/gesture state (polled in loop)
bool encSwPressed = false;
unsigned long encSwPressStart = 0;
unsigned long encSwLastEdgeMs = 0;
bool encRotatedWhileHeld = false;
bool encLongFired = false;

void handleEncoder() {
  int32_t detents = readEncoderDetents();
  bool pressed = (digitalRead(ENC_SW) == LOW);
  unsigned long now = millis();

  if (pressed != encSwPressed && (now - encSwLastEdgeMs) >= 30) {  // debounce
    encSwLastEdgeMs = now;
    encSwPressed = pressed;
    if (pressed) {
      encSwPressStart = now;
      encRotatedWhileHeld = false;
      encLongFired = false;
    } else if (!encRotatedWhileHeld && !encLongFired) {
      if (classifyEncoderPress(now - encSwPressStart) == PRESS_SHORT) {
        seekStation(true);
        updateDisplay();
      }
    }
  }

  if (detents != 0) {
    if (encSwPressed) {
      encRotatedWhileHeld = true;
      rx.setFrequency(encoderStepFrequency(rx.getFrequency(), (int)detents));
    } else {
      currentVolume = encoderStepVolume(currentVolume, (int)detents);
      rx.setVolume(currentVolume);
    }
    updateDisplay();
  }

  if (encSwPressed && !encRotatedWhileHeld && !encLongFired &&
      (now - encSwPressStart) >= ENCODER_LONG_PRESS_MS) {
    encLongFired = true;
    isMuted = !isMuted;
    rx.setMute(isMuted);
    updateDisplay();
  }
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    handleOptions();
    return;
  }
  setCORS();
  server.send(404, "application/json", "{\"error\":\"Not found\"}");
}
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>ESP8266 FM Radio</title>
<style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0f1117;--bg-card:#1a1d27;--bg-display:#0a0c10;--accent:#f59e0b;--accent-glow:rgba(245,158,11,0.3);--text:#e2e8f0;--text-dim:#64748b;--border:#2d3142;--success:#10b981;--danger:#ef4444;--stereo:#06b6d4}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:20px 16px}
.header{text-align:center;margin-bottom:20px}
.header h1{font-size:1.4rem;font-weight:600;color:var(--accent);letter-spacing:2px;text-transform:uppercase}
.header .subtitle{font-size:.75rem;color:var(--text-dim);margin-top:4px}
.display{background:var(--bg-display);border:2px solid var(--border);border-radius:16px;padding:24px 32px;width:100%;max-width:420px;text-align:center;position:relative;box-shadow:inset 0 2px 10px rgba(0,0,0,0.5),0 0 30px var(--accent-glow)}
.frequency-display{font-size:4rem;font-weight:700;color:var(--accent);font-variant-numeric:tabular-nums;text-shadow:0 0 20px var(--accent-glow),0 0 40px var(--accent-glow);line-height:1;letter-spacing:2px}
.frequency-display .mhz{font-size:1.4rem;color:var(--text-dim);margin-left:4px;text-shadow:none}
.station-name{font-size:1.1rem;color:var(--text);margin-top:8px;min-height:28px;font-weight:500}
.indicators{display:flex;justify-content:center;gap:16px;margin-top:12px}
.indicator{display:flex;align-items:center;gap:6px;font-size:.75rem;color:var(--text-dim);padding:4px 10px;border-radius:20px;background:rgba(255,255,255,0.03);transition:all .3s}
.indicator.active{background:rgba(6,182,212,0.15);color:var(--stereo)}
.indicator.active.stereo{box-shadow:0 0 10px rgba(6,182,212,0.3)}
.indicator-dot{width:8px;height:8px;border-radius:50%;background:var(--text-dim);transition:all .3s}
.indicator.active .indicator-dot{background:var(--stereo);box-shadow:0 0 6px var(--stereo)}
.signal-bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.signal-bar{width:3px;background:var(--text-dim);border-radius:1px;transition:all .3s}
.signal-bar.active{background:var(--success);box-shadow:0 0 4px rgba(16,185,129,0.5)}
.controls{width:100%;max-width:420px;margin-top:20px;display:flex;flex-direction:column;gap:16px}
.card{background:var(--bg-card);border:1px solid var(--border);border-radius:14px;padding:16px}
.card-title{font-size:.8rem;color:var(--text-dim);text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;display:flex;align-items:center;gap:8px}
.tuning-row{display:flex;align-items:center;gap:12px}
.tune-btn{width:48px;height:48px;border-radius:14px;border:1px solid var(--border);background:var(--bg);color:var(--text);font-size:1.5rem;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:all .2s;flex-shrink:0;user-select:none;-webkit-tap-highlight-color:transparent}
.tune-btn:hover{background:var(--accent);color:var(--bg);border-color:var(--accent)}
.tune-btn:active{transform:scale(0.92)}
.slider-container{flex:1;position:relative}
.slider-labels{display:flex;justify-content:space-between;font-size:.65rem;color:var(--text-dim);margin-bottom:4px}
input[type="range"]{width:100%;height:8px;-webkit-appearance:none;appearance:none;background:var(--bg);border-radius:4px;outline:none;cursor:pointer}
input[type="range"]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:var(--accent);cursor:pointer;box-shadow:0 0 10px var(--accent-glow);border:2px solid var(--bg)}
input[type="range"]::-moz-range-thumb{width:24px;height:24px;border-radius:50%;background:var(--accent);cursor:pointer;box-shadow:0 0 10px var(--accent-glow);border:2px solid var(--bg)}
.btn-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.btn{padding:12px;border-radius:12px;border:1px solid var(--border);background:var(--bg);color:var(--text);font-size:.85rem;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:6px;transition:all .2s;font-weight:500;user-select:none;-webkit-tap-highlight-color:transparent}
.btn:hover{background:rgba(245,158,11,0.1);border-color:var(--accent)}
.btn:active{transform:scale(0.95)}
.btn.active{background:var(--accent);color:var(--bg);border-color:var(--accent)}
.btn svg{width:18px;height:18px}
.btn-success:hover{background:rgba(16,185,129,0.1);border-color:var(--success)}
.btn-danger:hover{background:rgba(239,68,68,0.1);border-color:var(--danger)}
.volume-row{display:flex;align-items:center;gap:12px}
.volume-value{font-size:1.2rem;font-weight:600;color:var(--accent);min-width:36px;text-align:center}
.presets-list{display:flex;flex-direction:column;gap:8px;max-height:200px;overflow-y:auto;padding-right:4px}
.presets-list::-webkit-scrollbar{width:4px}
.presets-list::-webkit-scrollbar-track{background:var(--bg);border-radius:4px}
.presets-list::-webkit-scrollbar-thumb{background:var(--border);border-radius:4px}
.preset-item{display:flex;align-items:center;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:10px;cursor:pointer;transition:all .2s;gap:10px}
.preset-item:hover{border-color:var(--accent);background:rgba(245,158,11,0.05)}
.preset-item.active{border-color:var(--accent);background:rgba(245,158,11,0.1)}
.preset-num{width:28px;height:28px;border-radius:8px;background:var(--bg-card);display:flex;align-items:center;justify-content:center;font-size:.75rem;font-weight:600;color:var(--accent);flex-shrink:0}
.preset-info{flex:1;min-width:0}
.preset-name{font-size:.9rem;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.preset-freq{font-size:.75rem;color:var(--text-dim)}
.preset-delete{width:28px;height:28px;border-radius:8px;border:none;background:transparent;color:var(--text-dim);cursor:pointer;display:flex;align-items:center;justify-content:center;transition:all .2s;flex-shrink:0}
.preset-delete:hover{background:rgba(239,68,68,0.15);color:var(--danger)}
.empty-presets{text-align:center;padding:24px;color:var(--text-dim);font-size:.85rem}
.scan-list{display:flex;flex-wrap:wrap;gap:6px;max-height:150px;overflow-y:auto;padding-right:4px}
.scan-item{padding:8px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;cursor:pointer;font-size:.8rem;font-weight:500;transition:all .2s;display:flex;align-items:center;gap:6px}
.scan-item:hover{border-color:var(--success);background:rgba(16,185,129,0.1)}
.scan-item .scan-freq{color:var(--accent)}
.scan-item .scan-rssi{font-size:.7rem;color:var(--text-dim)}
.toast-container{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);z-index:1000;display:flex;flex-direction:column;gap:8px;align-items:center}
.toast{padding:10px 20px;border-radius:10px;font-size:.85rem;font-weight:500;animation:toastIn .3s ease,toastOut .3s ease 2.7s;pointer-events:none}
.toast.success{background:rgba(16,185,129,0.9);color:#fff}
.toast.error{background:rgba(239,68,68,0.9);color:#fff}
.toast.info{background:rgba(245,158,11,0.9);color:var(--bg)}
@keyframes toastIn{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:translateY(0)}}
@keyframes toastOut{from{opacity:1;transform:translateY(0)}to{opacity:0;transform:translateY(10px)}}
.modal-overlay{position:fixed;inset:0;background:rgba(0,0,0,0.7);display:flex;align-items:center;justify-content:center;z-index:2000;opacity:0;pointer-events:none;transition:opacity .2s}
.modal-overlay.show{opacity:1;pointer-events:auto}
.modal{background:var(--bg-card);border:1px solid var(--border);border-radius:16px;padding:24px;width:90%;max-width:320px;transform:scale(0.9);transition:transform .2s}
.modal-overlay.show .modal{transform:scale(1)}
.modal h3{margin-bottom:16px;font-size:1.1rem}
.modal input{width:100%;padding:12px 14px;border-radius:10px;border:1px solid var(--border);background:var(--bg);color:var(--text);font-size:1rem;outline:none;margin-bottom:16px}
.modal input:focus{border-color:var(--accent)}
.modal-actions{display:flex;gap:10px}
.modal-actions .btn{flex:1}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.scanning{animation:pulse 1s infinite}
.footer{margin-top:auto;padding-top:20px;text-align:center;font-size:.7rem;color:var(--text-dim)}
@media(max-width:400px){.frequency-display{font-size:3rem}.btn-grid{grid-template-columns:repeat(2,1fr)}}
</style>
</head>
<body>
<div class="header"><h1>FM Radio</h1><div class="subtitle">ESP8266 + RDA5807M</div></div>
<div class="display">
<div class="frequency-display"><span id="freqValue">87.5</span><span class="mhz">MHz</span></div>
<div class="station-name" id="stationName">Станция не выбрана</div>
<div class="indicators">
<div class="indicator" id="stereoIndicator"><div class="indicator-dot"></div><span>STEREO</span></div>
<div class="indicator" id="rssiIndicator">
<div class="signal-bars" id="signalBars"><div class="signal-bar" style="height:4px"></div><div class="signal-bar" style="height:7px"></div><div class="signal-bar" style="height:10px"></div><div class="signal-bar" style="height:13px"></div></div>
<span id="rssiValue">0 dBuV</span>
</div>
</div>
</div>
<div class="controls">
<div class="card">
<div class="card-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><circle cx="12" cy="12" r="3"/><path d="M12 1v4m0 14v4m-7-9H1m22 0h-4m-2.5-6.5L17 5m-10 10l-2.5 2.5M17 17l2.5 2.5M7 7L4.5 4.5"/></svg>Настройка</div>
<div class="tuning-row">
<button class="tune-btn" onclick="fineTune(-0.1)" title="-0.1 MHz">−</button>
<div class="slider-container">
<div class="slider-labels"><span>87.5</span><span>108.0</span></div>
<input type="range" id="freqSlider" min="8750" max="10800" value="8750" oninput="onSliderChange(this.value)">
</div>
<button class="tune-btn" onclick="fineTune(0.1)" title="+0.1 MHz">+</button>
</div>
</div>
<div class="card">
<div class="card-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><polygon points="5 3 19 12 5 21 5 3"/></svg>Управление</div>
<div class="btn-grid">
<button class="btn" onclick="seek(-1)"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><path d="M19 12H5m7 7l-7-7 7-7"/></svg>Назад</button>
<button class="btn" onclick="seek(1)">Вперёд<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><path d="M5 12h14m-7-7l7 7-7 7"/></svg></button>
<button class="btn btn-success" id="scanBtn" onclick="startScan()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.35-4.35"/></svg>Поиск</button>
<button class="btn" id="muteBtn" onclick="toggleMute()"><svg id="muteIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 010 7.07"/></svg>Звук</button>
<button class="btn btn-success" onclick="savePreset()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><path d="M19 21H5a2 2 0 01-2-2V5a2 2 0 012-2h11l5 5v11a2 2 0 01-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/></svg>Сохранить</button>
<button class="btn btn-danger" onclick="clearAllPresets()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 01-2 2H7a2 2 0 01-2-2V6m3 0V4a2 2 0 012-2h4a2 2 0 012 2v2"/></svg>Очистить</button>
</div>
</div>
<div class="card">
<div class="card-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 010 7.07"/></svg>Громкость</div>
<div class="volume-row">
<button class="tune-btn" onclick="changeVolume(-1)" style="width:36px;height:36px;font-size:1.2rem">−</button>
<input type="range" id="volumeSlider" min="0" max="15" value="8" oninput="setVolume(this.value)" style="flex:1">
<button class="tune-btn" onclick="changeVolume(1)" style="width:36px;height:36px;font-size:1.2rem">+</button>
<div class="volume-value" id="volumeValue">8</div>
</div>
</div>
<div class="card">
<div class="card-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><path d="M19 21l-7-5-7 5V5a2 2 0 012-2h10a2 2 0 012 2z"/></svg>Пресеты</div>
<div class="presets-list" id="presetsList"><div class="empty-presets">Нет сохранённых станций</div></div>
</div>
<div class="card" id="scanCard" style="display:none">
<div class="card-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.35-4.35"/></svg>Найденные станции</div>
<div class="scan-list" id="scanList"></div>
</div>
</div>
<div class="footer">ESP8266 FM Radio Controller v1.0</div>
<div class="toast-container" id="toastContainer"></div>
<div class="modal-overlay" id="modalOverlay">
<div class="modal">
<h3>Сохранить станцию</h3>
<input type="text" id="presetNameInput" placeholder="Название станции" maxlength="20">
<div class="modal-actions">
<button class="btn" onclick="closeModal()">Отмена</button>
<button class="btn active" onclick="confirmSavePreset()">Сохранить</button>
</div>
</div>
</div>
<script>
const API='';
let state={frequency:8750,volume:8,muted:false,stereo:false,rssi:0,presets:[],scanning:false};
document.addEventListener('DOMContentLoaded',()=>{loadState();updateUI();pollStatus();setInterval(pollStatus,2000);loadServerPresets()});
async function apiPost(e,t){try{const n=await fetch(API+e,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(t)});if(!n.ok)throw new Error('HTTP '+n.status);return await n.json()}catch(e){showToast('Ошибка: '+e.message,'error');throw e}}
async function apiGet(e){try{const t=await fetch(API+e);if(!t.ok)throw new Error('HTTP '+t.status);return await t.json()}catch(e){showToast('Ошибка: '+e.message,'error');throw e}}
async function pollStatus(){try{const e=await apiGet('/api/status');state.frequency=e.frequency||state.frequency;state.volume=e.volume!==undefined?e.volume:state.volume;state.muted=e.muted||false;state.stereo=e.stereo||false;state.rssi=e.rssi||0;updateUI()}catch(e){console.log('Poll error:',e)}}
function updateUI(){const e=(state.frequency/100).toFixed(1);document.getElementById('freqValue').textContent=e;document.getElementById('freqSlider').value=state.frequency;document.getElementById('volumeSlider').value=state.volume;document.getElementById('volumeValue').textContent=state.volume;document.getElementById('stereoIndicator').classList.toggle('active',state.stereo);document.getElementById('stereoIndicator').classList.toggle('stereo',state.stereo);document.getElementById('rssiValue').textContent=(state.rssi||0)+' dBuV';const t=document.querySelectorAll('.signal-bar'),n=Math.min(4,Math.max(0,Math.floor((state.rssi||0)/15)));t.forEach((e,t)=>e.classList.toggle('active',t<n));document.getElementById('muteBtn').classList.toggle('active',state.muted);renderPresets()}
function renderPresets(){const e=document.getElementById('presetsList');if(!state.presets||0===state.presets.length)return void(e.innerHTML='<div class="empty-presets">Нет сохранённых станций</div>');e.innerHTML=state.presets.map((e,t)=>{const n=Math.abs(e.frequency-state.frequency)<5;return`<div class="preset-item ${n?'active':''}" onclick="loadPreset(${t})"><div class="preset-num">${t+1}</div><div class="preset-info"><div class="preset-name">${escapeHtml(e.name)}</div><div class="preset-freq">${(e.frequency/100).toFixed(1)} MHz</div></div><button class="preset-delete" onclick="event.stopPropagation();deletePreset(${t})"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg></button></div>`}).join('')}
async function onSliderChange(e){state.frequency=parseInt(e);updateUI();try{await apiPost('/api/tune',{frequency:state.frequency})}catch(e){}}
async function fineTune(e){state.frequency=Math.max(8750,Math.min(10800,state.frequency+Math.round(10*e)));updateUI();try{await apiPost('/api/tune',{frequency:state.frequency})}catch(e){}}
async function seek(e){try{const t=await apiPost('/api/seek',{direction:e>0?'up':'down'});t.frequency&&(state.frequency=t.frequency,updateUI(),showToast('Найдена станция: '+(t.frequency/100).toFixed(1)+' MHz','success'))}catch(e){}}
async function setVolume(e){state.volume=parseInt(e);updateUI();try{await apiPost('/api/volume',{volume:state.volume})}catch(e){}}
async function changeVolume(e){await setVolume(Math.max(0,Math.min(15,state.volume+e)))}
async function toggleMute(){try{const e=await apiPost('/api/mute',{muted:!state.muted});state.muted=e.muted;updateUI();showToast(state.muted?'Звук выключен':'Звук включён','info')}catch(e){}}
async function startScan(){if(state.scanning)return;state.scanning=true;document.getElementById('scanBtn').classList.add('scanning','active');showToast('Поиск станций...','info');try{const e=await apiGet('/api/scan');e.stations&&e.stations.length>0?(renderScanResults(e.stations),showToast('Найдено станций: '+e.stations.length,'success')):showToast('Станции не найдены','error')}catch(e){showToast('Ошибка поиска','error')}finally{state.scanning=false;document.getElementById('scanBtn').classList.remove('scanning','active')}}
function renderScanResults(e){document.getElementById('scanCard').style.display='block';document.getElementById('scanList').innerHTML=e.map(e=>`<div class="scan-item" onclick="tuneTo(${e.frequency})"><span class="scan-freq">${(e.frequency/100).toFixed(1)}</span><span class="scan-rssi">${e.rssi}dB</span></div>`).join('')}
async function tuneTo(e){state.frequency=e;updateUI();try{await apiPost('/api/tune',{frequency:e})}catch(e){}}
let pendingSaveFreq=null;function savePreset(){pendingSaveFreq=state.frequency;document.getElementById('presetNameInput').value='';document.getElementById('modalOverlay').classList.add('show');setTimeout(()=>document.getElementById('presetNameInput').focus(),100)}function closeModal(){document.getElementById('modalOverlay').classList.remove('show');pendingSaveFreq=null}async function confirmSavePreset(){const e=document.getElementById('presetNameInput').value.trim();if(!e)return void showToast('Введите название','error');if(state.presets.length>=20)return showToast('Максимум 20 пресетов','error'),void closeModal();state.presets.push({name:e,frequency:pendingSaveFreq||state.frequency}),saveState(),renderPresets(),closeModal(),showToast('Станция сохранена','success'),await syncPresetsToServer()}function loadPreset(e){const t=state.presets[e];t&&(tuneTo(t.frequency),showToast(t.name+' — '+(t.frequency/100).toFixed(1)+' MHz','info'))}function deletePreset(e){state.presets.splice(e,1),saveState(),renderPresets(),showToast('Пресет удалён','info'),syncPresetsToServer()}function clearAllPresets(){state.presets.length&&(confirm('Удалить все пресеты?')&&(state.presets=[],saveState(),renderPresets(),showToast('Все пресеты удалены','info'),syncPresetsToServer()))}function loadState(){try{const e=localStorage.getItem('fm_radio_state');if(e){const t=JSON.parse(e);state.presets=t.presets||[],void 0!==t.volume&&(state.volume=t.volume)}}catch(e){}}function saveState(){try{localStorage.setItem('fm_radio_state',JSON.stringify({presets:state.presets,volume:state.volume}))}catch(e){}}async function loadServerPresets(){try{const e=await apiGet('/api/presets');e.presets&&e.presets.length>0&&(state.presets=e.presets,saveState(),renderPresets())}catch(e){console.log('No server presets')}}async function syncPresetsToServer(){try{await apiPost('/api/presets/sync',{presets:state.presets})}catch(e){console.log('Sync error:',e)}}function showToast(e,t='info'){const n=document.getElementById('toastContainer'),s=document.createElement('div');s.className='toast '+t,s.textContent=e,n.appendChild(s),setTimeout(()=>s.remove(),3e3)}function escapeHtml(e){const t=document.createElement('div');return t.textContent=e,t.innerHTML}document.getElementById('modalOverlay').addEventListener('click',e=>{e.target===e.currentTarget&&closeModal()}),document.getElementById('presetNameInput').addEventListener('keydown',e=>{'Enter'===e.key?confirmSavePreset():'Escape'===e.key&&closeModal()});
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleFirmwareUpdate() {
  setCORS();
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }

  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc.containsKey("version")) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON, need version field\"}");
    return;
  }

  String version = doc["version"].as<String>();
  (void)version;

  // Example: download firmware from URL and update
  if (doc.containsKey("url")) {
    String url = doc["url"].as<String>();
    server.send(200, "application/json", "{\"message\":\"Update started, reconnect to new IP\"}");

    delay(100);
    WiFiClient updateClient;
#if defined(ESP32)
    httpUpdate.update(updateClient, url);
#else
    ESPhttpUpdate.update(updateClient, url);
#endif
  } else {
    StaticJsonDocument<256> resp;
    resp["currentVersion"] = FIRMWARE_VERSION;
    resp["updateUrl"] = "/api/firmware/update";

    String response;
    serializeJson(resp, response);
    server.send(200, "application/json", response);
  }
}

// ============ SETUP & LOOP ============
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n\n=== FM Radio Controller ===");

  // Init EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadPresets();

  // Init I2C (pins are set per board via build_flags in platformio.ini)
  Wire.begin(I2C_SDA, I2C_SCL);

  // Init RDA5807
  rx.setup();
  rx.setVolume(currentVolume);
  rx.setFrequency(8750);   // Start at 87.5 MHz
  rx.setMono(false);       // Enable stereo
  rx.setBass(true);        // Enable bass boost
  rx.setMute(false);

  Serial.print("RDA5807M detected: ");
  Serial.println(radioConnected() ? "YES" : "NO");

  // Init RTC (DS3231 / DS1307)
  initRtc();
  Serial.print("RTC detected: ");
  Serial.println(rtcAvailable ? "YES" : "NO");

  // Init character LCD (PCF8574 backpack)
  initDisplay();
  Serial.print("LCD detected: ");
  if (lcdAvailable) {
    Serial.print("YES (");
    Serial.print(LCD_COLS);
    Serial.print("x");
    Serial.print(LCD_ROWS);
    Serial.println(")");
  } else {
    Serial.println("NO");
  }

  // Init rotary encoder (KY-040)
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderIsr, CHANGE);

  // WiFi setup
#if WIFI_AP_MODE
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("AP Mode");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
#else
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
#endif

  // mDNS: http://fm-radio.local
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS: http://" MDNS_HOSTNAME ".local\n");
  }

  // Web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/tune", HTTP_POST, handleTune);
  server.on("/api/seek", HTTP_POST, handleSeek);
  server.on("/api/volume", HTTP_POST, handleVolume);
  server.on("/api/mute", HTTP_POST, handleMute);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/presets", HTTP_GET, handleGetPresets);
  server.on("/api/presets", HTTP_POST, handleAddPreset);
  server.on("/api/presets/", HTTP_POST, handleAddPreset);
  server.on("/api/presets/{}", HTTP_DELETE, handleDeletePreset);
  server.on("/api/time", HTTP_GET, handleGetTime);
  server.on("/api/time", HTTP_POST, handleSetTime);
  server.on("/api/firmware", HTTP_GET, handleFirmwareUpdate);
  server.on("/api/firmware", HTTP_POST, handleFirmwareUpdate);

  server.onNotFound(handleNotFound);

  // CORS preflight
  server.on("/api/", HTTP_OPTIONS, handleOptions);

  server.begin();

  // OTA Update endpoints
  httpUpdater.setup(&server, "/update", "admin", "admin");

  Serial.println("Web server started on port 80");
  Serial.print("OTA available at: http://");
  Serial.print(WIFI_AP_MODE ? WiFi.softAPIP() : WiFi.localIP());
  Serial.println("/update");
  Serial.println("============================");

  updateDisplay();
}

void loop() {
  server.handleClient();
  handleEncoder();
#if !defined(ESP32)
  MDNS.update();  // ESP32 runs mDNS in a background task, only ESP8266 needs this
#endif
  // Refresh the LCD once per second (non-blocking)
  if (millis() - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
  delay(2);
}
