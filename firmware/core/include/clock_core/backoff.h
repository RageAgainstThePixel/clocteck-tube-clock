#pragma once

#include <array>
#include <cstdint>

namespace clock_core {

/// Per-client-IP slowdown for failed bearer auth (FW-SEC-014): after kFreeFailures failures, each
/// further 401 waits kDelayMs. A client's count resets kWindowMs after its last failure, and a
/// successful auth clears it, so there is never a lockout. Times are millis() values (wrap-safe);
/// nothing here depends on the wall clock (FW-SEC-020).
class AuthBackoff {
 public:
  static constexpr uint8_t kFreeFailures = 5;
  static constexpr uint32_t kWindowMs = 60000;
  static constexpr uint32_t kDelayMs = 1000;
  static constexpr int kSlots = 4;

  /// Records a failed auth from `ip`; returns how long to wait before sending the 401.
  uint32_t onFailure(uint32_t ip, uint32_t nowMs);
  void onSuccess(uint32_t ip);

 private:
  struct Slot {
    uint32_t ip = 0;
    uint32_t lastFailMs = 0;
    uint8_t failures = 0;  // 0 = free slot
  };
  std::array<Slot, kSlots> slots_{};
};

}  // namespace clock_core
