#include "clock_core/effect.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "clock_core/json.h"
#include "clock_core/json_arena.h"

// On the ESP8285 the presets, messages and the sine table stay in flash; ArduinoJson has pulled in
// Arduino.h.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef ARDUINO
#define memcpy_P memcpy
#define strlen_P strlen
#define pgm_read_byte(p) (*reinterpret_cast<const uint8_t*>(p))
#endif

namespace clock_core {

namespace {

// FW-FX-071: (1 - cos(pi x)) / 2 at x = i/64, scaled to 255. The only easing that is not a formula.
const uint8_t kSineEase[64] PROGMEM = {
    0,   0,   1,   1,   2,   4,   5,   7,   10,  12,  15,  18,  21,  25,  29,  33,
    37,  42,  47,  52,  57,  62,  67,  73,  79,  85,  90,  97,  103, 109, 115, 121,
    127, 134, 140, 146, 152, 158, 165, 170, 176, 182, 188, 193, 198, 203, 208, 213,
    218, 222, 226, 230, 234, 237, 240, 243, 245, 248, 250, 251, 253, 254, 254, 255,
};

// FxError order, NUL-separated (FW-API-002: names the valid form, echoes nothing).
const char kFxErrors[] PROGMEM =
    "ok\0"
    "body too large (limit 1024 bytes)\0"
    "body must be a JSON object\0"
    "unknown field (the fields are listed in spec/firmware/effects.md)\0"
    "pattern is required\0"
    "pattern: solid|tubes|wave|gradient|breathe|sparkle|chase|fire|strobe|matrix\0"
    "palette: need rainbow or an array of colors\0"
    "palette: need 1-8 colors\0"
    "palette: colors are a name, #rrggbb, #rgb or h,s,v\0"
    "palette: a stop object is {at: 0-100, color}\0"
    "palette: at must not decrease\0"
    "interpolate: need hsv or rgb\0"
    "direction: need ltr, rtl, center or edges\0"
    "period: need 0 or 100-600000\0"
    "phase: need 0-100\0"
    "drift: need 0 or 100-600000\0"
    "easing: need linear, sine, ease-in, ease-out, ease-in-out or step\0"
    "pulse: need null or {min: 0-255, max: 0-255, period: 0 or 100-600000, easing}\0"
    "pulse: min must not exceed max\0"
    "density: need 1-100\0"
    "width: need 1-6\0"
    "count: need 1-6\0"
    "duty: need 0-100\0"
    "seed: need 0-65535\0"
    "glyphs: need true or false\0"
    "accent: need null or {style, ms 50-3000, color, tubes, spread, strength}\0";

const char* const kPatternNames[] = {"solid", "tubes", "wave", "gradient", "breathe", "sparkle", "chase", "fire", "strobe", "matrix"};
const char* const kAccentStyleNames[] = {"flicker", "flash", "dip", "glow", "shift"};
const char* const kInterpolateNames[] = {"hsv", "rgb"};
const char* const kDirectionNames[] = {"ltr", "rtl", "center", "edges"};
const char* const kEasingNames[] = {"linear", "sine", "ease-in", "ease-out", "ease-in-out", "step"};

template <size_t N>
int nameIndex(const char* const (&names)[N], const char* text) {
  for (size_t i = 0; i < N; i++) {
    if (strcmp(names[i], text) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool validPeriod(uint32_t ms) { return ms == 0 || (ms >= 100 && ms <= 600000); }

void setRainbow(Effect& e) {
  // The stops are what `tubes`, `breathe` and a stepped palette see of the wheel; the lookup of a
  // fading rainbow walks the wheel itself.
  static const uint16_t kHues[6] = {0, 60, 120, 180, 240, 300};
  e.rainbow = true;
  e.stopCount = 6;
  for (int i = 0; i < 6; i++) e.stops[i] = {hsvToRgb({kHues[i], 100, 100}), kFxNoAt};
}

void resetPatternFields(Effect& e, FxPattern pattern) {
  // `breathe` has its sine swing (FW-FX-024); the v1.1 patterns have density, width, count, duty and
  // seed, which only they read, so any switch puts those back.
  const Effect fresh = Effect::defaults(pattern);
  if (pattern == FxPattern::kBreathe || e.pattern == FxPattern::kBreathe) {
    e.easing = fresh.easing;
    e.hasPulse = fresh.hasPulse;
    e.pulse = fresh.pulse;
  }
  e.density = fresh.density;
  e.width = fresh.width;
  e.count = fresh.count;
  e.duty = fresh.duty;
  e.seed = fresh.seed;
  e.glyphs = fresh.glyphs;
  e.pattern = pattern;
}

bool usesDensity(FxPattern p) { return p == FxPattern::kSparkle || p == FxPattern::kFire || p == FxPattern::kMatrix; }

// FW-FX-016: "all", "hm" or six of 0 and 1, left to right.
bool parseAccentTubes(const char* text, uint8_t& out) {
  if (strcmp(text, "all") == 0) {
    out = kAccentAllTubes;
    return true;
  }
  if (strcmp(text, "hm") == 0) {
    out = kAccentHmTubes;
    return true;
  }
  if (strlen(text) != kTubes) return false;
  uint8_t mask = 0;
  for (int t = 0; t < kTubes; t++) {
    if (text[t] != '0' && text[t] != '1') return false;
    if (text[t] == '1') mask = static_cast<uint8_t>(mask | (1u << t));
  }
  out = mask;
  return true;
}

void formatAccentTubes(uint8_t mask, char (&out)[kTubes + 1]) {
  if (mask == kAccentAllTubes) {
    strcpy(out, "all");
  } else if (mask == kAccentHmTubes) {
    strcpy(out, "hm");
  } else {
    for (int t = 0; t < kTubes; t++) out[t] = (mask >> t) & 1 ? '1' : '0';
    out[kTubes] = '\0';
  }
}

// A whole number in [lo, hi], or false.
bool readSmall(JsonVariantConst v, uint32_t lo, uint32_t hi, uint32_t& out) {
  if (!v.is<uint32_t>()) return false;
  out = v.as<uint32_t>();
  return out >= lo && out <= hi;
}

FxError parseStop(JsonVariantConst v, FxStop& stop) {
  const char* text = nullptr;
  stop.at = kFxNoAt;
  if (v.is<const char*>()) {
    text = v.as<const char*>();
  } else if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      if (kv.key() == "color") {
        if (!kv.value().is<const char*>()) return FxError::kColor;
        text = kv.value().as<const char*>();
      } else if (kv.key() == "at") {
        if (!kv.value().is<uint8_t>() || kv.value().as<uint8_t>() > 100) return FxError::kStop;
        stop.at = kv.value().as<uint8_t>();
      } else {
        return FxError::kStop;
      }
    }
    if (!text) return FxError::kStop;
  } else {
    return FxError::kColor;
  }
  return parseColorRgb(text, stop.color) ? FxError::kNone : FxError::kColor;
}

// A stop's place in the cycle, in units of 1/6400.
int stopPosition(const Effect& e, int i) {
  return e.stops[i].at != kFxNoAt ? e.stops[i].at * 64 : i * 6400 / e.stopCount;
}

FxError parsePalette(JsonVariantConst v, Effect& e) {
  if (v.is<const char*>()) {
    if (strcmp(v.as<const char*>(), "rainbow") != 0) return FxError::kPalette;
    setRainbow(e);
    return FxError::kNone;
  }
  if (!v.is<JsonArrayConst>()) return FxError::kPalette;
  JsonArrayConst list = v.as<JsonArrayConst>();
  if (list.size() < 1 || list.size() > kFxMaxStops) return FxError::kPaletteSize;
  e.rainbow = false;
  e.stopCount = static_cast<uint8_t>(list.size());
  int i = 0;
  for (JsonVariantConst item : list) {
    const FxError err = parseStop(item, e.stops[i++]);
    if (err != FxError::kNone) return err;
  }
  for (int k = 1; k < e.stopCount; k++) {
    if (stopPosition(e, k) < stopPosition(e, k - 1)) return FxError::kStopOrder;
  }
  return FxError::kNone;
}

FxError parsePulse(JsonVariantConst v, Effect& e) {
  if (v.isNull()) {
    // `breathe` always has a swing, so null puts its default back.
    const Effect fresh = Effect::defaults(e.pattern);
    e.hasPulse = fresh.hasPulse;
    e.pulse = fresh.pulse;
    return FxError::kNone;
  }
  if (!v.is<JsonObjectConst>()) return FxError::kPulse;
  FxPulse p = e.hasPulse ? e.pulse : FxPulse{};
  for (JsonPairConst kv : v.as<JsonObjectConst>()) {
    JsonVariantConst value = kv.value();
    if (kv.key() == "min") {
      if (!value.is<uint8_t>()) return FxError::kPulse;
      p.min = value.as<uint8_t>();
    } else if (kv.key() == "max") {
      if (!value.is<uint8_t>()) return FxError::kPulse;
      p.max = value.as<uint8_t>();
    } else if (kv.key() == "period") {
      if (!value.is<uint32_t>() || !validPeriod(value.as<uint32_t>())) return FxError::kPulse;
      p.period = value.as<uint32_t>();
    } else if (kv.key() == "easing") {
      const int index = value.is<const char*>() ? nameIndex(kEasingNames, value.as<const char*>()) : -1;
      if (index < 0) return FxError::kPulse;
      p.easing = static_cast<FxEasing>(index);
    } else {
      return FxError::kPulse;
    }
  }
  if (p.min > p.max) return FxError::kPulseMinMax;
  e.hasPulse = true;
  e.pulse = p;
  return FxError::kNone;
}

// FW-FX-016: like pulse, a missing field keeps the current accent's value (or the default), and null
// removes the accent.
FxError parseAccent(JsonVariantConst v, Effect& e) {
  if (v.isNull()) {
    e.hasAccent = false;
    e.accent = FxAccent{};
    return FxError::kNone;
  }
  if (!v.is<JsonObjectConst>()) return FxError::kAccent;
  FxAccent a = e.hasAccent ? e.accent : FxAccent{};
  for (JsonPairConst kv : v.as<JsonObjectConst>()) {
    JsonVariantConst value = kv.value();
    uint32_t n = 0;
    if (kv.key() == "style") {
      const int index = value.is<const char*>() ? nameIndex(kAccentStyleNames, value.as<const char*>()) : -1;
      if (index < 0) return FxError::kAccent;
      a.style = static_cast<FxAccentStyle>(index);
    } else if (kv.key() == "ms") {
      if (!readSmall(value, kAccentMinMs, kAccentMaxMs, n)) return FxError::kAccent;
      a.ms = static_cast<uint16_t>(n);
    } else if (kv.key() == "color") {
      if (value.isNull()) {
        a.hasColor = false;
      } else {
        if (!value.is<const char*>() || !parseColorRgb(value.as<const char*>(), a.color)) return FxError::kAccent;
        a.hasColor = true;
      }
    } else if (kv.key() == "tubes") {
      if (!value.is<const char*>() || !parseAccentTubes(value.as<const char*>(), a.tubes)) return FxError::kAccent;
    } else if (kv.key() == "spread") {
      if (!readSmall(value, 0, kAccentMaxSpreadMs, n)) return FxError::kAccent;
      a.spread = static_cast<uint16_t>(n);
    } else if (kv.key() == "strength") {
      if (!readSmall(value, 0, 100, n)) return FxError::kAccent;
      a.strength = static_cast<uint8_t>(n);
    } else {
      return FxError::kAccent;
    }
  }
  if (!a.hasColor) a.color = Rgb{};
  e.hasAccent = true;
  e.accent = a;
  return FxError::kNone;
}

// `preset` non-null: the /effect.json form, which also carries the preset's name.
FxError parseImpl(const char* json, size_t len, const Effect* base, Effect& out, char (*preset)[kPresetNameMax + 1]) {
  if (len > kFxMaxBodyBytes) return FxError::kTooLarge;
  // FW-FX-073: one 2 KB document, freed before this returns. Too big for the 4 KB loop stack (FW-BUD-009).
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kFxArenaBytes]);
  if (!buf) return FxError::kTooLarge;
  FixedArena arena(buf.get(), kFxArenaBytes);
  JsonDocument doc(&arena);
  const DeserializationError parsed = deserializeJson(doc, json, len);
  if (parsed == DeserializationError::NoMemory) return FxError::kTooLarge;
  if (parsed || !doc.is<JsonObjectConst>()) return FxError::kBadJson;
  JsonObjectConst root = doc.as<JsonObjectConst>();

