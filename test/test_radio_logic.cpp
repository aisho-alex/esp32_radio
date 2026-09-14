/*
  Unit tests for radio_logic.h (pure firmware logic, runs on host via env:native
  or on device via: pio test -e esp32dev).
*/

#include <unity.h>
#include <radio_logic.h>
#include <string.h>
#include <stdio.h>
#include <string>

void setUp(void) {}
void tearDown(void) {}

// ---------- isValidFrequency ----------

void test_valid_frequency_bounds(void) {
  TEST_ASSERT_TRUE(isValidFrequency(8750));   // 87.5 MHz - lower edge
  TEST_ASSERT_TRUE(isValidFrequency(10800));  // 108.0 MHz - upper edge
  TEST_ASSERT_TRUE(isValidFrequency(10150));  // 101.5 MHz - middle
}

void test_invalid_frequency_out_of_band(void) {
  TEST_ASSERT_FALSE(isValidFrequency(0));
  TEST_ASSERT_FALSE(isValidFrequency(8749));  // 1 step below the band
  TEST_ASSERT_FALSE(isValidFrequency(10801)); // 1 step above the band
  TEST_ASSERT_FALSE(isValidFrequency(65535));
}

// ---------- presetAdd ----------

void test_add_preset(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;

  int index = presetAdd(presets, count, "Radio Record", 10150);

  TEST_ASSERT_EQUAL_INT(0, index);
  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_EQUAL_STRING("Radio Record", presets[0].name);
  TEST_ASSERT_EQUAL_UINT16(10150, presets[0].frequency);
}

void test_add_preset_appends_sequentially(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;

  TEST_ASSERT_EQUAL_INT(0, presetAdd(presets, count, "A", 8750));
  TEST_ASSERT_EQUAL_INT(1, presetAdd(presets, count, "B", 9000));
  TEST_ASSERT_EQUAL_INT(2, presetAdd(presets, count, "C", 10800));
  TEST_ASSERT_EQUAL_INT(3, count);
  TEST_ASSERT_EQUAL_UINT16(9000, presets[1].frequency);
  TEST_ASSERT_EQUAL_STRING("C", presets[2].name);
}

void test_add_preset_truncates_long_name_to_20_chars(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  // 30 chars long name
  const char* longName = "012345678901234567890123456789";

  presetAdd(presets, count, longName, 10000);

  // 20 chars kept, null-terminated, no buffer overflow
  TEST_ASSERT_EQUAL_INT(20, (int)strlen(presets[0].name));
  TEST_ASSERT_EQUAL_INT('\0', presets[0].name[20]);
  TEST_ASSERT_EQUAL_STRING("01234567890123456789", presets[0].name);
}

void test_add_preset_accepts_20_char_name(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  const char* name20 = "ABCDEFGHIJKLMNOPQRST"; // exactly 20

  presetAdd(presets, count, name20, 10000);

  TEST_ASSERT_EQUAL_STRING(name20, presets[0].name);
}

void test_add_preset_handles_null_name(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;

  presetAdd(presets, count, NULL, 10000);

  TEST_ASSERT_EQUAL_STRING("", presets[0].name);
  TEST_ASSERT_EQUAL_UINT16(10000, presets[0].frequency);
}

void test_add_preset_rejects_when_full(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  char name[8];

  for (int i = 0; i < MAX_PRESETS; i++) {
    snprintf(name, sizeof(name), "P%d", i);
    TEST_ASSERT_EQUAL_INT(i, presetAdd(presets, count, name, 8750 + i * 10));
  }
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, count);

  // 21st preset must be rejected
  TEST_ASSERT_EQUAL_INT(-1, presetAdd(presets, count, "Extra", 10770));
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, count);
  // existing presets stay untouched
  TEST_ASSERT_EQUAL_STRING("P0", presets[0].name);
  TEST_ASSERT_EQUAL_STRING("P19", presets[19].name);
}

// ---------- presetDelete ----------

