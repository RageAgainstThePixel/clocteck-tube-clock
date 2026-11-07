#pragma once

#include <cstdint>

namespace clock_core {

/// FW-HW-006: software debounce for one button, polled from the loop, never blocking. The raw reading
/// has to hold for kDebounceMs before it counts. Times are millis() values; the math is wrap-safe.
class ButtonDebouncer {
 public:
  static constexpr uint32_t kDebounceMs = 30;

  /// Feeds one reading. True exactly once per press, when it has held long enough; a button still
  /// down from before (a stuck one, or one held through a new flash) does not count again.
  bool update(bool down, uint32_t nowMs) {
    if (down != raw_) {
      raw_ = down;
      changedMs_ = nowMs;
      return false;
    }
    if (raw_ == stable_ || nowMs - changedMs_ < kDebounceMs) return false;
    stable_ = raw_;
    return stable_;
  }

  bool isDown() const { return stable_; }

 private:
  uint32_t changedMs_ = 0;
  bool raw_ = false;
  bool stable_ = false;
};

}  // namespace clock_core
