#pragma once

#include <array>
#include <cstdint>

#include "clock_core/color.h"
#include "clock_core/display.h"

namespace clock_core {

/// A timed color override that sits on top of the saved tube colors. The saved colors are never
/// touched, so when the override ends the clock is back to its defaults with nothing to restore.
/// Times are millis() values; the math is wrap-safe across the 49-day rollover.
class Attention {
 public:
  static constexpr uint32_t kMaxDurationMs = 30000;
  static constexpr uint32_t kBlinkPeriodMs = 1000;

  void start(const Hsv& color, uint32_t nowMs, uint32_t durationMs, bool blink);
  void cancel() { active_ = false; }
  bool isActive(uint32_t nowMs) const;
  /// Call from the loop. Forgets a flash that has ended, so it cannot come back when millis() wraps
  /// (FW-ATT-004).
  void expire(uint32_t nowMs) {
    if (active_ && !isActive(nowMs)) active_ = false;
  }

  /// The colors to show now, and whether the tubes are lit (false during the dark half of a blink).
  bool apply(std::array<Hsv, kTubes>& colors, uint32_t nowMs) const;

  /// The same over the effect engine's RGB (FW-FX-070). The flash color is converted once, in
  /// start(), so the frame path stays free of floats (FW-FX-071).
  bool apply(std::array<Rgb, kTubes>& colors, uint32_t nowMs) const;

 private:
  bool blinkLit(uint32_t nowMs) const;

  Hsv color_;
  Rgb rgb_;
  uint32_t startMs_ = 0;
  uint32_t durationMs_ = 0;
  bool blink_ = false;
  bool active_ = false;
};

}  // namespace clock_core