  // FW-FX-013: unknown fields are errors, so typos in presets are caught.
  for (JsonPairConst kv : root) {
    const bool known = kv.key() == "pattern" || kv.key() == "palette" || kv.key() == "interpolate" ||
                       kv.key() == "direction" || kv.key() == "period" || kv.key() == "phase" ||
                       kv.key() == "drift" || kv.key() == "easing" || kv.key() == "pulse" || kv.key() == "density" ||
                       kv.key() == "width" || kv.key() == "count" || kv.key() == "duty" || kv.key() == "seed" ||
                       kv.key() == "glyphs" || kv.key() == "accent" || (preset && kv.key() == "preset");
    if (!known) return FxError::kUnknownField;
  }

  JsonVariantConst v = root["pattern"];
  int pattern = -1;
  if (!v.isUnbound()) {
    pattern = v.is<const char*>() ? nameIndex(kPatternNames, v.as<const char*>()) : -1;
    if (pattern < 0) return FxError::kPattern;
  } else if (!base) {
    return FxError::kPatternMissing;
  }
  Effect e = base ? *base : Effect::defaults(static_cast<FxPattern>(pattern));
  if (pattern >= 0 && static_cast<FxPattern>(pattern) != e.pattern) resetPatternFields(e, static_cast<FxPattern>(pattern));

  v = root["palette"];
  if (!v.isUnbound()) {
    const FxError err = parsePalette(v, e);
    if (err != FxError::kNone) return err;
  }
  v = root["interpolate"];
  if (!v.isUnbound()) {
    const int index = v.is<const char*>() ? nameIndex(kInterpolateNames, v.as<const char*>()) : -1;
    if (index < 0) return FxError::kInterpolate;
    e.interpolate = static_cast<FxInterpolate>(index);
  }
  v = root["direction"];
  if (!v.isUnbound()) {
    const int index = v.is<const char*>() ? nameIndex(kDirectionNames, v.as<const char*>()) : -1;
    if (index < 0) return FxError::kDirection;
    e.direction = static_cast<FxDirection>(index);
  }
  v = root["period"];
  if (!v.isUnbound()) {
    if (!v.is<uint32_t>() || !validPeriod(v.as<uint32_t>())) return FxError::kPeriod;
    e.period = v.as<uint32_t>();
  }
  v = root["phase"];
  if (!v.isUnbound()) {
    if (!v.is<uint8_t>() || v.as<uint8_t>() > 100) return FxError::kPhase;
    e.phase = v.as<uint8_t>();
  }
  v = root["drift"];
  if (!v.isUnbound()) {
    if (!v.is<uint32_t>() || !validPeriod(v.as<uint32_t>())) return FxError::kDrift;
    e.drift = v.as<uint32_t>();
  }
  v = root["easing"];
  if (!v.isUnbound()) {
    const int index = v.is<const char*>() ? nameIndex(kEasingNames, v.as<const char*>()) : -1;
    if (index < 0) return FxError::kEasing;
    e.easing = static_cast<FxEasing>(index);
  }
  v = root["pulse"];
  if (!v.isUnbound()) {
    const FxError err = parsePulse(v, e);
    if (err != FxError::kNone) return err;
  }
  // FW-FX-009..012: accepted on any pattern, read only by their own.
  uint32_t n = 0;
  v = root["density"];
  if (!v.isUnbound()) {
    if (!readSmall(v, 1, 100, n)) return FxError::kDensity;
    e.density = static_cast<uint8_t>(n);
  }
  v = root["width"];
  if (!v.isUnbound()) {
    if (!readSmall(v, 1, kTubes, n)) return FxError::kWidth;
    e.width = static_cast<uint8_t>(n);
  }
  v = root["count"];
  if (!v.isUnbound()) {
    if (!readSmall(v, 1, kTubes, n)) return FxError::kCount;
    e.count = static_cast<uint8_t>(n);
  }
  v = root["duty"];
  if (!v.isUnbound()) {
    if (!readSmall(v, 0, 100, n)) return FxError::kDuty;
    e.duty = static_cast<uint8_t>(n);
  }
  v = root["seed"];
  if (!v.isUnbound()) {
    if (!readSmall(v, 0, 65535, n)) return FxError::kSeed;
    e.seed = static_cast<uint16_t>(n);
  }
  v = root["glyphs"];
  if (!v.isUnbound()) {
    if (!v.is<bool>()) return FxError::kGlyphs;
    e.glyphs = v.as<bool>();
  }
  v = root["accent"];
  if (!v.isUnbound()) {
    const FxError err = parseAccent(v, e);
    if (err != FxError::kNone) return err;
  }