void test_delete_preset(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "A", 8750);

  TEST_ASSERT_TRUE(presetDelete(presets, count, 0));
  TEST_ASSERT_EQUAL_INT(0, count);
}

void test_delete_preset_shifts_remaining(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "A", 8750);
  presetAdd(presets, count, "B", 9000);
  presetAdd(presets, count, "C", 10250);

  TEST_ASSERT_TRUE(presetDelete(presets, count, 1)); // remove middle

  TEST_ASSERT_EQUAL_INT(2, count);
  TEST_ASSERT_EQUAL_STRING("A", presets[0].name);
  TEST_ASSERT_EQUAL_STRING("C", presets[1].name);
  TEST_ASSERT_EQUAL_UINT16(10250, presets[1].frequency);
}

void test_delete_preset_invalid_index(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "A", 8750);
  presetAdd(presets, count, "B", 9000);

  TEST_ASSERT_FALSE(presetDelete(presets, count, -1));  // negative
  TEST_ASSERT_FALSE(presetDelete(presets, count, 2));   // == count
  TEST_ASSERT_FALSE(presetDelete(presets, count, 100)); // far out
  TEST_ASSERT_EQUAL_INT(2, count);                      // nothing removed
}

void test_delete_preset_on_empty_list(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;

  TEST_ASSERT_FALSE(presetDelete(presets, count, 0));
  TEST_ASSERT_EQUAL_INT(0, count);
}

void test_delete_last_preset_of_filled_list(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  for (int i = 0; i < MAX_PRESETS; i++) {
    presetAdd(presets, count, "S", 8750 + i);
  }

  TEST_ASSERT_TRUE(presetDelete(presets, count, MAX_PRESETS - 1));
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS - 1, count);
  TEST_ASSERT_EQUAL_STRING("S", presets[MAX_PRESETS - 2].name);
}

// ---------- presetsToJson (API contract) ----------

void test_presets_json_contract(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "Europe Plus", 10620);
  presetAdd(presets, count, "Radio Dacha", 10030);

  StaticJsonDocument<1024> doc;
  presetsToJson(presets, count, doc);

  const char* json = "{\"presets\":[{\"name\":\"Europe Plus\",\"frequency\":10620},{\"name\":\"Radio Dacha\",\"frequency\":10030}]}";
  TEST_ASSERT_EQUAL_STRING(json, doc.as<std::string>().c_str());
}

void test_presets_json_empty_list(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;

  StaticJsonDocument<1024> doc;
  presetsToJson(presets, count, doc);

  TEST_ASSERT_EQUAL_STRING("{\"presets\":[]}", doc.as<std::string>().c_str());
}

void test_presets_json_after_delete(void) {
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "A", 8750);
  presetAdd(presets, count, "B", 9000);
  presetAdd(presets, count, "C", 10250);
  presetDelete(presets, count, 0);

  StaticJsonDocument<1024> doc;
  presetsToJson(presets, count, doc);

  TEST_ASSERT_EQUAL_STRING("{\"presets\":[{\"name\":\"B\",\"frequency\":9000},{\"name\":\"C\",\"frequency\":10250}]}", doc.as<std::string>().c_str());
}

// ---------- statusToJson (API contract) ----------

void test_status_json_contract(void) {
  StaticJsonDocument<256> doc;
  statusToJson(10150, 8, false, true, 24, doc);

  TEST_ASSERT_EQUAL_STRING("{\"frequency\":10150,\"volume\":8,\"muted\":false,\"stereo\":true,\"rssi\":24}", doc.as<std::string>().c_str());
}

void test_status_json_muted_mono(void) {
  StaticJsonDocument<256> doc;
  statusToJson(8750, 0, true, false, 0, doc);

  TEST_ASSERT_EQUAL_STRING("{\"frequency\":8750,\"volume\":0,\"muted\":true,\"stereo\":false,\"rssi\":0}", doc.as<std::string>().c_str());
}

// ---------- round trip: parse back like the web UI does ----------

