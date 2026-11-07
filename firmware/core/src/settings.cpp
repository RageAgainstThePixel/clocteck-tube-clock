#include "clock_core/settings.h"

#include <cstring>

namespace clock_core {

namespace {

constexpr uint8_t kMagic0 = 'T';
constexpr uint8_t kMagic1 = 'C';
constexpr uint8_t kVersion = 1;
constexpr size_t kChecksumAt = kSettingsBlobSize - 4;

uint32_t fnv1a(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace

Settings Settings::defaults() {
  Settings s;
  s.tubes.fill(Hsv{241, 100, 100});
  return s;
}

void encodeSettings(const Settings& settings, uint8_t (&out)[kSettingsBlobSize]) {
  std::memset(out, 0, sizeof(out));
  size_t i = 0;
  out[i++] = kMagic0;
  out[i++] = kMagic1;
  out[i++] = kVersion;
  for (const Hsv& tube : settings.tubes) {
    out[i++] = static_cast<uint8_t>(tube.h & 0xff);
    out[i++] = static_cast<uint8_t>(tube.h >> 8);
    out[i++] = tube.s;
  }
  out[i++] = settings.brightness;
  out[i++] = settings.is24Hour ? 1 : 0;
  std::memcpy(out + i, settings.tz, kTzMaxLen + 1);
  const uint32_t sum = fnv1a(out, kChecksumAt);
  for (int b = 0; b < 4; b++) out[kChecksumAt + b] = static_cast<uint8_t>(sum >> (8 * b));
}

bool decodeSettings(const uint8_t (&in)[kSettingsBlobSize], Settings& out) {
  if (in[0] != kMagic0 || in[1] != kMagic1 || in[2] != kVersion) return false;
  uint32_t stored = 0;
  for (int b = 0; b < 4; b++) stored |= static_cast<uint32_t>(in[kChecksumAt + b]) << (8 * b);
  if (stored != fnv1a(in, kChecksumAt)) return false;

  Settings s;
  size_t i = 3;
  for (Hsv& tube : s.tubes) {
    tube.h = static_cast<uint16_t>(in[i] | (in[i + 1] << 8));
    tube.s = in[i + 2];
    tube.v = 100;
    i += 3;
    if (tube.h > 360 || tube.s > 100) return false;
  }
  s.brightness = in[i++];
  if (in[i] > 1) return false;
  s.is24Hour = in[i++] == 1;
  std::memcpy(s.tz, in + i, kTzMaxLen + 1);
  if (s.tz[kTzMaxLen] != '\0' || !isValidTz(s.tz)) return false;
  out = s;
  return true;
}

bool sameSettings(const Settings& a, const Settings& b) {
  uint8_t blobA[kSettingsBlobSize];
  uint8_t blobB[kSettingsBlobSize];
  encodeSettings(a, blobA);
  encodeSettings(b, blobB);
  return std::memcmp(blobA, blobB, kSettingsBlobSize) == 0;
}

bool isValidTz(const char* tz) {
  if (!tz) return false;
  size_t len = 0;
  for (; tz[len]; len++) {
    const char c = tz[len];
    if (len >= kTzMaxLen || c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '\\') return false;
  }
  return len > 0;
}

bool parseInt(const char* text, long min, long max, long& out) {
  if (!text || !*text) return false;
  const char* p = text;
  const bool negative = *p == '-';
  if (negative) p++;
  if (!*p) return false;
  long value = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    if (value > (max > -min ? max : -min) / 10 + 1) return false;
    value = value * 10 + (*p - '0');
  }
  if (negative) value = -value;
  if (value < min || value > max) return false;
  out = value;
  return true;
}

}  // namespace clock_core