  if (preset) {
    (*preset)[0] = '\0';
    v = root["preset"];
    if (v.is<const char*>() && isValidPresetName(v.as<const char*>())) strcpy(*preset, v.as<const char*>());
  }
  out = e;
  return FxError::kNone;
}

// Keeps the buffer NUL-terminated; anything that does not fit sets `overflow`.
struct Json {
  char* buf;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  Json(char* b, size_t c) : buf(b), cap(c) {
    if (cap) buf[0] = '\0';
  }
  void str(const char* s) {
    const size_t n = strlen(s);
    if (overflow || len + n >= cap) {
      overflow = true;
      return;
    }
    memcpy(buf + len, s, n + 1);
    len += n;
  }
  void num(unsigned long v) {
    char t[12];
    snprintf(t, sizeof(t), "%lu", v);
    str(t);
  }
  // `,"key":`
  void key(const char* name, bool first = false) {
    str(first ? "\"" : ",\"");
    str(name);
    str("\":");
  }
  void quoted(const char* s) {
    str("\"");
    str(s);
    str("\"");
  }
};

// ---- Palette expansion, at apply time ----

constexpr int kHueSteps = 1536;  // six sectors of 256

struct WideHsv {
  int h;  // 0..1535
  int s;  // 0..255
  int v;  // 0..255
};

WideHsv toWideHsv(const Rgb& c) {
  const int r = c.r, g = c.g, b = c.b;
  const int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const int delta = max - min;
  int h = 0;
  if (delta) {
    if (max == r) {
      h = (g - b) * 256 / delta;
    } else if (max == g) {
      h = 512 + (b - r) * 256 / delta;
    } else {
      h = 1024 + (r - g) * 256 / delta;
    }
    if (h < 0) h += kHueSteps;
  }
  return {h, max ? delta * 255 / max : 0, max};
}

Rgb fromWideHsv(const WideHsv& c) {
  const int sector = c.h >> 8;
  const int f = c.h & 255;
  auto byte = [](int x) { return static_cast<uint8_t>(x); };
  const int p = c.v * (255 - c.s) / 255;
  const int q = c.v * (255 - c.s * f / 255) / 255;
  const int t = c.v * (255 - c.s * (255 - f) / 255) / 255;
  switch (sector) {
    case 0: return {byte(c.v), byte(t), byte(p)};
    case 1: return {byte(q), byte(c.v), byte(p)};
    case 2: return {byte(p), byte(c.v), byte(t)};
    case 3: return {byte(p), byte(q), byte(c.v)};
    case 4: return {byte(t), byte(p), byte(c.v)};
    default: return {byte(c.v), byte(p), byte(q)};
  }
}

int lerpInt(int a, int b, int f256) { return a + (b - a) * f256 / 256; }

// FW-FX-003: `f256` of the way from a to b, 0-255.
Rgb mix(const Rgb& a, const Rgb& b, int f256, FxInterpolate how) {
  if (f256 == 0) return a;
  if (how == FxInterpolate::kRgb) {
    return {static_cast<uint8_t>(lerpInt(a.r, b.r, f256)), static_cast<uint8_t>(lerpInt(a.g, b.g, f256)),
            static_cast<uint8_t>(lerpInt(a.b, b.b, f256))};
  }
  WideHsv from = toWideHsv(a);
  WideHsv to = toWideHsv(b);
  // A gray or black stop has no hue of its own: take the other's, or a fade to white detours round
  // the wheel.
  if (from.s == 0 || from.v == 0) from.h = to.h;
  if (to.s == 0 || to.v == 0) to.h = from.h;
  int dh = to.h - from.h;
  if (dh > kHueSteps / 2) dh -= kHueSteps;
  if (dh < -kHueSteps / 2) dh += kHueSteps;
  const int h = (from.h + dh * f256 / 256 + kHueSteps) % kHueSteps;
  return fromWideHsv({h, lerpInt(from.s, to.s, f256), lerpInt(from.v, to.v, f256)});
}

uint32_t cycle16(uint64_t tMs, uint32_t period) {
  if (period == 0) return 0;
  return static_cast<uint32_t>((static_cast<uint64_t>(tMs % period) << 16) / period);
}

// Up and back over one cycle, 0 at the start, the top halfway.
uint16_t triangle(uint32_t pos16) {
  return static_cast<uint16_t>(pos16 < 32768 ? pos16 * 2 : (65535 - pos16) * 2);
}

uint8_t envelope(uint8_t min, uint8_t max, FxEasing easing, uint32_t pos16) {
  const uint32_t e = fxEase(easing, triangle(pos16));
  return static_cast<uint8_t>(min + ((max - min) * e + 32768) / 65536);
}

// The preset table (FW-FX-041, built-ins table in effects.md). One flash string each, `pattern`
// first (fxPatternOfJson).
const char kNameRainbowWave[] PROGMEM = "rainbow-wave";
const char kJsonRainbowWave[] PROGMEM =
    R"({"pattern":"wave","palette":["#ff2030","#d04c00","#807000","#00bc00","#00a8a8","#3070ff","#a838ff","#ff08e0"],"interpolate":"rgb","direction":"ltr","period":18000,"phase":8})";
const char kNameNixie[] PROGMEM = "nixie";
const char kJsonNixie[] PROGMEM = R"({"pattern":"solid","palette":["#ff1800"]})";
const char kNameClassicBlue[] PROGMEM = "classic-blue";
const char kJsonClassicBlue[] PROGMEM =
    R"({"pattern":"tubes","palette":["241,100,100","241,100,100","241,100,100","241,100,100","241,100,100","241,100,100"]})";
const char kNameOcean[] PROGMEM = "ocean";
const char kJsonOcean[] PROGMEM =
    R"({"pattern":"wave","palette":[{"at":0,"color":"#101a78"},{"at":20,"color":"#3c10a8"},{"at":35,"color":"#0a48d0"},{"at":45,"color":"#00a8b8"},{"at":55,"color":"#0a48d0"},{"at":70,"color":"#2a18a0"},{"at":80,"color":"#1040c0"},{"at":90,"color":"#101a78"}],"interpolate":"rgb","direction":"ltr","period":5000,"phase":25,"pulse":{"min":170,"max":255,"period":7000}})";
const char kNameSunset[] PROGMEM = "sunset";
const char kJsonSunset[] PROGMEM =
    R"({"pattern":"solid","palette":["#ffc000","#ff8000","#ff4000","#ff0000","#ff4000","#ff8000"],"period":96000})";
const char kNameSynthwave[] PROGMEM = "synthwave";
const char kJsonSynthwave[] PROGMEM =
    R"({"pattern":"wave","palette":["#ff08e0","#00a0b0","#a838ff"],"interpolate":"rgb","direction":"rtl","period":10000,"phase":15})";
const char kNameAurora[] PROGMEM = "aurora";
const char kJsonAurora[] PROGMEM =
    R"({"pattern":"wave","palette":[{"at":0,"color":"#141458"},{"at":14,"color":"#00c050"},{"at":22,"color":"#00e0a0"},{"at":30,"color":"#2a1490"},{"at":42,"color":"#00ff66"},{"at":50,"color":"#00b0b0"},{"at":62,"color":"#1a2090"},{"at":78,"color":"#301080"}],"interpolate":"rgb","period":18000,"phase":10,"drift":90000})";
const char kNameLava[] PROGMEM = "lava";
const char kJsonLava[] PROGMEM =
    R"({"pattern":"wave","palette":[{"at":0,"color":"#500000"},{"at":22,"color":"#c01000"},{"at":32,"color":"#ff5000"},{"at":40,"color":"#ffa000"},{"at":48,"color":"#ff5000"},{"at":60,"color":"#900800"},{"at":72,"color":"#e02000"},{"at":84,"color":"#600400"}],"interpolate":"rgb","period":20000,"phase":14,"drift":75000})";
const char kNameCandy[] PROGMEM = "candy";
const char kJsonCandy[] PROGMEM = R"({"pattern":"wave","palette":["#ff6fb5","#ffffff","#4de3c1"],"direction":"ltr","period":12000})";
const char kNameIce[] PROGMEM = "ice";
const char kJsonIce[] PROGMEM = R"({"pattern":"gradient","palette":["#e0f7ff","#8ad4ff","#ffffff"],"period":0})";
const char kNameNight[] PROGMEM = "night";
const char kJsonNight[] PROGMEM = R"({"pattern":"solid","palette":["#ff8c1a"],"pulse":{"max":40,"period":0}})";
const char kNameVaporwave[] PROGMEM = "vaporwave";
const char kJsonVaporwave[] PROGMEM =
    R"({"pattern":"solid","palette":["#c05400","#d64300","#f23500","#ff1888","#ff08e0","#a838ff","#00a0b0","#a838ff"],"interpolate":"rgb","period":16000})";
const char kNameFire[] PROGMEM = "fire";
const char kJsonFire[] PROGMEM = R"({"pattern":"fire","palette":["#4a0000","#ff1800","#ff5000","#ff9000"],"interpolate":"rgb","period":6000,"density":40})";
const char kNameCandle[] PROGMEM = "candle";
const char kJsonCandle[] PROGMEM =
    R"({"pattern":"fire","palette":["#300800","#ff4000","#ff7a10"],"interpolate":"rgb","period":8000,"density":4,"accent":{"style":"flicker","ms":600}})";
const char kNameChristmas[] PROGMEM = "christmas";
const char kJsonChristmas[] PROGMEM = R"({"pattern":"strobe","palette":["#ff1000","#00d000"],"period":1600,"phase":50,"duty":50})";
const char kNameMatrix[] PROGMEM = "matrix";
const char kJsonMatrix[] PROGMEM =
    R"({"pattern":"matrix","palette":["#008000","#00ff00","#50ff00"],"interpolate":"rgb","period":1600,"density":30,"glyphs":true})";
const char kNameCyberpunk[] PROGMEM = "cyberpunk";
const char kJsonCyberpunk[] PROGMEM =
    R"({"pattern":"chase","palette":["#ff00a0","#fcee0a","#00f0ff"],"interpolate":"rgb","direction":"ltr","period":2000,"width":2,"count":1})";
const char kNameTwinkle[] PROGMEM = "twinkle";
const char kJsonTwinkle[] PROGMEM = R"({"pattern":"sparkle","palette":["#1c38b0","#ffffff","#a0c8ff"],"interpolate":"rgb","density":30})";

struct BuiltinPreset {
  const char* name;
  const char* json;
};

const BuiltinPreset kBuiltins[kBuiltinPresetCount] PROGMEM = {
    {kNameRainbowWave, kJsonRainbowWave}, {kNameNixie, kJsonNixie},   {kNameClassicBlue, kJsonClassicBlue},
    {kNameOcean, kJsonOcean},             {kNameSunset, kJsonSunset}, {kNameSynthwave, kJsonSynthwave},
    {kNameAurora, kJsonAurora},           {kNameLava, kJsonLava},     {kNameCandy, kJsonCandy},
    {kNameIce, kJsonIce},                 {kNameNight, kJsonNight},   {kNameVaporwave, kJsonVaporwave},
    {kNameFire, kJsonFire},               {kNameCandle, kJsonCandle}, {kNameChristmas, kJsonChristmas},
    {kNameMatrix, kJsonMatrix},           {kNameCyberpunk, kJsonCyberpunk}, {kNameTwinkle, kJsonTwinkle},
};

BuiltinPreset builtinAt(int index) {
  BuiltinPreset b;
  memcpy_P(&b, &kBuiltins[index], sizeof(b));
  return b;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

// ---- Effect ----

Effect Effect::defaults(FxPattern pattern) {
  Effect e;
  e.pattern = pattern;
  e.stops[0].color = {0xff, 0x18, 0x00};  // FW-FX-002 nixie neon orange
  if (pattern == FxPattern::kBreathe) {   // FW-FX-024
    e.easing = FxEasing::kSine;
    e.hasPulse = true;
    e.pulse.min = 32;
    e.pulse.max = 255;
  }
  return e;
}

bool Effect::operator==(const Effect& o) const {
  if (pattern != o.pattern || rainbow != o.rainbow || stopCount != o.stopCount || interpolate != o.interpolate ||
      direction != o.direction || period != o.period || phase != o.phase || drift != o.drift || easing != o.easing ||
      hasPulse != o.hasPulse || (hasPulse && !(pulse == o.pulse)) || density != o.density || width != o.width ||
      count != o.count || duty != o.duty || seed != o.seed || glyphs != o.glyphs || hasAccent != o.hasAccent ||
      (hasAccent && !(accent == o.accent))) {
    return false;
  }
  for (int i = 0; i < stopCount; i++) {
    if (!(stops[i] == o.stops[i])) return false;
  }
  return true;
}

void fxErrorText(FxError error, char* out, size_t cap) {
  // Walks the NUL-separated list to the entry, a byte at a time as flash wants it.
  const char* p = kFxErrors;
  for (int skip = static_cast<int>(error); skip > 0; p++) {
    if (pgm_read_byte(p) == 0) skip--;
  }
  size_t n = 0;
  while (n + 1 < cap) {
    const char c = static_cast<char>(pgm_read_byte(p + n));
    if (!c) break;
    out[n++] = c;
  }
  if (cap) out[n] = '\0';
}

FxError parseEffect(const char* json, size_t len, const Effect* base, Effect& out) {
  return parseImpl(json, len, base, out, nullptr);
}

FxError parseActiveEffect(const char* json, size_t len, Effect& out, char (&preset)[kPresetNameMax + 1]) {
  return parseImpl(json, len, nullptr, out, &preset);
}

size_t writeEffectJson(const Effect& e, char* out, size_t cap, bool withPreset, const char* preset) {
  Json j(out, cap);
  j.str("{");
  j.key("pattern", true);
  j.quoted(fxPatternName(e.pattern));
  j.key("palette");
  if (e.rainbow) {
    j.quoted("rainbow");
  } else {
    j.str("[");
    for (int i = 0; i < e.stopCount; i++) {
      char hex[8];
      formatHex(e.stops[i].color, hex);
      if (i) j.str(",");
      if (e.stops[i].at != kFxNoAt) {
        j.str("{");
        j.key("at", true);
        j.num(e.stops[i].at);
        j.key("color");
        j.quoted(hex);
        j.str("}");
      } else {
        j.quoted(hex);
      }
    }
    j.str("]");
  }
  j.key("interpolate");
  j.quoted(kInterpolateNames[static_cast<int>(e.interpolate)]);
  j.key("direction");
  j.quoted(kDirectionNames[static_cast<int>(e.direction)]);
  j.key("period");
  j.num(e.period);
  j.key("phase");
  j.num(e.phase);
  j.key("drift");
  j.num(e.drift);
  j.key("easing");
  j.quoted(kEasingNames[static_cast<int>(e.easing)]);
  j.key("pulse");
  if (e.hasPulse) {
    j.str("{");
    j.key("min", true);
    j.num(e.pulse.min);
    j.key("max");
    j.num(e.pulse.max);
    j.key("period");
    j.num(e.pulse.period);
    j.key("easing");
    j.quoted(kEasingNames[static_cast<int>(e.pulse.easing)]);
    j.str("}");
  } else {
    j.str("null");
  }
  // FW-FX-060: a pattern's own fields, only on the patterns that read them, so the older patterns'
  // JSON stays as it was.
  if (usesDensity(e.pattern)) {
    j.key("density");
    j.num(e.density);
    j.key("seed");
    j.num(e.seed);
    if (e.pattern == FxPattern::kMatrix) {
      j.key("glyphs");
      j.str(e.glyphs ? "true" : "false");
    }
  } else if (e.pattern == FxPattern::kChase) {
    j.key("width");
    j.num(e.width);
    j.key("count");
    j.num(e.count);
  } else if (e.pattern == FxPattern::kStrobe) {
    j.key("duty");
    j.num(e.duty);
  }
  j.key("accent");  // FW-FX-016: on every pattern
  if (e.hasAccent) {
    char tubes[kTubes + 1];
    formatAccentTubes(e.accent.tubes, tubes);
    j.str("{");
    j.key("style", true);
    j.quoted(kAccentStyleNames[static_cast<int>(e.accent.style)]);
    j.key("ms");
    j.num(e.accent.ms);
    j.key("color");
    if (e.accent.hasColor) {
      char hex[8];
      formatHex(e.accent.color, hex);
      j.quoted(hex);
    } else {
      j.str("null");
    }
    j.key("tubes");
    j.quoted(tubes);
    j.key("spread");
    j.num(e.accent.spread);
    j.key("strength");
    j.num(e.accent.strength);
    j.str("}");
  } else {
    j.str("null");
  }
  if (withPreset) {
    j.key("preset");
    if (preset && *preset) {
      j.quoted(preset);
    } else {
      j.str("null");
    }
  }
  j.str("}");
  return j.overflow ? 0 : j.len;
}

const char* fxPatternName(FxPattern pattern) { return kPatternNames[static_cast<int>(pattern)]; }

// ---- Rendering ----

int fxDistanceHalves(FxDirection direction, int tube) {
  const int centered = tube * 2 - 5 < 0 ? 5 - tube * 2 : tube * 2 - 5;  // 5,3,1,1,3,5
  switch (direction) {
    case FxDirection::kRtl: return 10 - tube * 2;
    case FxDirection::kCenter: return centered;
    case FxDirection::kEdges: return 6 - centered;
    default: return tube * 2;
  }
}

uint16_t fxEase(FxEasing easing, uint16_t x) {
  const uint32_t v = x;
  const uint32_t inv = 65535 - v;
  switch (easing) {
    case FxEasing::kSine: {
      const uint32_t i = v >> 10;
      const uint32_t a = pgm_read_byte(&kSineEase[i]) * 257u;
      const uint32_t b = i == 63 ? 65535u : pgm_read_byte(&kSineEase[i + 1]) * 257u;
      return static_cast<uint16_t>(a + ((b - a) * (v & 1023)) / 1024);
    }
    case FxEasing::kEaseIn: return static_cast<uint16_t>(v * v / 65535);
    case FxEasing::kEaseOut: return static_cast<uint16_t>(65535 - inv * inv / 65535);
    case FxEasing::kEaseInOut:
      return static_cast<uint16_t>(v < 32768 ? (v * v) >> 15 : 65535 - ((inv * inv) >> 15));
    case FxEasing::kStep: return v < 32768 ? 0 : 65535;
    default: return x;
  }
}

void EffectEngine::apply(const Effect& effect) {
  effect_ = effect;
  const Effect& e = effect_;
  // FW-FX-071: expanded once here, so a frame only reads the table.
  for (int i = 0; i < kFxLutEntries; i++) {
    const int x = i * 100;  // units of 1/6400
    if (e.rainbow && e.easing != FxEasing::kStep) {
      lut_[i] = fromWideHsv({i * (kHueSteps / kFxLutEntries), 255, 255});
      continue;
    }
    // The last stop at or before x; before the first stop that is the last one, a cycle ago.
    int from = e.stopCount - 1;
    for (int k = 0; k < e.stopCount; k++) {
      if (stopPosition(e, k) <= x) from = k;
    }
    if (e.easing == FxEasing::kStep || e.stopCount == 1) {  // FW-FX-007 hard switches at the stops
      lut_[i] = e.stops[from].color;
      continue;
    }
    const int to = (from + 1) % e.stopCount;
    int start = stopPosition(e, from);
    int end = stopPosition(e, to);
    if (start > x) start -= 6400;  // x comes before the first stop
    if (end <= start) end += 6400;
    lut_[i] = mix(e.stops[from].color, e.stops[to].color, (x - start) * 256 / (end - start), e.interpolate);
  }
}

Rgb EffectEngine::lookup(uint32_t pos16) const {
  const uint32_t index = (pos16 >> 10) & 63;
  const Rgb& a = lut_[index];
  if (effect_.easing == FxEasing::kStep) return a;
  const Rgb& b = lut_[(index + 1) & 63];
  const int f = (pos16 >> 2) & 255;
  auto ch = [f](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (static_cast<int>(y) - x) * f / 256); };
  return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b)};
}

