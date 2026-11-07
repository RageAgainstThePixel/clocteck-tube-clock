#pragma once

#include <array>
#include <cstdint>

#include "clock_core/display.h"

namespace clock_core {

/// Splits a time into the six tube digits HH MM SS. In 12-hour mode, 0 shows as 12.
std::array<int8_t, kTubes> timeDigits(uint8_t hour, uint8_t minute, uint8_t second, bool is24Hour);

/// What the tubes show: the time when it is known, else blank (FW-TIME-004, FW-DSP-007). While the
/// time is unknown an attention flash still needs something to light, so every tube shows digit 0.
std::array<int8_t, kTubes> tubeDigits(bool timeKnown, bool attentionActive, uint8_t hour, uint8_t minute,
                                      uint8_t second, bool is24Hour);

}  // namespace clock_core
