# Specification

These documents are the source of truth for the ClocTeck CT-001 project: the replacement firmware, the on-device MCP server, and the host software (TypeScript client, CLI, PC MCP server). Work starts from a requirement here; code and tests follow it.

## Goals

1. Own the clock: a firmware the user controls, built from this repo, with no cloud service, open hotspot or unauthenticated route.
2. Let agents get the user's attention on the clock without touching the user's saved colors, and without a PC in the loop.
3. Leave the existing stock-firmware tooling (client, CLI commands, PC MCP server) working as it is; it is frozen, not extended (OQ-18).
4. Everything testable without hardware: host unit tests, the ESP8266 host emulator, and an end-to-end API script.

## Non-goals

- Copying the stock firmware's modes one for one. Colors and animations are a data-driven effects engine with presets ([firmware/effects.md](firmware/effects.md)); the stock digit effects exist only as optional digit transitions, never forced.
- Alarm, buzzer melodies (the buzzer only beeps for attention), any runtime Wi-Fi provisioning, captive portal, HTTPS on the device.
- Running the firmware on another board. The target is the "Nixie Clock V4.5.1" ESP8285 board only.
- Supporting agents outside the LAN. The device is never exposed to the internet.

## Documents

| Area | File | ID prefix |
|---|---|---|
| Wi-Fi, provisioning, security model | [firmware/security.md](firmware/security.md) | `FW-SEC` |
| HTTP API | [firmware/http-api.md](firmware/http-api.md) | `FW-API` |
| On-device MCP server | [firmware/mcp.md](firmware/mcp.md) | `FW-MCP` |
| Display, LEDs, attention flashes | [firmware/display.md](firmware/display.md) | `FW-DSP`, `FW-ATT` |
| Effects engine, display options, presets | [firmware/effects.md](firmware/effects.md) | `FW-FX` |
| Time, NTP, time zone, RTC | [firmware/time.md](firmware/time.md) | `FW-TIME` |
| Settings persistence | [firmware/settings.md](firmware/settings.md) | `FW-SET` |
| OTA updates | [firmware/ota.md](firmware/ota.md) | `FW-OTA` |
| Hardware abstraction | [firmware/hardware.md](firmware/hardware.md) | `FW-HW` |
| Board diagram and parts list (reference) | [firmware/board.md](firmware/board.md), [board.svg](firmware/board.svg) | — |
| Memory and size budgets | [firmware/budgets.md](firmware/budgets.md) | `FW-BUD` |
| TypeScript client | [host/client.md](host/client.md) | `HOST-LIB` |
| CLI | [host/cli.md](host/cli.md) | `HOST-CLI` |
| PC MCP server | [host/pc-mcp.md](host/pc-mcp.md) | `HOST-MCP` |
| Verification and traceability | [verification.md](verification.md) | `VER` |
| Open questions | [open-questions.md](open-questions.md) | `OQ` |

## How specs drive work

- Every requirement has a stable ID: `<prefix>-<three digits>`, e.g. `FW-API-012`. IDs are never renumbered or reused; a withdrawn requirement keeps its ID with the status `Withdrawn`.
- Each requirement has a **Status**: `Done` (code and a test exist), `Partial` (code exists, test or part of the behavior missing), `Planned` (no code yet), `Withdrawn`.
- Where the current code differs from the intended behavior, the requirement says so in a **Gap** line. Gaps are the backlog.
- Tests cite the IDs they cover (in the test name or a comment), so `rg FW-API-012` finds the requirement, the code comment and the test. The rule is in [verification.md](verification.md).
- A change to behavior starts with a change to the spec in the same commit, or in a commit before it.
- Keywords **MUST**, **SHOULD**, **MAY** follow RFC 2119.
- Anything not decided is an open question with an `OQ-nn` ID, not a guess. Requirements that depend on one say so.

## Glossary

| Term | Meaning |
|---|---|
| CT-001 | The ClocTeck RGB tube clock (FCC ID 2BGKQ-CT001). |
| Board | "Nixie Clock V4.5.1": ESP8285, 60 WS2812-style LEDs, RTC, 4 buttons, buzzer, header JP1. |
| Stock firmware | The vendor firmware, version 2110 as observed. Unauthenticated HTTP API on port 80, open setup hotspot `ClocTeck_xxxx`. |
| New firmware | The replacement firmware in `firmware/`, this spec's subject. |
| Tube | One of the six digit columns. Each has ten LEDs, one per digit 0–9. |
| Frame | The 60 RGB values sent to the LED chain in one update. |
| Attention (flash) | A temporary color override requested by an agent that ends by itself and never changes saved settings. |
| Effect | A pattern (wave, breathe, fire, …) plus its parameters (palette, direction, period, …), as one JSON object. What the tubes show when no flash is active. |
| Preset | A named effect: built-in (in flash, read-only) or saved by the user on the device. |
| Display options | What the digits do: 12/24 h, leading zero, seconds on/off, digit transition, per-tube brightness, night cap. |
| Token | The bearer secret `FW_API_TOKEN` that every `/api/*` and `/mcp` request must present. |
| Core | `firmware/core/`: hardware-independent C++ that builds on the host and the ESP8285. |
| Emulator | The ESP8266 Arduino core's host emulator (`tests/host`), which runs the sketch as a Linux program. |
| Host software | The Node/TypeScript package in `src/`: client library, `clocteck` CLI, `clocteck-mcp` server. |
| MCP | Model Context Protocol. The device speaks the Streamable HTTP transport. |

## Current layout

- `src/` host software: stock client, new-firmware client, detection, CLI for both; PC MCP server (stock only, frozen)
- `firmware/core/` core library, `firmware/tubeclock/` sketch, `firmware/pinfinder/` pin-finder sketch (FW-HW-004), `firmware/hwtest/` hardware test sketch (FW-HW-010), `firmware/test/` host tests, `firmware/scripts/` build, emulate, test, update, backup and flash scripts, `firmware/third_party/` vendored ArduinoJson
- `clock/` (git-ignored on purpose, OQ-14) a saved copy of the stock `/admin` page (`admin.html`, `admin-app.js`), vendor code used as reference for the stock API, and `clock/backups/`, firmware backups that contain the Wi-Fi password (FW-OTA-008).
- `.env` (git-ignored) holds `FW_WIFI_SSID`, `FW_WIFI_PASSWORD`, `FW_API_TOKEN`, optional `FW_HOSTNAME`, `CLOCTECK_HOST`, `CLOCTECK_MCP_TOKEN`. Values are never printed by any script or tool.
