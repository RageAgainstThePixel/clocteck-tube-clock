# Custom firmware

Custom C++ firmware for the CT-001's board ("Nixie Clock V4.5.1", ESP8285). The goals are authenticated control, no Wi-Fi password leak, no open setup hotspot, direct control of every tube, and an MCP server on the clock so agents can get your attention. It runs on a real clock, with some features still to check there (see [the spec](../spec/firmware/hardware.md)).

## Hardware

| Part | Notes |
|---|---|
| ESP8285 | ESP8266 with 1 MB flash inside the chip; PCB antenna; 26 MHz crystal |
| 60 × WS2812-style RGB LEDs | 6 tubes × 10 digits, one LED per digit (LED1–LED60) |
| RTC (GC1307, DS1307-compatible, I2C) | 32.768 kHz crystal, backed by a CR1220 coin cell |
| 4 buttons, buzzer | Rear buttons; buzzer driven through a transistor |
| JP1 | Unpopulated 6-pin header, the serial programming port. Square pad = pin 1, nearest USB-C: 1 = 5 V (never connect to an adapter), 2 = ESP RX, 3 = ESP TX, 4 = GPIO0, 5 = unidentified, 6 = GND |
| USB-C | Power only: 5.1 kΩ CC resistors, no USB-serial bridge |

The GPIO for each part and the LED chain order were measured on a real clock, and `LedLayout::ct001()` holds the chain order. To enter flashing mode, wire adapter TXD to pin 2, RXD to pin 3 and GND to pin 6, then ground pin 4 while powering on.

## Layout

- `core/`: hardware-independent C++ (tube rendering, effects engine, time digits, color, auth, attention flashes, saved settings, MCP). It builds on the host and on the ESP8285.
- `test/`: host unit tests for `core/`.
- `tubeclock/`: the Arduino sketch (Wi-Fi, web API, MCP server, mDNS, OTA, display loop).
- `pinfinder/`, `hwtest/`: separate sketches for mapping the board (below).
- `scripts/`: POSIX sh scripts that run inside WSL2, and Node scripts run from Windows (tests, Wi-Fi update, backup, flash).

## What it does differently from the stock firmware

