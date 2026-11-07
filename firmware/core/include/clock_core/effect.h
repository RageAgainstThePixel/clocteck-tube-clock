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
constexpr size_t kFxJsonBytes = 768;      // the longest effect writeEffectJson can produce
constexpr uint8_t kFxNoAt = 255;          // a palette stop without an explicit `at`
constexpr size_t kPresetNameMax = 24;     // FW-FX-040
constexpr int kMaxUserPresets = 16;       // FW-FX-042
/// One stored preset: name, a tab, the effect, a newline and the NUL (FW-FX-042).
constexpr size_t kPresetLineBytes = kPresetNameMax + 1 + kFxJsonBytes + 2;
/// The time between the last change and /effect.json being written (FW-FX-051).
constexpr uint32_t kFxSaveDebounceMs = 2000;

enum class FxPattern : uint8_t { kSolid, kTubes, kWave, kGradient, kBreathe, kSparkle, kChase, kFire, kStrobe, kMatrix };
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

/// FW-FX-016: what a tube does when its digit changes, on top of any pattern.
enum class FxAccentStyle : uint8_t { kFlicker, kFlash, kDip, kGlow, kShift };
constexpr uint8_t kAccentAllTubes = 0x3F;  // bit t = tube t, left to right
constexpr uint8_t kAccentHmTubes = 0x0F;   // hours and minutes
constexpr uint32_t kAccentMinMs = 50;
constexpr uint32_t kAccentMaxMs = 3000;
constexpr uint32_t kAccentMaxSpreadMs = 1000;
constexpr uint32_t kSparkleMinFadeMs = 50;    // FW-FX-018
constexpr uint32_t kSparkleMaxFadeMs = 3000;

struct FxAccent {
  FxAccentStyle style = FxAccentStyle::kGlow;
  uint16_t ms = 400;
  bool hasColor = false;  // without one, the palette's last stop
  Rgb color{};
  uint8_t tubes = kAccentAllTubes;
  uint16_t spread = 0;     // ms per tube the accent takes to reach a neighbour, 0 for none
  uint8_t strength = 100;  // 0-100

  bool operator==(const FxAccent& o) const {
    return style == o.style && ms == o.ms && hasColor == o.hasColor && (!hasColor || color == o.color) && tubes == o.tubes &&
           spread == o.spread && strength == o.strength;
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
  uint8_t density = 20;  // FW-FX-009: sparkle and fire, 1-100
  uint8_t width = 1;     // FW-FX-010: chase, tubes per runner, 1-6
  uint8_t count = 1;     // FW-FX-010: chase, runners, 1-6
  uint8_t duty = 50;     // FW-FX-011: strobe, percent of a period on the first color
  uint16_t seed = 0;     // FW-FX-012: sparkle, fire and matrix, which random sequence
  bool glyphs = false;   // FW-FX-017: matrix, random digits while a drop passes
  uint16_t fade = 250;   // FW-FX-018: sparkle, ms a flash takes to fade back to the base
  uint8_t splash = 0;    // FW-FX-018: sparkle, percent of a flash that reaches each neighbour
  uint16_t splashFade = 250;  // FW-FX-018: sparkle, ms the splash on a neighbour takes to fade
  bool hasAccent = false;
  FxAccent accent;  // FW-FX-016

  /// The defaults for a pattern: nixie neon orange, and for `breathe` a sine swing over 32..255 (FW-FX-024).
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
  kDensity,
  kWidth,
  kCount,
  kDuty,
  kSeed,
  kGlyphs,
  kAccent,
  kFade,
  kSplash,
  kSplashFade,
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

  /// FW-FX-016: `base` with the accent applied, `ageMs` after it reached this tube (< accent.ms).
  Rgb accented(int tube, uint64_t tMs, const Rgb& base, uint32_t ageMs) const;

  /// FW-FX-017: the random digit a matrix drop shows on this tube, or kNoGlyph.
  int8_t glyph(int tube, uint64_t tMs) const;

 private:
  Rgb lookup(uint32_t pos16) const;
  Rgb eased(uint16_t pos16) const;
  Rgb sparkle(int tube, uint64_t tMs) const;
  int sparkleFlash(int tube, uint64_t tMs, uint32_t fade, Rgb& flash) const;
  Rgb chase(int tube, uint32_t cycle) const;
  Rgb fire(int tube, uint64_t tMs) const;
  Rgb strobe(int tube, uint32_t cycle) const;
  Rgb matrix(int tube, uint64_t tMs) const;
  /// Where a matrix drop is on this tube, 0..65535 through it; false when no drop is passing.
  bool matrixDrop(int tube, uint64_t tMs, uint32_t& progress) const;

  Effect effect_;
  Rgb lut_[kFxLutEntries];
};

constexpr int8_t kNoGlyph = -128;

// ---- Display options (FW-FX-031..033) ----

/// FW-FX-033: how a tube changes digit. Scramble shows random digits, carry counts up to the new one.
enum class FxTransition : uint8_t { kCut, kCrossfade, kScramble, kCarry };

struct DisplayOptions {
  bool leadingZero = true;
  bool seconds = true;
  FxTransition transition = FxTransition::kCut;
  uint16_t fadeMs = 200;  // 50-1000
  /// FW-FX-034: percent per tube, left to right, multiplied in after everything else.
  std::array<uint8_t, kTubes> tubeBrightness{{100, 100, 100, 100, 100, 100}};