Rgb EffectEngine::eased(uint16_t pos16) const {
  // A stepped palette is flat in the table, so the position goes in as it is.
  return lookup(effect_.easing == FxEasing::kStep ? pos16 : fxEase(effect_.easing, pos16));
}

namespace {

// FW-FX-015: a tube's offset on the way between rtl (sway -1), center (0) and ltr (+1). The two rates
// keep the rhythm uneven, and the blend moves smoothly through center, so the colors never jump.
uint32_t driftOffset(int tube, uint8_t phase, uint32_t driftCycle) {
  const auto sway = [](uint32_t pos16) -> int32_t { return fxEase(FxEasing::kSine, triangle(pos16 & 0xFFFF)); };
  const int32_t s = (3 * sway(driftCycle) + sway(driftCycle * 3)) / 2 - 65535;  // -65535..65535
  const int32_t side = tube * 2 - 5;                                            // -5,-3,-1,1,3,5 half units
  const int32_t centered = side < 0 ? -side : side;
  const int32_t blend = side * s + centered * (65535 - (s < 0 ? -s : s));
  return static_cast<uint32_t>(blend * static_cast<int32_t>(phase) / 200);
}

// FW-FX-012, FW-FX-029: the "random" of sparkle and fire is a hash of the seed, the tube and a time
// slot, so a frame stays a pure function of (effect, tube, t).
uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t x = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// Smooth value noise, 0..65535: a random level every `spacingMs`, eased from one to the next.
uint32_t valueNoise(uint32_t seed, uint32_t lane, uint64_t tMs, uint32_t spacingMs) {
  const uint32_t k = static_cast<uint32_t>(tMs / spacingMs);
  const uint32_t frac = static_cast<uint32_t>((tMs % spacingMs) * 65535u / spacingMs);
  const int32_t a = static_cast<int32_t>(hash3(seed, lane, k) & 0xFFFF);
  const int32_t b = static_cast<int32_t>(hash3(seed, lane, k + 1) & 0xFFFF);
  const int64_t s = fxEase(FxEasing::kSine, static_cast<uint16_t>(frac));
  return static_cast<uint32_t>(a + (b - a) * s / 65536);
}

constexpr Rgb kOff{0, 0, 0};
constexpr uint32_t kSparkleSlotMs = 400;   // FW-FX-025: at most one flash per tube in each slot
constexpr uint32_t kSparkleFlashMs = 250;  // a flash's fade back to the base
constexpr int kChaseTailUnits = 384;       // FW-FX-026: a runner's fading tail, 1.5 tubes

// `f` of 0..256 of the way from a to b.
Rgb blend256(const Rgb& a, const Rgb& b, int f, FxInterpolate how) { return f >= 256 ? b : mix(a, b, f, how); }

}  // namespace

