#include "clock_core/timefmt.h"

namespace clock_core {

std::array<int8_t, kTubes> timeDigits(uint8_t hour, uint8_t minute, uint8_t second, bool is24Hour) {
  uint8_t h = hour % 24;
  if (!is24Hour) {
    h %= 12;
    if (h == 0) h = 12;
  }
  auto tens = [](uint8_t x) { return static_cast<int8_t>(x / 10); };
  auto ones = [](uint8_t x) { return static_cast<int8_t>(x % 10); };
  return {tens(h), ones(h), tens(minute % 60), ones(minute % 60), tens(second % 60), ones(second % 60)};
}

std::array<int8_t, kTubes> tubeDigits(bool timeKnown, bool attentionActive, uint8_t hour, uint8_t minute,
                                      uint8_t second, bool is24Hour) {
  if (timeKnown) return timeDigits(hour, minute, second, is24Hour);
  std::array<int8_t, kTubes> digits{};
  digits.fill(attentionActive ? 0 : kBlank);
  return digits;
}

}  // namespace clock_core