void test_status_json_matches_web_ui_poll_fields(void) {
  // web UI pollStatus() reads: frequency, volume, muted, stereo, rssi
  StaticJsonDocument<256> doc;
  statusToJson(9350, 12, false, true, 30, doc);

  DynamicJsonDocument parsed(256);
  DeserializationError err = deserializeJson(parsed, doc.as<std::string>());

  TEST_ASSERT_FALSE(err);
  TEST_ASSERT_TRUE(parsed.containsKey("frequency"));
  TEST_ASSERT_TRUE(parsed.containsKey("volume"));
  TEST_ASSERT_TRUE(parsed.containsKey("muted"));
  TEST_ASSERT_TRUE(parsed.containsKey("stereo"));
  TEST_ASSERT_TRUE(parsed.containsKey("rssi"));
  TEST_ASSERT_EQUAL_UINT16(9350, parsed["frequency"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT(12, parsed["volume"].as<int>());
  TEST_ASSERT_FALSE(parsed["muted"].as<bool>());
  TEST_ASSERT_TRUE(parsed["stereo"].as<bool>());
  TEST_ASSERT_EQUAL_INT(30, parsed["rssi"].as<int>());
}

void test_presets_json_round_trip_like_web_ui(void) {
  // web UI syncPresetsToServer() posts {"presets":[{name, frequency}, ...]}
  Preset presets[MAX_PRESETS];
  int count = 0;
  presetAdd(presets, count, "NRJ", 10100);

  StaticJsonDocument<1024> doc;
  presetsToJson(presets, count, doc);

  DynamicJsonDocument parsed(1024);
  TEST_ASSERT_FALSE(deserializeJson(parsed, doc.as<std::string>()));
  JsonArray arr = parsed["presets"];
  TEST_ASSERT_EQUAL_INT(1, arr.size());
  TEST_ASSERT_EQUAL_STRING("NRJ", arr[0]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT16(10100, arr[0]["frequency"].as<uint16_t>());
}

// ---------- tune request contract ----------

void test_tune_request_valid_and_invalid(void) {
  // /api/tune {"frequency": 10150} - firmware tunes only in-band values
  StaticJsonDocument<128> req;
  deserializeJson(req, "{\"frequency\":10150}");
  uint16_t freq = req["frequency"] | 0;
  TEST_ASSERT_TRUE(isValidFrequency(freq));

  deserializeJson(req, "{}"); // missing field -> default 0 -> rejected
  freq = req["frequency"] | 0;
  TEST_ASSERT_FALSE(isValidFrequency(freq));

  deserializeJson(req, "{\"frequency\":123456}"); // out of band -> rejected
  freq = req["frequency"] | 0;
  TEST_ASSERT_FALSE(isValidFrequency(freq));
}

// ---------- LCD line formatting ----------

void test_format_freq_line_stereo(void) {
  char buf[24];
  formatFreqLine(buf, sizeof(buf), 10150, true);
  TEST_ASSERT_EQUAL_STRING("FM 101.50 MHz ST", buf);
}

void test_format_freq_line_mono_pads_to_16(void) {
  char buf[24];
  formatFreqLine(buf, sizeof(buf), 8750, false);
  // frequency is right-aligned in 13 cols, ST field padded with spaces
  TEST_ASSERT_EQUAL_STRING("FM  87.50 MHz   ", buf);
  TEST_ASSERT_EQUAL_INT(16, (int)strlen(buf));
}

void test_format_freq_line_band_top_stereo_16_chars(void) {
  char buf[24];
  formatFreqLine(buf, sizeof(buf), 10800, true);
  TEST_ASSERT_EQUAL_STRING("FM 108.00 MHz ST", buf);
  TEST_ASSERT_EQUAL_INT(16, (int)strlen(buf));
}

void test_format_status_line_normal(void) {
  char buf[24];
  formatStatusLine(buf, sizeof(buf), 8, false, 24, 12, 34, true);
  TEST_ASSERT_EQUAL_STRING(" V08 R24 12:34", buf);
}

void test_format_status_line_muted_shows_dash_volume(void) {
  char buf[24];
  formatStatusLine(buf, sizeof(buf), 0, true, 10, 23, 59, true);
  TEST_ASSERT_EQUAL_STRING("MV-- R10 23:59", buf);
}

void test_format_status_line_without_rtc(void) {
  char buf[24];
  formatStatusLine(buf, sizeof(buf), 8, false, 0, 0, 0, false);
  TEST_ASSERT_EQUAL_STRING(" V08 R00 --:--", buf);
}

void test_format_status_line_midnight_max_rssi(void) {
  char buf[24];
  formatStatusLine(buf, sizeof(buf), 0, false, 63, 0, 5, true);
  TEST_ASSERT_EQUAL_STRING(" V00 R63 00:05", buf);
}

void test_format_status_line_max_volume(void) {
  char buf[24];
  formatStatusLine(buf, sizeof(buf), 15, false, 5, 9, 5, true);
  TEST_ASSERT_EQUAL_STRING(" V15 R05 09:05", buf);
}

// ---------- padTo ----------

void test_pad_to_pads_short_string(void) {
  char buf[8];
  strcpy(buf, "AB");
  padTo(buf, sizeof(buf), 5);
  TEST_ASSERT_EQUAL_STRING("AB   ", buf);
}

void test_pad_to_exact_width_unchanged(void) {
  char buf[8];
  strcpy(buf, "ABCDE");
  padTo(buf, sizeof(buf), 5);
  TEST_ASSERT_EQUAL_STRING("ABCDE", buf);
}

void test_pad_to_truncates_longer_than_width(void) {
  char buf[8];
  strcpy(buf, "ABCDEFGH");
  padTo(buf, sizeof(buf), 6);
  TEST_ASSERT_EQUAL_STRING("ABCDEF", buf);
}

void test_pad_to_guards_buffer_size(void) {
  char buf[8]; // real buffer is bigger, but padTo must respect bufSize = 4
  strcpy(buf, "ABCDE");
  padTo(buf, 4, 10); // width clamped to 3, string truncated, no overflow
  TEST_ASSERT_EQUAL_STRING("ABC", buf);
  TEST_ASSERT_EQUAL_INT('\0', buf[3]);
}

void test_pad_to_zero_width(void) {
  char buf[8];
  strcpy(buf, "AB");
  padTo(buf, sizeof(buf), 0);
  TEST_ASSERT_EQUAL_STRING("", buf);
}

// ---------- rotary encoder ----------

void test_encoder_step_volume_clamps(void) {
  TEST_ASSERT_EQUAL_INT(9, encoderStepVolume(8, 1));
  TEST_ASSERT_EQUAL_INT(7, encoderStepVolume(8, -1));
  TEST_ASSERT_EQUAL_INT(0, encoderStepVolume(0, -1));   // below min
  TEST_ASSERT_EQUAL_INT(15, encoderStepVolume(15, 1));  // above max
  TEST_ASSERT_EQUAL_INT(15, encoderStepVolume(13, 5));  // overshoot
  TEST_ASSERT_EQUAL_INT(0, encoderStepVolume(2, -5));   // undershoot
}

void test_encoder_step_volume_multi_detents(void) {
  TEST_ASSERT_EQUAL_INT(8, encoderStepVolume(5, 3));
  TEST_ASSERT_EQUAL_INT(2, encoderStepVolume(5, -3));
  TEST_ASSERT_EQUAL_INT(15, encoderStepVolume(10, 10));
}

void test_encoder_step_frequency_clamps_to_band(void) {
  TEST_ASSERT_EQUAL_UINT16(8750, encoderStepFrequency(8750, -1));   // below band
  TEST_ASSERT_EQUAL_UINT16(10800, encoderStepFrequency(10800, 1));  // above band
  TEST_ASSERT_EQUAL_UINT16(8760, encoderStepFrequency(8750, 1));
  TEST_ASSERT_EQUAL_UINT16(10790, encoderStepFrequency(10800, -1));
}

void test_encoder_step_frequency_multi_detents(void) {
  TEST_ASSERT_EQUAL_UINT16(10200, encoderStepFrequency(10150, 5));
  TEST_ASSERT_EQUAL_UINT16(10100, encoderStepFrequency(10150, -5));
  TEST_ASSERT_EQUAL_UINT16(10150, encoderStepFrequency(10150, 0));
  TEST_ASSERT_EQUAL_UINT16(10800, encoderStepFrequency(10750, 10));  // clamp mid-path
}

void test_encoder_press_classification_thresholds(void) {
  TEST_ASSERT_EQUAL_INT(PRESS_NONE, classifyEncoderPress(0));
  TEST_ASSERT_EQUAL_INT(PRESS_SHORT, classifyEncoderPress(1));
  TEST_ASSERT_EQUAL_INT(PRESS_SHORT, classifyEncoderPress(100));
  TEST_ASSERT_EQUAL_INT(PRESS_SHORT, classifyEncoderPress(599));  // just below threshold
  TEST_ASSERT_EQUAL_INT(PRESS_LONG, classifyEncoderPress(600));   // threshold inclusive
  TEST_ASSERT_EQUAL_INT(PRESS_LONG, classifyEncoderPress(5000));
}

static void runAllTests(void) {
  RUN_TEST(test_valid_frequency_bounds);
  RUN_TEST(test_invalid_frequency_out_of_band);

  RUN_TEST(test_add_preset);
  RUN_TEST(test_add_preset_appends_sequentially);
  RUN_TEST(test_add_preset_truncates_long_name_to_20_chars);
  RUN_TEST(test_add_preset_accepts_20_char_name);
  RUN_TEST(test_add_preset_handles_null_name);
  RUN_TEST(test_add_preset_rejects_when_full);

  RUN_TEST(test_delete_preset);
  RUN_TEST(test_delete_preset_shifts_remaining);
  RUN_TEST(test_delete_preset_invalid_index);
  RUN_TEST(test_delete_preset_on_empty_list);
  RUN_TEST(test_delete_last_preset_of_filled_list);

  RUN_TEST(test_presets_json_contract);
  RUN_TEST(test_presets_json_empty_list);
  RUN_TEST(test_presets_json_after_delete);

  RUN_TEST(test_status_json_contract);
  RUN_TEST(test_status_json_muted_mono);

  RUN_TEST(test_status_json_matches_web_ui_poll_fields);
  RUN_TEST(test_presets_json_round_trip_like_web_ui);
  RUN_TEST(test_tune_request_valid_and_invalid);

  RUN_TEST(test_format_freq_line_stereo);
  RUN_TEST(test_format_freq_line_mono_pads_to_16);
  RUN_TEST(test_format_freq_line_band_top_stereo_16_chars);
  RUN_TEST(test_format_status_line_normal);
  RUN_TEST(test_format_status_line_muted_shows_dash_volume);
  RUN_TEST(test_format_status_line_without_rtc);
  RUN_TEST(test_format_status_line_midnight_max_rssi);
  RUN_TEST(test_format_status_line_max_volume);

  RUN_TEST(test_pad_to_pads_short_string);
  RUN_TEST(test_pad_to_exact_width_unchanged);
  RUN_TEST(test_pad_to_truncates_longer_than_width);
  RUN_TEST(test_pad_to_guards_buffer_size);
  RUN_TEST(test_pad_to_zero_width);

  RUN_TEST(test_encoder_step_volume_clamps);
  RUN_TEST(test_encoder_step_volume_multi_detents);
  RUN_TEST(test_encoder_step_frequency_clamps_to_band);
  RUN_TEST(test_encoder_step_frequency_multi_detents);
  RUN_TEST(test_encoder_press_classification_thresholds);
}

#ifdef ARDUINO

#include <Arduino.h>

void setup() {
  delay(2000); // time for the serial monitor to settle
  UNITY_BEGIN();
  runAllTests();
  UNITY_END();
}

void loop() {}

#else

int main(void) {
  UNITY_BEGIN();
  runAllTests();
  return UNITY_END();
}

#endif