// FW-FX-025: with one color the base is off and the flashes are that color.
Rgb EffectEngine::sparkle(int tube, uint64_t tMs) const {
  const Effect& e = effect_;
  const Rgb base = e.stopCount > 1 ? e.stops[0].color : kOff;
  // Each tube's slots start at their own time, so flashes don't line up across the clock.
  const uint64_t shifted = tMs + hash3(e.seed, static_cast<uint32_t>(tube), 0xFFFFFFFFu) % kSparkleSlotMs;
  const uint32_t slot = static_cast<uint32_t>(shifted / kSparkleSlotMs);
  const uint32_t within = static_cast<uint32_t>(shifted % kSparkleSlotMs);
  const uint32_t h = hash3(e.seed, static_cast<uint32_t>(tube), slot);
  if ((h & 0xFF) >= e.density * 256u / 100u) return base;  // density 100 flashes in every slot
  const uint32_t start = (h >> 8) % (kSparkleSlotMs - kSparkleFlashMs + 1);
  if (within < start || within >= start + kSparkleFlashMs) return base;
  const Rgb flash = e.stopCount > 1 ? e.stops[1 + (h >> 20) % (e.stopCount - 1)].color : e.stops[0].color;
  const int left = static_cast<int>(kSparkleFlashMs - (within - start)) * 256 / static_cast<int>(kSparkleFlashMs);
  return blend256(base, flash, left * left / 256, e.interpolate);  // bright at once, then a quick fade
}