- Station mode only: no setup hotspot, and no route ever returns the Wi-Fi password.
- Every `/api` route and `/mcp` need `Authorization: Bearer <FW_API_TOKEN>`.
- Agents can flash the tubes through an MCP server on the clock itself, with no computer in between ([tools and setup](../README.md#mcp-server-let-agents-get-your-attention)). It has been tested on the emulator, but not yet on a real clock.
- Attention flashes run on the clock and fall back to the saved colors by themselves.
- The time zone is a POSIX TZ rule (`EST5EDT,M3.2.0,M11.1.0`), so daylight saving is automatic.
- It answers at `tubeclock.local` (mDNS), so it doesn't matter which IP it gets.
- Later updates go over Wi-Fi (ArduinoOTA, password = the API token), so the case only has to be opened once.

Anyone with the board in hand can still read the Wi-Fi password out of flash; that is true of any device that joins Wi-Fi by itself. What changes is that nobody on the network can.

## API

| Route | Args | |
|---|---|---|
| `GET /health` | | `ok`, no auth |
| `GET /api/state` | | version, hostname, time, tz, is24h, brightness, preset, effect, display, attention, tubes, timeSource, lastSync, rejectedSyncs |
| `POST /api/tube` | `t=1-6\|all`, `h=0-360`, `s=0-100` | saved; the active effect becomes `tubes` |
| `POST /api/brightness` | `v=0-255` | saved |
| `POST /api/hours` | `format=12\|24` | saved |
| `POST /api/tz` | `tz=<POSIX TZ rule>` | saved |
| `POST /api/attention` | `h`, `s`, `ms=1-30000` (5000), `blink=0\|1`, `beep=0\|1` | not saved |
| `DELETE /api/attention` | | ends a flash early |
| `POST /api/restart` | | `{"ok":true}`, then reboots |
| `GET /api/effect` | | the active effect, every field filled in, and `preset` |
| `PUT /api/effect` | JSON body, partial allowed | the resulting effect, saved 2 s after the last change |
| `GET /api/presets` | | `[{name, builtin, pattern, active?}]` |
| `GET /api/preset` | `name` | that preset's effect |
| `POST /api/preset` | `name` | applies it; the state |
| `PUT /api/preset` | `name`, JSON body or none (the active effect) | the saved effect |
| `DELETE /api/preset` | `name` | `{"ok":true}` |
| `GET`, `POST /api/display` | `leadingZero=0\|1`, `seconds=0\|1`, `transition=cut\|crossfade`, `fadeMs=50-1000` | the display options, saved |

The MCP server is separate from this API: it answers at `/mcp` over Streamable HTTP, with the same token ([spec](../spec/firmware/mcp.md)).

Every write returns the new state (the effect routes return the effect), and only a write that changes something touches flash. JSON bodies are `Content-Type: application/json`, at most 1024 bytes (`415`, `413`); the other routes take no body and refuse one over the same limit. A known path with the wrong method answers `405` with `Allow`. From the sixth bad token in a row, each `401` to that IP takes 1 s.

## Effects

What the tubes show is data: a pattern (`solid`, `tubes`, `wave`, `gradient`, `breathe`) with a palette, direction, period, phase, drift, easing and optional pulse ([spec](../spec/firmware/effects.md)). A fresh clock shows the `rainbow-wave` preset. Twelve presets are built in and up to 16 more can be saved. Effect, display options and presets live on a 64 KB LittleFS beside the 80-byte EEPROM settings; an attention flash overrides the effect and the effect carries on behind it.

## Secrets

`scripts/gen-secrets.sh` writes `tubeclock/secrets.h` (git-ignored) from the repo's `.env`: `FW_WIFI_SSID`, `FW_WIFI_PASSWORD`, `FW_API_TOKEN` and optionally `FW_HOSTNAME` (default `tubeclock`). The build scripts run it for you.

## Host tests

```sh
cmake -S firmware -B firmware/build -G Ninja
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

## Emulator

The ESP8266 core ships a host emulator that builds the sketch as a Linux program using the host's network. Inside WSL2 (needs `build-essential`):

```sh
sh firmware/scripts/emulate.sh
```

The web server's port 80 moves to 9080, and what the tubes show prints as `[leds] ...` lines, at most about one a second. The EEPROM and a 64 KB LittleFS with the device's block size persist as files in `~/.cache/tubeclock/host`, and the log goes to `emulator.log` there as well, which the API test reads. From Windows, WSL2 forwards `localhost:9080`, `node firmware/scripts/api-test.mjs` runs the end-to-end API checks against it, and `node firmware/scripts/mcp-test.mjs` runs the MCP checks with the official client (pass a URL to test a real clock). Run the MCP checks first: the API test restarts the clock to see what survives (starting the emulator again itself, without a rebuild), and ends by restarting it once more, which leaves the emulator stopped.

The emulator in core 3.1.2 doesn't build against glibc 2.38+ (Ubuntu 24.04), so `emulate.sh` patches a private copy of the core under `~/.cache/tubeclock` and leaves the installed core alone.

From Windows, `npm run fw:build`, `npm run fw:emulate`, `npm run fw:api-test` and `npm run fw:mcp-test` wrap these.

ArduinoJson is vendored as a single header in `firmware/third_party/ArduinoJson` (MIT, see its `LICENSE.txt`). All three builds use that file, so nothing is downloaded at build time.

## Device build

Inside WSL2 with arduino-cli and the `esp8266:esp8266` core:

```sh
sh firmware/scripts/build.sh                        # the CT-001's measured pins
sh firmware/scripts/build.sh -DTUBECLOCK_LED_PIN=13 # only these parts
```

`build.sh` uses these measured values (FW-HW-002). Naming any pin flag replaces them all, so such a build must include the LED pin (`build.sh` refuses one without it), and any other part whose flags are then missing is compiled out:

| Part | Flags | |
|---|---|---|
| LEDs | `-DTUBECLOCK_LED_PIN=13` | |
| RTC | `-DTUBECLOCK_RTC_SDA=4 -DTUBECLOCK_RTC_SCL=5` | read at boot, written after each NTP sync, holds UTC |
| Buttons | `-DTUBECLOCK_BTN_1=0 -DTUBECLOCK_BTN_2=2 -DTUBECLOCK_BTN_3=9 -DTUBECLOCK_BTN_4=16 -DTUBECLOCK_BTN_PRESSED_LEVEL=0` | any press ends a flash; the level is what a pressed one reads, 0 turns on the internal pull-up, which GPIO 16 does not have |
| Buzzer | `-DTUBECLOCK_BUZZER_PIN=15 -DTUBECLOCK_BUZZER_ON_LEVEL=1` | the level that sounds it |

### Version and time servers

The version is the checkout's `git describe --tags --always --dirty` (no leading `v`), and the build stamps its own time as the earliest believable clock time. The image lands in `firmware/build/device/tubeclock.ino.bin` and as `tubeclock-<version>.bin`. NTP uses `time.cloudflare.com`, `time.google.com`, `time.nist.gov`; to use your own, set `TUBECLOCK_NTP_SERVERS="ntp.lan"` (up to 3, comma-separated, no public pools) before `build.sh`.

### Updating over Wi-Fi

`npm run fw:ota` uploads `firmware/build/device/tubeclock.ino.bin` to `CLOCTECK_HOST`, checks the image first, and waits for the clock to come back. `npm run fw:ota -- <host> [image]` names another clock or image, and `npm run fw:ota -- --check` sends nothing (FW-OTA-006). The password is the API token from `.env`. If an image breaks Wi-Fi, recovery is over JP1.

### Pin-finder

`sh firmware/scripts/build.sh -DTUBECLOCK_PINFINDER` builds `pinfinder.ino.bin` instead: no Wi-Fi and no secrets, only a walk over GPIO 4, 5, 12, 13 and 14. For each it lights LED 0 with each color byte alone, then every LED in turn, one second a step, printing what it sent at 115200 on JP1. It never drives GPIO 0, 2 or 15 (boot straps), 6-8 or 11 (flash), 1 or 3 (the serial port) or 16 (out of the LED driver's reach), and it leaves 9 and 10 alone. Add `-DTUBECLOCK_LED_PIN=<gpio>` to walk just that pin, and leave the buttons alone while it runs, since a candidate can turn out to be one of them.

### Hardware test

`sh firmware/scripts/build.sh -DTUBECLOCK_HWTEST` builds `hwtest.ino.bin` instead (FW-HW-010): no Wi-Fi, settings or secrets, and its log comes out at 115200 on JP1. It looks for the RTC on every pair of GPIO 4, 5, 12 and 14, only reading from it, then tries the buzzer on GPIO 15 once as a plain level and once as a tone. After that it logs every change on GPIO 0, 2, 4, 5, 9, 10, 12, 14 and 16, so pressing each button in turn shows its pin and pressed level.

### First install over JP1

This needs a 3.3 V USB-serial adapter, wired as in [Hardware](#hardware), and the `npm install` dependencies. Never connect the adapter's power pin to JP1: the clock runs from its own USB-C cable.

1. **Back up the stock firmware.** Run `npm run fw:backup -- COM4`, then hold pin 4 to ground while plugging in the clock, and let go after a few seconds. A verified copy lands in the git-ignored `clock/backups/`. It holds your Wi-Fi password, so keep it private. Rename the first one so you can find it, keeping its `.sha256` file's name in step.
2. **Build and check:** `npm run fw:build`, then `npm run fw:flash -- --check firmware/build/device/tubeclock.ino.bin`.
3. **Flash:** `npm run fw:flash -- --write firmware/build/device/tubeclock.ino.bin COM4`, then enter flashing mode the same way.
4. **Unplug and plug the clock back in.** Later updates can use `npm run fw:ota`.

To go back to the stock firmware, pass the backup image to `fw:flash` the same way. This has not been tried yet.
