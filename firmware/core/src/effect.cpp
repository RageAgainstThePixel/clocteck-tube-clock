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
    "unknown field (allowed: pattern, palette, interpolate, direction, period, phase, drift, easing, pulse)\0"
    "pattern is required\0"
    "pattern: need solid, tubes, wave, gradient or breathe\0"
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
    "pulse: min must not exceed max\0";

const char* const kPatternNames[] = {"solid", "tubes", "wave", "gradient", "breathe"};
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
  // In v1 only `breathe` has fields of its own: its sine swing (FW-FX-024).
  const Effect fresh = Effect::defaults(pattern);
  if (pattern == FxPattern::kBreathe || e.pattern == FxPattern::kBreathe) {
    e.easing = fresh.easing;
    e.hasPulse = fresh.hasPulse;
    e.pulse = fresh.pulse;
  }
  e.pattern = pattern;
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

  // FW-FX-013: unknown fields are errors, so typos in presets are caught. The v1.1 fields (density,
  // width, count, duty, seed) are unknown until their patterns exist.
  for (JsonPairConst kv : root) {
    const bool known = kv.key() == "pattern" || kv.key() == "palette" || kv.key() == "interpolate" ||
                       kv.key() == "direction" || kv.key() == "period" || kv.key() == "phase" ||
                       kv.key() == "drift" || kv.key() == "easing" || kv.key() == "pulse" || (preset && kv.key() == "preset");
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

struct BuiltinPreset {
  const char* name;
  const char* json;
};

const BuiltinPreset kBuiltins[kBuiltinPresetCount] PROGMEM = {
    {kNameRainbowWave, kJsonRainbowWave}, {kNameNixie, kJsonNixie},   {kNameClassicBlue, kJsonClassicBlue},
    {kNameOcean, kJsonOcean},             {kNameSunset, kJsonSunset}, {kNameSynthwave, kJsonSynthwave},
    {kNameAurora, kJsonAurora},           {kNameLava, kJsonLava},     {kNameCandy, kJsonCandy},
    {kNameIce, kJsonIce},                 {kNameNight, kJsonNight},   {kNameVaporwave, kJsonVaporwave},
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
      hasPulse != o.hasPulse || (hasPulse && !(pulse == o.pulse))) {
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

}  // namespace

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

const char* fxTransitionName(FxTransition transition) { return transition == FxTransition::kCrossfade ? "crossfade" : "cut"; }

bool parseFxTransition(const char* text, FxTransition& out) {
  if (strcmp(text, "cut") == 0) {
    out = FxTransition::kCut;
  } else if (strcmp(text, "crossfade") == 0) {
    out = FxTransition::kCrossfade;
  } else {
    return false;
  }
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
  j.str("}");
  return j.overflow ? 0 : j.len;
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
      digit_[t] = digits[t];
      from_[t] = kBlank;
      mix_[t] = 255;
    } else if (digits[t] != digit_[t]) {
      from_[t] = digit_[t];
      digit_[t] = digits[t];
      startMs_[t] = nowMs;
      mix_[t] = fades ? 0 : 255;
    } else if (mix_[t] != 255) {
      const uint32_t elapsed = nowMs - startMs_[t];
      mix_[t] = !fades || elapsed >= options.fadeMs ? 255 : static_cast<uint8_t>(elapsed * 255 / options.fadeMs);
    }
    if (mix_[t] == 255) from_[t] = kBlank;
  }
  started_ = true;
}

void DigitFader::fill(std::array<TubeState, kTubes>& tubes) const {
  for (int t = 0; t < kTubes; t++) {
    tubes[t].digit = digit_[t];
    tubes[t].fromDigit = from_[t];
    tubes[t].mix = mix_[t];
  }
}

// ---- One frame ----

Frame composeFrame(const LedLayout& layout, const EffectEngine& engine, const Attention& attention, DigitFader& fader,
                   const DisplayOptions& options, std::array<int8_t, kTubes> digits, uint64_t tMs, uint32_t nowMs,
                   uint8_t brightness, FrameInfo& info) {
  for (int t = 0; t < kTubes; t++) info.colors[t] = engine.tubeColor(t, tMs);
  info.attention = attention.isActive(nowMs);
  const bool blinkLit = attention.apply(info.colors, nowMs);
  // FW-FX-036 pulse only scales down; FW-ATT-002 a flash shows at the saved brightness.
  info.brightness = info.attention ? brightness : static_cast<uint8_t>((brightness * (engine.level(tMs) + 1u)) >> 8);

  applyDisplayOptions(digits, options, info.attention);
  fader.update(digits, nowMs, options);

  std::array<TubeState, kTubes> tubes{};
  fader.fill(tubes);
  info.lit = false;
  for (int t = 0; t < kTubes; t++) {
    tubes[t].color = info.colors[t];
    if (!blinkLit) tubes[t].digit = tubes[t].fromDigit = kBlank;  // FW-ATT-003 unlit means off, not dimmed
    info.digits[t] = tubes[t].digit;
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