// FW-FX-026: `count` runners, evenly spaced, one lap per period. Each fades in over half a tube as it
// arrives, holds for `width` tubes and leaves a short tail. With one color the base is off.
Rgb EffectEngine::chase(int tube, uint32_t cycle) const {
  const Effect& e = effect_;
  const bool mirrored = e.direction == FxDirection::kCenter || e.direction == FxDirection::kEdges;
  const int d = fxDistanceHalves(e.direction, tube);
  const int track = (mirrored ? 3 : kTubes) * 256;  // in 1/256 of a tube
  const int x = (mirrored ? d - 1 : d) * 128;
  const int head0 = static_cast<int>(static_cast<uint64_t>(cycle) * track / 65536u);
  const int held = e.width * 256;
  int best = 0;
  int bestRunner = 0;
  for (int k = 0; k < e.count; k++) {
    const int head = (head0 + k * track / e.count) % track;
    const int behind = (head - x + track) % track;
    // Fades in over the first half tube, holds for `width` tubes, then the tail.
    int f = 0;
    if (behind < 128) {
      f = behind * 2;
    } else if (behind < 128 + held) {
      f = 256;
    } else if (behind < 128 + held + kChaseTailUnits) {
      f = (128 + held + kChaseTailUnits - behind) * 256 / kChaseTailUnits;
    }
    if (f > best) {
      best = f;
      bestRunner = k;
    }
  }
  const Rgb base = e.stopCount > 1 ? e.stops[0].color : kOff;
  const Rgb runner = e.stopCount > 1 ? e.stops[1 + bestRunner % (e.stopCount - 1)].color : e.stops[0].color;
  return blend256(base, runner, best, e.interpolate);
}

// FW-FX-027: each tube wanders on its own: a fast flicker at `density`, plus a slow drift over `period`.
// The palette is read from its first stop to its last without wrapping, so a dark-to-bright palette
// never jumps from brightest to darkest. With one color it flickers between off and that color.
Rgb EffectEngine::fire(int tube, uint64_t tMs) const {
  const Effect& e = effect_;
  uint32_t spacing = 4000u / e.density;
  if (spacing < 50) spacing = 50;  // one frame
  uint32_t n = valueNoise(e.seed, static_cast<uint32_t>(tube) * 2, tMs, spacing);
  if (e.period) n = (n + valueNoise(e.seed, static_cast<uint32_t>(tube) * 2 + 1, tMs, e.period)) / 2;
  if (e.stopCount == 1 && !e.rainbow) return blend256(kOff, e.stops[0].color, static_cast<int>(n >> 8), FxInterpolate::kRgb);
  const uint32_t last = e.rainbow ? 6400u : static_cast<uint32_t>(stopPosition(e, e.stopCount - 1));
  return lookup(n * last / 6400u);
}

// FW-FX-028: the first color for `duty` percent of each period, then the second (or off). `phase` and
// `direction` offset the tubes as a wave's are.
Rgb EffectEngine::strobe(int tube, uint32_t cycle) const {
  const Effect& e = effect_;
  const uint32_t offset = static_cast<uint32_t>(fxDistanceHalves(e.direction, tube)) * e.phase * 65536u / 200u;
  const uint32_t pos = (cycle - offset) & 0xFFFF;
  if (pos < e.duty * 65536u / 100u) return e.stops[0].color;
  return e.stopCount > 1 ? e.stops[1].color : kOff;
}

// FW-FX-017: each tube is a column of rain. In each `period` slot a drop falls with a chance of
// `density` percent, taking two thirds of the slot: a bright head, then a long tail fading to the
// background. Each tube's slots start at their own time.
bool EffectEngine::matrixDrop(int tube, uint64_t tMs, uint32_t& progress) const {
  const Effect& e = effect_;
  const uint32_t slotMs = e.period ? e.period : 1500;
  const uint64_t shifted = tMs + hash3(e.seed ^ 0x3A7u, static_cast<uint32_t>(tube), 0xFFFFFFFFu) % slotMs;
  const uint32_t slot = static_cast<uint32_t>(shifted / slotMs);
  const uint32_t within = static_cast<uint32_t>(shifted % slotMs);
  const uint32_t h = hash3(e.seed ^ 0x3A7u, static_cast<uint32_t>(tube), slot);
  if ((h & 0xFF) >= e.density * 256u / 100u) return false;
  const uint32_t dropMs = slotMs * 2 / 3;
  const uint32_t start = (h >> 8) % (slotMs - dropMs + 1);
  if (within < start || within >= start + dropMs) return false;
  progress = static_cast<uint32_t>(static_cast<uint64_t>(within - start) * 65536u / dropMs);
  return true;
}

// Palette: background, tail, head (the last stop). Two colors: no separate head. One: the background is off.
Rgb EffectEngine::matrix(int tube, uint64_t tMs) const {
  const Effect& e = effect_;
  const Rgb bg = e.stopCount > 1 ? e.stops[0].color : kOff;
  const Rgb tail = e.stopCount > 1 ? e.stops[1].color : e.stops[0].color;
  const Rgb head = e.stops[e.stopCount - 1].color;
  uint32_t p = 0;
  if (!matrixDrop(tube, tMs, p)) return bg;
  constexpr uint32_t kHead = 8192;  // the first eighth of the drop
  if (p < kHead) return head;
  const uint32_t q = (p - kHead) * 256u / (65536u - kHead);  // 0..255 through the tail
  if (q < 48) return blend256(head, tail, static_cast<int>(q * 256 / 48), e.interpolate);
  const int f = static_cast<int>((q - 48) * 256 / 208);
  return blend256(tail, bg, f * f / 256, e.interpolate);  // stays bright, then goes
}

int8_t EffectEngine::glyph(int tube, uint64_t tMs) const {
  const Effect& e = effect_;
  if (e.pattern != FxPattern::kMatrix || !e.glyphs) return kNoGlyph;
  uint32_t p = 0;
  if (!matrixDrop(tube, tMs, p) || p >= 21000) return kNoGlyph;  // the head and the start of the tail
  return static_cast<int8_t>(hash3(e.seed, static_cast<uint32_t>(tube), static_cast<uint32_t>(tMs / 60)) % 10);
}