  bool operator==(const DisplayOptions& o) const {
    return leadingZero == o.leadingZero && seconds == o.seconds && transition == o.transition && fadeMs == o.fadeMs &&
           tubeBrightness == o.tubeBrightness;
  }
};
constexpr long kFadeMinMs = 50;
constexpr long kFadeMaxMs = 1000;
/// The longest display object writeDisplayJson can produce, with room to spare.
constexpr size_t kDisplayJsonBytes = 160;

const char* fxTransitionName(FxTransition transition);
bool parseFxTransition(const char* text, FxTransition& out);
/// `a,b,c,d,e,f`: six whole numbers 0-100 (FW-FX-034, FW-FX-068). `out` is only written on success.
bool parseTubeBrightness(const char* text, std::array<uint8_t, kTubes>& out);

/// The options as a JSON object, with `hours` (FW-FX-030) when non-zero. `/display.json` leaves
/// `hours` out: the hour format stays in the EEPROM blob (FW-FX-050). Returns the length, 0 if it
/// does not fit.
size_t writeDisplayJson(const DisplayOptions& options, int hours, char* out, size_t cap);
/// `/display.json`. False when the text is not what writeDisplayJson writes (FW-FX-052); a file
/// from before `tubeBrightness` existed is still read, with every tube at 100.
bool parseDisplayJson(const char* json, size_t len, DisplayOptions& out);

/// Blanks what the options hide (FW-FX-031, FW-FX-032). Not during a flash, which has to show on
/// every tube (FW-ATT-002).
void applyDisplayOptions(std::array<int8_t, kTubes>& digits, const DisplayOptions& options, bool attentionActive);

/// Follows the digits and moves between them as the transition says (FW-FX-033). A digit only ever
/// changes because the digits given to update() did: nothing about colors reaches here (FW-DSP-025).
class DigitFader {
 public:
  void update(const std::array<int8_t, kTubes>& digits, uint32_t nowMs, const DisplayOptions& options);
  /// Writes digit, fromDigit and mix of each tube: during a scramble or carry, the digit on its way.
  void fill(std::array<TubeState, kTubes>& tubes) const;
  /// The digit the tube is meant to show, whatever a transition is showing on the way.
  int8_t digit(int tube) const { return digit_[tube]; }
  /// FW-FX-016: when the tube last changed to a digit (not to blank), after the first update.
  bool lastChange(int tube, uint32_t& atMs) const {
    atMs = startMs_[tube];
    return changed_[tube];
  }

 private:
  std::array<int8_t, kTubes> digit_{};
  std::array<int8_t, kTubes> from_{};
  std::array<int8_t, kTubes> prev_{};   // the digit before the last change, for carry
  std::array<int8_t, kTubes> shown_{};  // what a scramble or carry shows now
  std::array<uint8_t, kTubes> mix_{};
  std::array<uint32_t, kTubes> startMs_{};
  std::array<bool, kTubes> changed_{};
  bool started_ = false;
};

// ---- One frame (FW-FX-070) ----

/// What a frame showed, for the log and the tests.
struct FrameInfo {
  std::array<Rgb, kTubes> colors{};
  std::array<int8_t, kTubes> digits{};  // what the time means; blank in the dark half of a blink
  std::array<int8_t, kTubes> shown{};   // what the LEDs show: a scramble, carry or glyph on its way
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

constexpr int kBuiltinPresetCount = 19;

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
