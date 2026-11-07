// FW-HW-004: the pin-finder, a separate image for mapping the board. Build it with
//   sh firmware/scripts/build.sh -DTUBECLOCK_PINFINDER
// and watch the tubes and the serial log at 115200 on JP1. No Wi-Fi, no settings, no secrets.
//
// For each candidate GPIO it lights LED 0 with each color byte alone, then every LED in turn, one
// second a step, and prints what it just sent:
//   gpio=<n>       the pin under test: when something lights, that is the LED data pin
//   led=<i>        the chain index: which digit lit gives the chain order
//   byte=<0|1|2>   the only color byte on: the color seen for byte=0 is the first in the color order
//                  (green first means GRB)
//
// With nothing lit on one GPIO the walk takes about a minute; add -DTUBECLOCK_LED_PIN=<gpio> to walk
// just that pin once it is known. It MUST never drive GPIO 0, 2, 15 (boot straps) or 6-8 and 11 (flash),
// and has no reason to drive 9 and 10; it also leaves GPIO 1 and 3 alone, since those are the serial port. A candidate can turn out to be a
// button or the RTC's bus: leave the buttons alone while it runs.

#include <Adafruit_NeoPixel.h>

#include "clock_core/pins.h"

using namespace clock_core;

#ifndef TUBECLOCK_PINFINDER
#error "build the pin-finder with: sh firmware/scripts/build.sh -DTUBECLOCK_PINFINDER"
#endif

#ifdef TUBECLOCK_LED_PIN
static_assert(pinFinderMayDrive(TUBECLOCK_LED_PIN), "the pin-finder must not drive this GPIO (FW-HW-004)");
const int kPins[] = {TUBECLOCK_LED_PIN};
#else
const int (&kPins)[kPinFinderPinCount] = kPinFinderPins;
#endif
const int kPinCount = sizeof(kPins) / sizeof(kPins[0]);

static const uint32_t kStepMs = 1000;
static const uint8_t kLevel = 64;  // one LED at a time, and dim: the tubes are easy to read at this

// NEO_RGB sends the bytes in the order given, so `byte` below is the byte's place on the wire. The
// constructor only notes the pin; nothing is driven before begin().
Adafruit_NeoPixel strip(kLedCount, kPins[0], NEO_RGB + NEO_KHZ800);
uint32_t startMs = 0;
uint32_t shownStep = 0xFFFFFFFFu;
int shownSlot = 0;

void showStep(uint32_t step) {
  const PinFinderStep s = pinFinderStep(step, kPinCount);
  strip.clear();
  if (s.pinSlot != shownSlot) {
    strip.show();                    // dark before leaving the old pin
    strip.setPin(kPins[s.pinSlot]);  // the old pin goes back to an input
    shownSlot = s.pinSlot;
  }
  const uint8_t r = s.onlyByte < 0 || s.onlyByte == 0 ? kLevel : 0;
  const uint8_t g = s.onlyByte < 0 || s.onlyByte == 1 ? kLevel : 0;
  const uint8_t b = s.onlyByte < 0 || s.onlyByte == 2 ? kLevel : 0;
  strip.setPixelColor(s.led, r, g, b);
  strip.show();
  if (s.onlyByte < 0) {
    Serial.printf("[pinfinder] gpio=%d led=%d\n", kPins[s.pinSlot], s.led);
  } else {
    Serial.printf("[pinfinder] gpio=%d led=%d byte=%d\n", kPins[s.pinSlot], s.led, s.onlyByte);
  }
}

void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  Serial.println();
  Serial.println(F("[pinfinder] FW-HW-004: walking the candidate GPIOs"));
  strip.begin();
  startMs = millis();
}

void loop() {
  const uint32_t step = (millis() - startMs) / kStepMs;
  if (step != shownStep) {
    shownStep = step;
    showStep(step);
  }
}