// FW-FX-016. `p` runs 0..65535 through the accent; the strength scales how far it goes.
Rgb EffectEngine::accented(int tube, uint64_t tMs, const Rgb& base, uint32_t ageMs) const {
  const Effect& e = effect_;
  const FxAccent& a = e.accent;
  const uint32_t p = ageMs >= a.ms ? 65535u : ageMs * 65535u / a.ms;
  const uint32_t fade = 65535u - p;                                  // 1 at the change, 0 at the end
  const uint32_t bell = fxEase(FxEasing::kSine, triangle(p));         // up and back down
  const Rgb color = a.hasColor ? a.color : e.stops[e.stopCount - 1].color;
  auto scaled = [](const Rgb& c, int level256) {
    return Rgb{static_cast<uint8_t>(c.r * level256 / 256), static_cast<uint8_t>(c.g * level256 / 256),
               static_cast<uint8_t>(c.b * level256 / 256)};
  };
  auto amount = [&a](uint32_t x) { return static_cast<int>(x * a.strength / 100u / 256u); };  // 0..255
  switch (a.style) {
    case FxAccentStyle::kFlicker: {
      // A new random level every 40 ms: sometimes a dip toward dark, sometimes a lift toward the
      // accent color, wilder at the change and calmer as it ends.
      const uint32_t r = hash3(0xF11C4u ^ e.seed, static_cast<uint32_t>(tube), static_cast<uint32_t>(tMs / 40));
      const int depth = amount(fade);
      const int roll = static_cast<int>((r >> 8) & 0xFF);
      if (r & 1) return blend256(base, color, depth * roll / 512, FxInterpolate::kRgb);
      return scaled(base, 256 - depth * roll / 256);
    }
    case FxAccentStyle::kFlash: {
      const uint32_t quick = fade * fade / 65535u;  // bright at once, then a fast fade
      return blend256(base, color, amount(quick), FxInterpolate::kRgb);
    }
    case FxAccentStyle::kDip: return scaled(base, 256 - amount(fxEase(FxEasing::kSine, static_cast<uint16_t>(fade))));
    case FxAccentStyle::kGlow: return blend256(base, color, amount(bell), FxInterpolate::kRgb);
    default: {  // kShift: along the palette and back, up to half a cycle
      const uint32_t half = bell * a.strength / 100u / 2u;  // 0..32767 of a cycle
      if (e.period) return tubeColor(tube, tMs + static_cast<uint64_t>(half) * e.period / 65536u);
      return eased(static_cast<uint16_t>(half));
    }
  }
}

Rgb EffectEngine::tubeColor(int tube, uint64_t tMs) const {
  const Effect& e = effect_;
  const uint32_t cycle = cycle16(tMs, e.period);
  switch (e.pattern) {
    case FxPattern::kSolid:  // FW-FX-020
      return e.period ? eased(static_cast<uint16_t>(cycle)) : e.stops[0].color;
    case FxPattern::kTubes:  // FW-FX-021
      return e.stops[tube % e.stopCount].color;
    case FxPattern::kWave: {  // FW-FX-022, FW-FX-006
      if (e.drift) return eased(static_cast<uint16_t>(cycle - driftOffset(tube, e.phase, cycle16(tMs, e.drift))));
      const uint32_t offset = static_cast<uint32_t>(fxDistanceHalves(e.direction, tube)) * e.phase * 65536u / 200u;
      return eased(static_cast<uint16_t>(cycle - offset));
    }
    case FxPattern::kGradient: {  // FW-FX-023: one palette length across the tubes
      const int d = fxDistanceHalves(e.direction, tube);
      const bool mirrored = e.direction == FxDirection::kCenter || e.direction == FxDirection::kEdges;
      const uint32_t spread = mirrored ? static_cast<uint32_t>(d - 1) * 65536u / 6u : static_cast<uint32_t>(d) * 65536u / 12u;
      return eased(static_cast<uint16_t>(spread - cycle));
    }
    case FxPattern::kSparkle: return sparkle(tube, tMs);
    case FxPattern::kChase: return chase(tube, cycle);
    case FxPattern::kFire: return fire(tube, tMs);
    case FxPattern::kStrobe: return strobe(tube, cycle);
    case FxPattern::kMatrix: return matrix(tube, tMs);
    default:  // FW-FX-024: the color moves on in the dark, between breaths
      return e.period ? e.stops[(tMs / e.period) % e.stopCount].color : e.stops[0].color;
  }
}

uint8_t EffectEngine::level(uint64_t tMs) const {
  const Effect& e = effect_;
  if (e.pattern == FxPattern::kBreathe) {
    if (!e.period) return e.pulse.max;
    return envelope(e.pulse.min, e.pulse.max, e.easing, cycle16(tMs, e.period));
  }
  if (!e.hasPulse) return 255;
  if (!e.pulse.period) return e.pulse.max;
  return envelope(e.pulse.min, e.pulse.max, e.pulse.easing, cycle16(tMs, e.pulse.period));
}

// ---- Display options ----

namespace {
const char* const kTransitionNames[] = {"cut", "crossfade", "scramble", "carry"};
}  // namespace

const char* fxTransitionName(FxTransition transition) { return kTransitionNames[static_cast<int>(transition)]; }

bool parseFxTransition(const char* text, FxTransition& out) {
  const int index = text ? nameIndex(kTransitionNames, text) : -1;
  if (index < 0) return false;
  out = static_cast<FxTransition>(index);
  return true;
}

size_t writeDisplayJson(const DisplayOptions& options, int hours, char* out, size_t cap) {
  Json j(out, cap);
  j.str("{");
  if (hours) {
    j.key("hours", true);
    j.num(static_cast<unsigned long>(hours));
    j.str(",");
  }
  j.key("leadingZero", true);
  j.str(options.leadingZero ? "true" : "false");
  j.key("seconds");
  j.str(options.seconds ? "true" : "false");
  j.key("transition");
  j.quoted(fxTransitionName(options.transition));
  j.key("fadeMs");
  j.num(options.fadeMs);
  j.key("tubeBrightness");
  j.str("[");
  for (int t = 0; t < kTubes; t++) {
    if (t) j.str(",");
    j.num(options.tubeBrightness[t]);
  }
  j.str("]");
  j.str("}");
  return j.overflow ? 0 : j.len;
}

bool parseTubeBrightness(const char* text, std::array<uint8_t, kTubes>& out) {
  if (!text) return false;
  std::array<uint8_t, kTubes> levels{};
  const char* p = text;
  for (int t = 0; t < kTubes; t++) {
    if (*p < '0' || *p > '9') return false;
    unsigned v = 0;
    for (int digits = 0; *p >= '0' && *p <= '9'; p++) {
      if (++digits > 3) return false;
      v = v * 10 + static_cast<unsigned>(*p - '0');
    }
    if (v > 100) return false;
    levels[t] = static_cast<uint8_t>(v);
    if (t + 1 < kTubes && *p++ != ',') return false;
  }
  if (*p) return false;
  out = levels;
  return true;
}

bool parseDisplayJson(const char* json, size_t len, DisplayOptions& out) {
  uint8_t buf[kFxArenaBytes / 4];
  FixedArena arena(buf, sizeof(buf));
  JsonDocument doc(&arena);
  if (deserializeJson(doc, json, len) || !doc.is<JsonObjectConst>()) return false;
  JsonVariantConst leadingZero = doc["leadingZero"], seconds = doc["seconds"], transition = doc["transition"], fade = doc["fadeMs"];
  DisplayOptions o;
  if (!leadingZero.is<bool>() || !seconds.is<bool>() || !transition.is<const char*>() || !fade.is<uint16_t>()) return false;
  if (!parseFxTransition(transition.as<const char*>(), o.transition)) return false;
  o.leadingZero = leadingZero.as<bool>();
  o.seconds = seconds.as<bool>();
  o.fadeMs = fade.as<uint16_t>();
  if (o.fadeMs < kFadeMinMs || o.fadeMs > kFadeMaxMs) return false;
  // FW-FX-034: a file written before per-tube brightness existed has none, and keeps all at 100.
  JsonVariantConst levels = doc["tubeBrightness"];
  if (!levels.isUnbound()) {
    if (!levels.is<JsonArrayConst>() || levels.as<JsonArrayConst>().size() != kTubes) return false;
    int t = 0;
    for (JsonVariantConst level : levels.as<JsonArrayConst>()) {
      if (!level.is<uint8_t>() || level.as<uint8_t>() > 100) return false;
      o.tubeBrightness[t++] = level.as<uint8_t>();
    }
  }
  out = o;
  return true;
}

void applyDisplayOptions(std::array<int8_t, kTubes>& digits, const DisplayOptions& options, bool attentionActive) {
  if (attentionActive) return;
  if (!options.leadingZero && digits[0] == 0) digits[0] = kBlank;  // FW-FX-031
  if (!options.seconds) digits[4] = digits[5] = kBlank;            // FW-FX-032
}

