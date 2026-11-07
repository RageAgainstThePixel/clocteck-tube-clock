#!/bin/sh
# Builds the ESP8285 image into firmware/build/device/. The firmware gets the CT-001's measured pins
# (FW-HW-002), so an update over Wi-Fi cannot leave a clock dark by accident. Naming any pin flag
# replaces all of them, so such a build must name the LED pin too; any other feature whose pin is then
# not given is compiled out.
# -DTUBECLOCK_PINFINDER builds the pin-finder image instead (FW-HW-004).
# -DTUBECLOCK_HWTEST builds the hardware test image instead (FW-HW-010).
# TUBECLOCK_NTP_SERVERS="a,b,c" in the environment replaces the default NTP servers (up to 3).
set -eu
fw=$(cd "$(dirname "$0")/.." && pwd)
sketch=tubeclock
case " $* " in
  *" -DTUBECLOCK_PINFINDER "*) sketch=pinfinder ;;
  *" -DTUBECLOCK_HWTEST "*) sketch=hwtest ;;
esac
stage="$HOME/.cache/tubeclock/device/$sketch"
export PATH="$HOME/.local/bin:$PATH"

# FW-API-031, FW-OTA-009: the version is the checkout's `git describe`, without the leading v.
version=$(git -C "$fw/.." describe --tags --always --dirty)
version=${version#v}
# FW-TIME-013: times before this build are never believed.
epoch=$(date +%s)

# FW-TIME-010, VER-018: trusted servers only, at most lwIP's 3.
servers=${TUBECLOCK_NTP_SERVERS:-}
ntp_flag=
case " $* " in
  *-DTUBECLOCK_NTP_SERVERS*) echo "set TUBECLOCK_NTP_SERVERS in the environment, not as -D" >&2; exit 1 ;;
esac
if [ -n "$servers" ]; then
  case "$servers" in
    *pool.ntp.org*) echo "TUBECLOCK_NTP_SERVERS: public pools such as pool.ntp.org are not allowed" >&2; exit 1 ;;
    *[!A-Za-z0-9.:,-]* | ,* | *, | *,,*) echo "TUBECLOCK_NTP_SERVERS: host names or IPs separated by commas" >&2; exit 1 ;;
  esac
  [ "$(printf '%s' "$servers" | tr -cd , | wc -c)" -le 2 ] || { echo "TUBECLOCK_NTP_SERVERS: at most 3 servers" >&2; exit 1; }
  ntp_flag="-DTUBECLOCK_NTP_SERVERS=\"$servers\""
fi

pins=
if [ "$sketch" = tubeclock ]; then
  case " $* " in
    *" -DTUBECLOCK_LED_PIN"*) ;;
    *" -DTUBECLOCK_RTC_"* | *" -DTUBECLOCK_BTN_"* | *" -DTUBECLOCK_BUZZER_"*)
      echo "naming a pin flag replaces the measured pins: add -DTUBECLOCK_LED_PIN too, or this build is dark" >&2
      exit 1
      ;;
    *) pins="-DTUBECLOCK_LED_PIN=13 -DTUBECLOCK_RTC_SDA=4 -DTUBECLOCK_RTC_SCL=5 -DTUBECLOCK_BTN_1=0 -DTUBECLOCK_BTN_2=2 -DTUBECLOCK_BTN_3=9 -DTUBECLOCK_BTN_4=16 -DTUBECLOCK_BTN_PRESSED_LEVEL=0 -DTUBECLOCK_BUZZER_PIN=15 -DTUBECLOCK_BUZZER_ON_LEVEL=1" ;;
  esac
  sh "$fw/scripts/gen-secrets.sh"
fi
arduino-cli lib list | grep -q '^Adafruit NeoPixel ' || arduino-cli lib install "Adafruit NeoPixel"

# Arduino compiles everything under the sketch's src/, so stage the core there.
rm -rf "$stage"
mkdir -p "$stage/src"
if [ "$sketch" != tubeclock ]; then
  cp "$fw/$sketch/$sketch.ino" "$stage/"
else
  cp "$fw/tubeclock/tubeclock.ino" "$fw/tubeclock/secrets.h" "$stage/"
fi
cp -r "$fw/core/include/clock_core" "$stage/src/"
cp "$fw"/core/src/*.cpp "$stage/src/clock_core/"
# ArduinoJson (OQ-11) is the vendored single header, the same file the host tests and emulator use.
cp "$fw/third_party/ArduinoJson/ArduinoJson.h" "$stage/src/"

# arduino-cli keeps quotes inside a flag, so the defines below arrive as C string literals.
# FW-HW-009, FW-FX-050: 1M (FS:64KB OTA:~470KB).
arduino-cli compile --fqbn esp8266:esp8266:esp8285:eesz=1M64 \
  --build-property "compiler.cpp.extra_flags=-I$stage/src -DTUBECLOCK_VERSION=\"$version\" -DTUBECLOCK_BUILD_EPOCH=$epoch $ntp_flag $pins $*" \
  --output-dir "$fw/build/device" "$stage"

# FW-BUD-001: the image has to leave room for a second one during OTA.
size=$(wc -c < "$fw/build/device/$sketch.ino.bin")
limit=481280
echo "image $size bytes of $limit (FW-BUD-001)"
[ "$size" -le "$limit" ] || { echo "image is over the FW-BUD-001 limit of 470 KB" >&2; exit 1; }

# FW-OTA-009: name the image after the version so an upload traces back to a commit.
cp "$fw/build/device/$sketch.ino.bin" "$fw/build/device/$sketch-$version.bin"
echo "built $sketch-$version.bin"
