#pragma once

#include <array>
#include <cstdint>

#include "clock_core/color.h"

namespace clock_core {

constexpr int kTubes = 6;
constexpr int kDigitsPerTube = 10;
constexpr int kLedCount = kTubes * kDigitsPerTube;

/// A tube with no digit lit.
constexpr int8_t kBlank = -1;

/// Where each digit's LED sits in the WS2812 chain: index[tube][digit], tube 0 being the viewer's leftmost.
struct LedLayout {
  std::array<std::array<uint8_t, kDigitsPerTube>, kTubes> index{};

  /// The CT-001's chain, measured with the pin-finder (FW-DSP-002, FW-HW-005).
  static LedLayout ct001();
  bool isValid() const;
};

using Frame = std::array<Rgb, kLedCount>;

struct TubeState {
  int8_t digit = kBlank;
  Rgb color;
  /// During a crossfade (FW-FX-033): the outgoing digit, and `digit`'s share of the color, 255 = all.
  int8_t fromDigit = kBlank;
  uint8_t mix = 255;
};

/// Builds the LED frame: one lit LED per tube (two during a crossfade), every other LED off.
Frame render(const LedLayout& layout, const std::array<TubeState, kTubes>& tubes, uint8_t brightness);

/// FW-OTA-005: upload progress. Tubes lock on a green 0 from the left as `done` nears `total`, and the rest
/// show digits and colors picked from `seed`, so a new seed scrambles them. Pure, for the host tests.
Frame renderUpdateProgress(const LedLayout& layout, uint32_t done, uint32_t total, uint32_t seed, uint8_t brightness);

}  // namespace clock_core
