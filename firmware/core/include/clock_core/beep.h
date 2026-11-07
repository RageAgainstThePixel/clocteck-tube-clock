#pragma once

#include <cstdint>

namespace clock_core {

/// FW-ATT-010: the attention beep, two 80 ms pulses 80 ms apart, played once from `start()`.
/// Non-blocking: the loop asks `isOn()` every pass and drives the buzzer pin to match. Times are
/// millis() values; the math is wrap-safe.
class BeepPattern {
 public:
  static constexpr uint32_t kPulseMs = 80;
  static constexpr uint32_t kPulses = 2;
  /// From the start of the first pulse to the end of the last.
  static constexpr uint32_t kLengthMs = (2 * kPulses - 1) * kPulseMs;

  void start(uint32_t nowMs) {
    startMs_ = nowMs;
    active_ = true;
  }
  void cancel() { active_ = false; }
  bool isActive(uint32_t nowMs) const { return active_ && nowMs - startMs_ < kLengthMs; }
  /// Call from the loop. Forgets a pattern that has ended, so it cannot replay when millis() wraps.
  void expire(uint32_t nowMs) {
    if (active_ && !isActive(nowMs)) active_ = false;
  }
  /// True while a pulse should sound.
  bool isOn(uint32_t nowMs) const { return isActive(nowMs) && ((nowMs - startMs_) / kPulseMs) % 2 == 0; }

 private:
  uint32_t startMs_ = 0;
  bool active_ = false;
};

}  // namespace clock_core
