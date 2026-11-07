#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "clock_core/attention.h"
#include "clock_core/color.h"
#include "clock_core/display.h"

namespace clock_core {

// The effects engine (spec/firmware/effects.md, FW-FX). Everything the tubes show is data: an Effect
// is parsed once from JSON, expanded once into a palette lookup, and every frame is then a pure
// function of (effect, tube, time) in whole-number math (FW-FX-029, FW-FX-071).

constexpr int kFxMaxStops = 8;
constexpr int kFxLutEntries = 64;
constexpr size_t kFxMaxBodyBytes = 1024;  // FW-FX-014, FW-BUD-004
// FW-FX-073: the parse document, freed after apply: 2 KB on the device. JSON slots are twice the size
// on a 64-bit host, so the tests get the same capacity rather than the same bytes.
constexpr size_t kFxArenaBytes = 2048 * (sizeof(void*) / 4);
constexpr size_t kFxJsonBytes = 640;      // the longest effect writeEffectJson can produce
constexpr uint8_t kFxNoAt = 255;          // a palette stop without an explicit `at`
constexpr size_t kPresetNameMax = 24;     // FW-FX-040
constexpr int kMaxUserPresets = 16;       // FW-FX-042
/// One stored preset: name, a tab, the effect, a newline and the NUL (FW-FX-042).
constexpr size_t kPresetLineBytes = kPresetNameMax + 1 + kFxJsonBytes + 2;
/// The time between the last change and /effect.json being written (FW-FX-051).
constexpr uint32_t kFxSaveDebounceMs = 2000;

enum class FxPattern : uint8_t { kSolid, kTubes, kWave, kGradient, kBreathe };
enum class FxInterpolate : uint8_t { kHsv, kRgb };
enum class FxDirection : uint8_t { kLtr, kRtl, kCenter, kEdges };
enum class FxEasing : uint8_t { kLinear, kSine, kEaseIn, kEaseOut, kEaseInOut, kStep };

struct FxStop {
  Rgb color;
  uint8_t at = kFxNoAt;  // 0-100, or kFxNoAt for evenly spaced

  bool operator==(const FxStop& o) const { return color == o.color && at == o.at; }
};

/// FW-FX-008. `period` 0 holds the level at `max`.
struct FxPulse {
  uint8_t min = 0;
  uint8_t max = 255;
  uint32_t period = 4000;
  FxEasing easing = FxEasing::kSine;

  bool operator==(const FxPulse& o) const {
    return min == o.min && max == o.max && period == o.period && easing == o.easing;
  }
};

/// One effect, every field filled in (FW-FX-001..008).
struct Effect {
  FxPattern pattern = FxPattern::kWave;
  bool rainbow = false;  // palette "rainbow"; `stops` then holds the six primary hues
  uint8_t stopCount = 1;
  FxStop stops[kFxMaxStops];
  FxInterpolate interpolate = FxInterpolate::kHsv;
  FxDirection direction = FxDirection::kLtr;
  uint32_t period = 18000;
  uint8_t phase = 8;
  uint32_t drift = 0;  // FW-FX-015: ms per sway of a wave's direction, 0 for none
  FxEasing easing = FxEasing::kLinear;
  bool hasPulse = false;
  FxPulse pulse;

  /// The defaults for a pattern: nixie amber, and for `breathe` a sine swing over 32..255 (FW-FX-024).
  static Effect defaults(FxPattern pattern);

  bool operator==(const Effect& o) const;
};

// ---- JSON (FW-FX-013, FW-FX-014) ----

enum class FxError : uint8_t {
  kNone,
  kTooLarge,
  kBadJson,
  kUnknownField,
  kPatternMissing,
  kPattern,
  kPalette,
  kPaletteSize,
  kColor,
  kStop,
  kStopOrder,
  kInterpolate,
  kDirection,
  kPeriod,
  kPhase,
  kDrift,
  kEasing,
  kPulse,
  kPulseMinMax,
};

/// The message for a 400 answer. It names the field and the valid form and never echoes the request
/// (FW-API-002). Kept in flash; `out` of 80 bytes always fits.
void fxErrorText(FxError error, char* out, size_t cap);

/// Parses an effect. With `base` the body is partial: what it leaves out keeps base's value, and a
/// pattern switch resets the pattern's own fields (FW-FX-061). Without `base`, `pattern` is required
/// and the rest takes its default (FW-FX-001). `out` is only written on success.
FxError parseEffect(const char* json, size_t len, const Effect* base, Effect& out);

/// `/effect.json`: an effect as writeEffectJson wrote it, including `preset` (FW-FX-050).
FxError parseActiveEffect(const char* json, size_t len, Effect& out, char (&preset)[kPresetNameMax + 1]);

/// The effect with every field filled in; `preset` is left out when `withPreset` is false, and an
/// empty or null `preset` is written as null. Returns the length, 0 if it does not fit.
size_t writeEffectJson(const Effect& effect, char* out, size_t cap, bool withPreset, const char* preset);

const char* fxPatternName(FxPattern pattern);

// ---- Rendering (FW-FX-020..024, FW-FX-070, FW-FX-071) ----

/// A tube's distance d(i) in half units, so the 0.5 steps of `center` and `edges` stay whole (FW-FX-004).
int fxDistanceHalves(FxDirection direction, int tube);

/// An easing on 0..65535 (FW-FX-007). `step` here is the switch at the halfway point, for envelopes;
/// a palette's `step` is built into its lookup instead (stop boundaries, not the halfway point).
uint16_t fxEase(FxEasing easing, uint16_t x);

/// millis() extended past its 49-day wrap, since a period rarely divides 2^32: without this a wave
/// would jump at the wrap (VER-019).
class EffectClock {
 public:
  uint64_t advance(uint32_t nowMs) {
    tMs_ += static_cast<uint32_t>(nowMs - lastMs_);
    lastMs_ = nowMs;
    return tMs_;
  }

