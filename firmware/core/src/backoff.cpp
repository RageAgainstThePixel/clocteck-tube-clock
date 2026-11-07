#include "clock_core/backoff.h"

namespace clock_core {

uint32_t AuthBackoff::onFailure(uint32_t ip, uint32_t nowMs) {
  auto age = [nowMs](const Slot& s) { return static_cast<uint32_t>(nowMs - s.lastFailMs); };
  Slot* slot = nullptr;
  for (Slot& s : slots_) {
    if (s.failures && age(s) >= kWindowMs) s.failures = 0;
    if (s.failures && s.ip == ip) slot = &s;
  }
  if (!slot) {
    // A free slot if there is one, else evict the client whose last failure is oldest.
    slot = &slots_[0];
    for (Slot& s : slots_) {
      if (!s.failures) {
        slot = &s;
        break;
      }
      if (age(s) > age(*slot)) slot = &s;
    }
    slot->ip = ip;
    slot->failures = 0;
  }
  slot->lastFailMs = nowMs;
  if (slot->failures < 255) slot->failures++;
  return slot->failures > kFreeFailures ? kDelayMs : 0;
}

void AuthBackoff::onSuccess(uint32_t ip) {
  for (Slot& s : slots_) {
    if (s.ip == ip) s.failures = 0;
  }
}

}  // namespace clock_core
