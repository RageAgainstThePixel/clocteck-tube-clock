#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "clock_core/color.h"
#include "clock_core/display.h"

namespace clock_core {

constexpr size_t kTzMaxLen = 47;

/// What the user can change and what survives a reboot.
struct Settings {
  /// Per-tube hue and saturation; `v` is ignored in favor of `brightness`.
  std::array<Hsv, kTubes> tubes{};
  uint8_t brightness = 99;
  bool is24Hour = false;
  /// POSIX TZ rule, e.g. "EST5EDT,M3.2.0,M11.1.0", so daylight saving is handled on the device.
  char tz[kTzMaxLen + 1] = "EST5EDT,M3.2.0,M11.1.0";

  static Settings defaults();
};

/// Bytes `encodeSettings` writes: magic, version, fields, then a checksum.
constexpr size_t kSettingsBlobSize = 80;

void encodeSettings(const Settings& settings, uint8_t (&out)[kSettingsBlobSize]);

/// False when the blob is blank, from another version, corrupt or out of range. The caller then
/// falls back to `Settings::defaults()`.
bool decodeSettings(const uint8_t (&in)[kSettingsBlobSize], Settings& out);

/// True when both encode to the same blob, i.e. saving `b` over `a` would not change flash. Lets the
/// firmware skip commits for writes that change nothing (FW-SET-007).
bool sameSettings(const Settings& a, const Settings& b);

/// Printable ASCII without spaces, quotes or backslashes, 1 to kTzMaxLen chars. That is enough for
/// any POSIX TZ rule and keeps the value safe to echo back in JSON.
bool isValidTz(const char* tz);

/// Parses a whole decimal string into [min, max]. Rejects empty strings, junk and overflow.
bool parseInt(const char* text, long min, long max, long& out);

}  // namespace clock_core