void DigitFader::update(const std::array<int8_t, kTubes>& digits, uint32_t nowMs, const DisplayOptions& options) {
  const bool fades = options.transition == FxTransition::kCrossfade && started_;
  for (int t = 0; t < kTubes; t++) {
    if (!started_) {
      digit_[t] = prev_[t] = digits[t];
      from_[t] = kBlank;
      mix_[t] = 255;
    } else if (digits[t] != digit_[t]) {
      from_[t] = prev_[t] = digit_[t];
      digit_[t] = digits[t];
      startMs_[t] = nowMs;
      changed_[t] = digits[t] != kBlank;  // FW-FX-016 going blank is no change to accent
      mix_[t] = fades ? 0 : 255;
    } else if (mix_[t] != 255) {
      const uint32_t elapsed = nowMs - startMs_[t];
      mix_[t] = !fades || elapsed >= options.fadeMs ? 255 : static_cast<uint8_t>(elapsed * 255 / options.fadeMs);
    }
    if (mix_[t] == 255) from_[t] = kBlank;

    // FW-FX-033 scramble and carry: one digit at a time, on the way to the new one, for fadeMs.
    shown_[t] = digit_[t];
    const uint32_t elapsed = nowMs - startMs_[t];
    const bool moving = started_ && digit_[t] != kBlank && prev_[t] != kBlank && prev_[t] != digit_[t] && elapsed < options.fadeMs;
    if (moving && options.transition == FxTransition::kScramble) {
      shown_[t] = static_cast<int8_t>(hash3(startMs_[t], static_cast<uint32_t>(t), elapsed / 50) % 10);
    } else if (moving && options.transition == FxTransition::kCarry) {
      // Counts up from the old digit like an odometer, 9 rolling over to 0.
      const uint32_t steps = static_cast<uint32_t>(digit_[t] - prev_[t] + 10) % 10;
      const uint32_t k = elapsed * steps / options.fadeMs;
      shown_[t] = static_cast<int8_t>((prev_[t] + 1 + k) % 10);
    }
  }
  started_ = true;
}

void DigitFader::fill(std::array<TubeState, kTubes>& tubes) const {
  for (int t = 0; t < kTubes; t++) {
    tubes[t].digit = shown_[t];
    tubes[t].fromDigit = from_[t];
    tubes[t].mix = mix_[t];
  }
}

// ---- One frame ----

Frame composeFrame(const LedLayout& layout, const EffectEngine& engine, const Attention& attention, DigitFader& fader,
                   const DisplayOptions& options, std::array<int8_t, kTubes> digits, uint64_t tMs, uint32_t nowMs,
                   uint8_t brightness, FrameInfo& info) {
  info.attention = attention.isActive(nowMs);
  // The digits first, so the accent knows which tubes just changed.
  applyDisplayOptions(digits, options, info.attention);
  fader.update(digits, nowMs, options);

  const Effect& fx = engine.effect();
  for (int t = 0; t < kTubes; t++) {
    info.colors[t] = engine.tubeColor(t, tMs);
    if (!fx.hasAccent || info.attention) continue;
    // FW-FX-016: the most recent change among the tubes that trigger, reaching this tube
    // `spread` ms per tube later.
    uint32_t best = fx.accent.ms;
    for (int src = 0; src < kTubes; src++) {
      uint32_t atMs = 0;
      if (!((fx.accent.tubes >> src) & 1) || !fader.lastChange(src, atMs)) continue;
      const uint32_t apart = static_cast<uint32_t>(src > t ? src - t : t - src);
      if (apart && !fx.accent.spread) continue;
      const uint32_t elapsed = nowMs - atMs;
      const uint32_t delay = apart * fx.accent.spread;
      if (elapsed < delay || elapsed - delay >= best) continue;
      best = elapsed - delay;
    }
    if (best < fx.accent.ms) info.colors[t] = engine.accented(t, tMs, info.colors[t], best);
  }
  const bool blinkLit = attention.apply(info.colors, nowMs);
  // FW-FX-034: after the effect and any flash, so a tamed tube stays tamed during a flash too.
  for (int t = 0; t < kTubes; t++) {
    const unsigned pct = options.tubeBrightness[t];
    if (pct >= 100) continue;
    Rgb& c = info.colors[t];
    c = {static_cast<uint8_t>(c.r * pct / 100), static_cast<uint8_t>(c.g * pct / 100), static_cast<uint8_t>(c.b * pct / 100)};
  }
  // FW-FX-036 pulse only scales down; FW-ATT-002 a flash shows at the saved brightness.
  info.brightness = info.attention ? brightness : static_cast<uint8_t>((brightness * (engine.level(tMs) + 1u)) >> 8);

  std::array<TubeState, kTubes> tubes{};
  fader.fill(tubes);
  info.lit = false;
  for (int t = 0; t < kTubes; t++) {
    tubes[t].color = info.colors[t];
    info.digits[t] = fader.digit(t);
    // FW-FX-017: a passing drop shows random digits, but never lights a hidden tube or covers a flash.
    if (!info.attention && info.digits[t] != kBlank) {
      const int8_t g = engine.glyph(t, tMs);
      if (g != kNoGlyph) {
        tubes[t].digit = g;
        tubes[t].fromDigit = kBlank;
        tubes[t].mix = 255;
      }
    }
    if (!blinkLit) {
      tubes[t].digit = tubes[t].fromDigit = kBlank;  // FW-ATT-003 unlit means off, not dimmed
      info.digits[t] = kBlank;
    }
    info.shown[t] = tubes[t].digit;
    info.lit = info.lit || tubes[t].digit != kBlank;
  }
  return render(layout, tubes, info.brightness);
}

// ---- Presets ----

bool isValidPresetName(const char* name) {
  if (!name || !*name) return false;
  size_t n = 0;
  for (const char* p = name; *p; p++, n++) {
    const char c = *p;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
    if (!ok || n >= kPresetNameMax) return false;
  }
  return true;
}

bool presetNamesEqual(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    if (lower(*a) != lower(*b)) return false;
  }
  return *a == *b;
}

int findBuiltinPreset(const char* name) {
  char candidate[kPresetNameMax + 1];
  for (int i = 0; i < kBuiltinPresetCount; i++) {
    builtinPresetName(i, candidate);
    if (presetNamesEqual(candidate, name)) return i;
  }
  return -1;
}

void builtinPresetName(int index, char (&out)[kPresetNameMax + 1]) {
  const char* name = builtinAt(index).name;
  const size_t n = strlen_P(name);
  memcpy_P(out, name, n + 1);
}

size_t builtinPresetJson(int index, char* out, size_t cap) {
  const char* json = builtinAt(index).json;
  const size_t n = strlen_P(json);
  if (n >= cap) return 0;
  memcpy_P(out, json, n + 1);
  return n;
}

bool loadBuiltinPreset(int index, Effect& out) {
  char json[512];  // FW-FX-041
  const size_t n = builtinPresetJson(index, json, sizeof(json));
  return n && parseEffect(json, n, nullptr, out) == FxError::kNone;
}

void fxPatternOfJson(const char* json, char (&out)[12]) {
  static const char kPrefix[] = "{\"pattern\":\"";
  out[0] = '\0';
  if (strncmp(json, kPrefix, sizeof(kPrefix) - 1) != 0) return;
  const char* p = json + sizeof(kPrefix) - 1;
  size_t n = 0;
  while (p[n] && p[n] != '"' && n + 1 < sizeof(out)) n++;
  if (p[n] != '"') return;
  memcpy(out, p, n);
  out[n] = '\0';
}

bool splitPresetLine(char* line, const char*& name, const char*& json) {
  char* tab = strchr(line, '\t');
  if (!tab) return false;
  *tab = '\0';
  name = line;
  json = tab + 1;
  return isValidPresetName(name);
}

bool isJsonContentType(const char* value, size_t len) {
  static const char kJson[] = "application/json";
  const size_t n = sizeof(kJson) - 1;
  if (len < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (lower(value[i]) != kJson[i]) return false;
  }
  return len == n || value[n] == ';' || value[n] == ' ';
}

}  // namespace clock_core
