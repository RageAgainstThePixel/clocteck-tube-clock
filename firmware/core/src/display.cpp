#include "clock_core/display.h"

namespace clock_core {

LedLayout LedLayout::ct001() {
  // The two leftmost tubes run forward along the chain and come last; the other four run backward.
  LedLayout layout;
  layout.index = {{
      {40, 41, 42, 43, 44, 45, 46, 47, 48, 49},
      {50, 51, 52, 53, 54, 55, 56, 57, 58, 59},
      {39, 38, 37, 36, 35, 34, 33, 32, 31, 30},
      {29, 28, 27, 26, 25, 24, 23, 22, 21, 20},
      {19, 18, 17, 16, 15, 14, 13, 12, 11, 10},
      {9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
  }};
  return layout;
}

bool LedLayout::isValid() const {
  std::array<bool, kLedCount> used{};
  for (const auto& tube : index) {
    for (uint8_t i : tube) {
      if (i >= kLedCount || used[i]) return false;
      used[i] = true;
    }
  }
  return true;
}

Frame render(const LedLayout& layout, const std::array<TubeState, kTubes>& tubes, uint8_t brightness) {
  Frame frame{};
  for (int t = 0; t < kTubes; t++) {
    const TubeState& tube = tubes[t];
    const Rgb color = scale(tube.color, brightness);
    if (tube.digit >= 0 && tube.digit < kDigitsPerTube) {
      frame[layout.index[t][tube.digit]] = tube.mix == 255 ? color : scale(color, tube.mix);
    }
    if (tube.mix < 255 && tube.fromDigit >= 0 && tube.fromDigit < kDigitsPerTube) {
      frame[layout.index[t][tube.fromDigit]] = scale(color, static_cast<uint8_t>(255 - tube.mix));
    }
  }
  return frame;
}

Frame renderUpdateProgress(const LedLayout& layout, uint32_t done, uint32_t total, uint32_t seed, uint8_t brightness) {
  const uint64_t locked = total == 0 ? 0 : static_cast<uint64_t>(done) * kTubes / total;
  std::array<TubeState, kTubes> tubes{};
  for (int t = 0; t < kTubes; t++) {
    if (static_cast<uint64_t>(t) < locked) {
      tubes[t].digit = 0;
      tubes[t].color = {0, 255, 0};
    } else {
      const uint32_t mixed = (seed + static_cast<uint32_t>(t) * 7u) * 2654435761u;
      tubes[t].digit = static_cast<int8_t>((mixed >> 16) % kDigitsPerTube);
      tubes[t].color = hsvToRgb({static_cast<uint16_t>((mixed >> 22) % 360), 100, 100});
    }
  }
  return render(layout, tubes, brightness);
}

}  // namespace clock_core
