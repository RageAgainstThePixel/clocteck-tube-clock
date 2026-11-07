#!/bin/sh
# Builds the firmware with the ESP8266 core's host emulator and runs it as a Linux program.
# Wi-Fi is the host's network; the web server's port 80 is shifted to 9080. Extra args go to the
# emulator (-h lists them). EMULATE_SKIP_BUILD=1 runs the last build again, as a reboot would.
# The serial log also goes to ~/.cache/tubeclock/host/emulator.log for the API test (VER-020).
set -eu
fw=$(cd "$(dirname "$0")/.." && pwd)
core=$(ls -d "$HOME"/.arduino15/packages/esp8266/hardware/esp8266/*/ | tail -n 1)
work="$HOME/.cache/tubeclock/host"
emu="$work/esp8266-$(basename "$core")"

# The emulator in core 3.1.2 declares strlcpy/strlcat itself, which clashes with glibc 2.38+
# (Ubuntu 24.04). Patch a private copy so the installed core stays untouched.
if [ ! -d "$emu" ]; then
  mkdir -p "$work"
  cp -r "$core" "$emu.tmp"
  sed -i \
    -e 's|^\(\s*size_t strlcat(.*\)$|#ifndef HAVE_STRLCAT\n\1\n#endif|' \
    -e 's|^\(\s*size_t strlcpy(.*\)$|#ifndef HAVE_STRLCPY\n\1\n#endif|' \
    "$emu.tmp/tests/host/common/mock.h"
  mv "$emu.tmp" "$emu"
fi

# FW-FX-050: the same LittleFS geometry as the device (1M64: 4 KB blocks, 256 B pages), not the
# emulator's default 8 KB blocks, so what fits here fits there. `-L 64` below sets the size.
main="$emu/tests/host/common/ArduinoMain.cpp"
if grep -q 'mock_start_littlefs(name, littlefs_kb);' "$main"; then
  sed -i 's|mock_start_littlefs(name, littlefs_kb);|mock_start_littlefs(name, littlefs_kb, 4, 256);|' "$main"
fi

if [ -z "${EMULATE_SKIP_BUILD:-}" ]; then
  sh "$fw/scripts/gen-secrets.sh"
  rm -rf "$work/tubeclock"
  mkdir -p "$work/tubeclock"
  cp "$fw/tubeclock/tubeclock.ino" "$fw/tubeclock/secrets.h" "$work/tubeclock/"

  # FW-API-031, FW-OTA-009, FW-TIME-013: same version and build-time floor as build.sh.
  version=$(git -C "$fw/.." describe --tags --always --dirty)
  version=${version#v}
  epoch=$(date +%s)

  # MKFLAGS ends up in make's shell recipes, hence the single quotes around the string define.
  make -s -C "$emu/tests/host" FORCE32=0 BINDIR="$work/bin" \
    MKFLAGS="-DHAVE_STRLCAT -DHAVE_STRLCPY -I$work/tubeclock -I$fw/core/include -I$fw/third_party/ArduinoJson '-DTUBECLOCK_VERSION=\"$version\"' -DTUBECLOCK_BUILD_EPOCH=$epoch" \
    USERCXXSOURCES="$(echo "$fw"/core/src/*.cpp)" \
    "$work/tubeclock/tubeclock"
fi

# The emulated EEPROM is a file in the current directory and the LittleFS one beside the binary; both
# stay out of the repo.
cd "$work"
"$work/bin/tubeclock/tubeclock" -S 0 -L 64 "$@" 2>&1 | tee "$work/emulator.log"