 private:
  uint64_t tMs_ = 0;
  uint32_t lastMs_ = 0;
};

/// The active effect and its palette lookup, expanded once per apply() (FW-FX-071).
class EffectEngine {
 public:
  void apply(const Effect& effect);
  const Effect& effect() const { return effect_; }

  /// The pattern's color for one tube at time `tMs`. A pure function of those and the effect; it
  /// neither allocates nor uses floats (FW-FX-029, FW-FX-071).
  Rgb tubeColor(int tube, uint64_t tMs) const;

  /// The envelope at `tMs`, 0-255, 255 = no dimming: `pulse`, or the swing of `breathe` (FW-FX-008).
  uint8_t level(uint64_t tMs) const;

 private:
  Rgb lookup(uint32_t pos16) const;
  Rgb eased(uint16_t pos16) const;

  Effect effect_;
  Rgb lut_[kFxLutEntries];
};

// ---- Display options (FW-FX-031..033) ----

enum class FxTransition : uint8_t { kCut, kCrossfade };

struct DisplayOptions {
  bool leadingZero = true;
  bool seconds = true;
  FxTransition transition = FxTransition::kCut;
  uint16_t fadeMs = 200;  // 50-1000

  bool operator==(const DisplayOptions& o) const {
    return leadingZero == o.leadingZero && seconds == o.seconds && transition == o.transition && fadeMs == o.fadeMs;
  }
};
constexpr long kFadeMinMs = 50;
constexpr long kFadeMaxMs = 1000;

const char* fxTransitionName(FxTransition transition);
bool parseFxTransition(const char* text, FxTransition& out);

/// The options as a JSON object, with `hours` (FW-FX-030) when non-zero. `/display.json` leaves
/// `hours` out: the hour format stays in the EEPROM blob (FW-FX-050). Returns the length, 0 if it
/// does not fit.
size_t writeDisplayJson(const DisplayOptions& options, int hours, char* out, size_t cap);
/// `/display.json`. False when the text is not exactly what writeDisplayJson writes (FW-FX-052).
bool parseDisplayJson(const char* json, size_t len, DisplayOptions& out);

/// Blanks what the options hide (FW-FX-031, FW-FX-032). Not during a flash, which has to show on
/// every tube (FW-ATT-002).
void applyDisplayOptions(std::array<int8_t, kTubes>& digits, const DisplayOptions& options, bool attentionActive);

/// Follows the digits and fades between them when asked to (FW-FX-033). A digit only ever changes
/// because the digits given to update() did: nothing about colors reaches here (FW-DSP-025).
class DigitFader {
 public:
  void update(const std::array<int8_t, kTubes>& digits, uint32_t nowMs, const DisplayOptions& options);
  /// Writes digit, fromDigit and mix of each tube.
  void fill(std::array<TubeState, kTubes>& tubes) const;

 private:
  std::array<int8_t, kTubes> digit_{};
  std::array<int8_t, kTubes> from_{};
  std::array<uint8_t, kTubes> mix_{};
  std::array<uint32_t, kTubes> startMs_{};
  bool started_ = false;
};

// ---- One frame (FW-FX-070) ----

/// What a frame showed, for the log and the tests.
struct FrameInfo {
  std::array<Rgb, kTubes> colors{};
  std::array<int8_t, kTubes> digits{};
  uint8_t brightness = 0;  // after pulse; the saved value during a flash
  bool lit = false;
  bool attention = false;
};

/// Effect colors, then a flash over them, then the digits pick the LEDs. The flash takes nothing from
/// the engine: everything is a function of `tMs`, so the effect resumes as if it had never stopped.
Frame composeFrame(const LedLayout& layout, const EffectEngine& engine, const Attention& attention, DigitFader& fader,
                   const DisplayOptions& options, std::array<int8_t, kTubes> digits, uint64_t tMs, uint32_t nowMs,
                   uint8_t brightness, FrameInfo& info);

// ---- Presets (FW-FX-040..045) ----

constexpr int kBuiltinPresetCount = 12;

/// 1-24 chars of A-Z a-z 0-9 - _ and space (FW-FX-040).
bool isValidPresetName(const char* name);
/// Preset names compare without case (FW-FX-040).
bool presetNamesEqual(const char* a, const char* b);

/// The index of a built-in preset, or -1. Index 0 is the default, `rainbow-wave` (OQ-22).
int findBuiltinPreset(const char* name);
void builtinPresetName(int index, char (&out)[kPresetNameMax + 1]);
/// Copies a built-in preset's JSON (FW-FX-041) out of flash; returns its length.
size_t builtinPresetJson(int index, char* out, size_t cap);
/// Parses a built-in preset. False only if the table itself is broken (VER-019 checks all of them).
bool loadBuiltinPreset(int index, Effect& out);

/// The `pattern` a normalized effect starts with: both the built-ins and the store are written with
/// it first, so listing presets needs no JSON parser. Empty when it is not there.
void fxPatternOfJson(const char* json, char (&out)[12]);

/// Splits a stored line "name\tjson" in place. False when it is not one.
bool splitPresetLine(char* line, const char*& name, const char*& json);

/// A Content-Type naming JSON (FW-FX §6). Takes the length because a cleared ESP8266 String's c_str()
/// still holds the previous request's bytes.
bool isJsonContentType(const char* value, size_t len);

}  // namespace clock_core
