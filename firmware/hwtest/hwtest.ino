// FW-HW-010: the hardware test, a separate image for reading the remaining pins. Build it with
//   sh firmware/scripts/build.sh -DTUBECLOCK_HWTEST
// and watch the serial log at 115200 on JP1. No Wi-Fi, no settings, no secrets.
//
// At startup it looks for the RTC on every pair of the candidate pins, only reading from it, then
// tries the buzzer on GPIO 15 twice: a plain level, then a tone in case the buzzer is passive. After
// that it logs every input change:
//   rtc sda=<n> scl=<n> seconds=<bcd>   the chip answered on this pair
//   buzzer level / buzzer tone          say which of the two made a sound
//   gpio=<n> level=<0|1>                press each button in turn: this is its pin and pressed level
//
// Reading GPIO 9 and 10 is safe on the ESP8285, whose flash runs in DOUT mode (FW-HW-002). Nothing here
// drives them. Any change flashes a green LED, so GPIO 10's RTC signal keeps it flashing untouched.

#include <Adafruit_NeoPixel.h>
#include <Wire.h>

#include "clock_core/display.h"

using namespace clock_core;

#ifndef TUBECLOCK_HWTEST
#error "build the hardware test with: sh firmware/scripts/build.sh -DTUBECLOCK_HWTEST"
#endif

static const int kLedPin = 13;
static const int kBuzzerPin = 15;
static const uint8_t kRtcAddress = 0x68;
static const uint32_t kBeepMs = 150;
static const uint32_t kToneHz = 2000;
static const uint32_t kDebounceMs = 30;
static const uint32_t kPressFlashMs = 200;

static const int kRtcCandidates[] = {4, 5, 12, 14};
static const int kRtcCandidateCount = sizeof(kRtcCandidates) / sizeof(kRtcCandidates[0]);
static const int kInputs[] = {0, 2, 4, 5, 9, 10, 12, 14, 16};
static const int kInputCount = sizeof(kInputs) / sizeof(kInputs[0]);

struct InputState {
  int stable;
  int seen;
  uint32_t changedMs;
};

Adafruit_NeoPixel strip(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);
InputState inputs[kInputCount];
bool rtcFound = false;
uint32_t flashUntilMs = 0;
bool flashing = false;

void showFlash(bool on) {
  strip.clear();
  if (on) strip.setPixelColor(LedLayout::ct001().index[0][0], 0, 64, 0);
  strip.show();
  flashing = on;
}

// The RTC is only read: register 0 holds the seconds, and writing it would disturb the time.
void findRtc() {
  for (int a = 0; a < kRtcCandidateCount; a++) {
    for (int b = 0; b < kRtcCandidateCount; b++) {
      if (a == b) continue;
      const int sda = kRtcCandidates[a];
      const int scl = kRtcCandidates[b];
      Wire.begin(sda, scl);
      Wire.beginTransmission(kRtcAddress);
      Wire.write(static_cast<uint8_t>(0));
      if (Wire.endTransmission() != 0) continue;
      if (Wire.requestFrom(kRtcAddress, static_cast<uint8_t>(1)) != 1) continue;
      const int seconds = Wire.read();
      Serial.printf("[hwtest] rtc sda=%d scl=%d seconds=%02x\n", sda, scl, seconds);
      rtcFound = true;
    }
  }
  if (!rtcFound) Serial.println(F("[hwtest] rtc not found on GPIO 4, 5, 12, 14"));
}

void tryBuzzer() {
  Serial.println(F("[hwtest] buzzer level"));
  digitalWrite(kBuzzerPin, HIGH);
  delay(kBeepMs);
  digitalWrite(kBuzzerPin, LOW);
  delay(1500);
  Serial.println(F("[hwtest] buzzer tone"));
  tone(kBuzzerPin, kToneHz);
  delay(kBeepMs);
  noTone(kBuzzerPin);
  digitalWrite(kBuzzerPin, LOW);
}

void setupInputs() {
  const uint32_t now = millis();
  for (int i = 0; i < kInputCount; i++) {
    const int gpio = kInputs[i];
    pinMode(gpio, gpio == 16 ? INPUT : INPUT_PULLUP);  // GPIO 16 has no pull-up
    delay(1);
    const int level = digitalRead(gpio);
    inputs[i] = {level, level, now};
    Serial.printf("[hwtest] idle gpio=%d level=%d\n", gpio, level);
  }
}

void setup() {
  digitalWrite(kBuzzerPin, LOW);
  pinMode(kBuzzerPin, OUTPUT);
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  Serial.println();
  Serial.println(F("[hwtest] FW-HW-010: starting"));
  strip.begin();
  showFlash(false);
  delay(1000);
  findRtc();
  tryBuzzer();
  setupInputs();
  Serial.println(F("[hwtest] ready: press each button in turn"));
}

void loop() {
  const uint32_t now = millis();
  for (int i = 0; i < kInputCount; i++) {
    InputState& input = inputs[i];
    const int level = digitalRead(kInputs[i]);
    if (level != input.seen) {
      input.seen = level;
      input.changedMs = now;
    }
    if (input.seen != input.stable && static_cast<uint32_t>(now - input.changedMs) >= kDebounceMs) {
      input.stable = input.seen;
      Serial.printf("[hwtest] gpio=%d level=%d\n", kInputs[i], input.stable);
      showFlash(true);
      flashUntilMs = now + kPressFlashMs;
    }
  }
  if (flashing && static_cast<int32_t>(now - flashUntilMs) >= 0) showFlash(false);
}
