#pragma once

#include <cstddef>
#include <cstdint>

#include "clock_core/display.h"

namespace clock_core {

// ---- Which GPIOs may be used for what (FW-HW-002, FW-HW-004) ----

/// GPIO 6, 7, 8 and 11 are the in-package flash. Nothing may touch them. DOUT leaves GPIO 9 and 10 free,
/// and this board uses them (FW-HW-009, FW-HW-002).
constexpr bool isFlashGpio(int gpio) { return (gpio >= 6 && gpio <= 8) || gpio == 11; }
/// GPIO 0, 2 and 15 select the boot mode at reset.
constexpr bool isBootStrapGpio(int gpio) { return gpio == 0 || gpio == 2 || gpio == 15; }
/// GPIO 1 and 3 are the serial port on JP1, which the pin-finder prints on.
constexpr bool isSerialGpio(int gpio) { return gpio == 1 || gpio == 3; }

/// A pin build flag can name this GPIO. Measured pins may be boot straps or the serial port.
constexpr bool isUsableGpio(int gpio) { return gpio >= 0 && gpio <= 16 && !isFlashGpio(gpio); }

template <size_t N>
constexpr bool allUsableGpios(const int (&pins)[N]) {
  for (size_t i = 0; i < N; i++) {
    if (!isUsableGpio(pins[i])) return false;
  }
  return true;
}

/// FW-HW-004: the pin-finder may put data on this GPIO. Besides the spec's boot straps and flash pins:
/// not the serial port, which it needs, not GPIO 9 and 10, which it has no reason to drive, and not
/// GPIO 16, which the LED driver cannot reach (its output registers only cover GPIO 0-15).
constexpr bool pinFinderMayDrive(int gpio) {
  return isUsableGpio(gpio) && !isBootStrapGpio(gpio) && !isSerialGpio(gpio) && gpio != 9 && gpio != 10 && gpio != 16;
}

// ---- The pin-finder's walk (FW-HW-004) ----

/// Every GPIO the pin-finder drives unless the build names one.
constexpr int kPinFinderPins[] = {4, 5, 12, 13, 14};
constexpr int kPinFinderPinCount = static_cast<int>(sizeof(kPinFinderPins) / sizeof(kPinFinderPins[0]));

constexpr bool pinFinderPinsAreSafe() {
  for (int gpio : kPinFinderPins) {
    if (!pinFinderMayDrive(gpio)) return false;
  }
  return true;
}
static_assert(pinFinderPinsAreSafe(), "the pin-finder must not drive these GPIOs (FW-HW-004)");

/// Per pin: LED 0 with each of its three color bytes alone, to read the color order, then every LED.
constexpr int kPinFinderColorSteps = 3;
constexpr int kPinFinderStepsPerPin = kPinFinderColorSteps + kLedCount;

struct PinFinderStep {
  int pinSlot;   // index into the build's list of pins
  int led;       // chain index 0..59
  int onlyByte;  // 0..2 when only that color byte is on, -1 for all three
};

/// What step number `step` of the walk shows; the walk repeats for good. Pure, for the host tests.
constexpr PinFinderStep pinFinderStep(uint32_t step, int pinCount) {
  const uint32_t total = static_cast<uint32_t>(pinCount) * kPinFinderStepsPerPin;
  const uint32_t at = step % total;
  const int slot = static_cast<int>(at / kPinFinderStepsPerPin);
  const int inPin = static_cast<int>(at % kPinFinderStepsPerPin);
  if (inPin < kPinFinderColorSteps) return {slot, 0, inPin};
  return {slot, inPin - kPinFinderColorSteps, -1};
}

}  // namespace clock_core
