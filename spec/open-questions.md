# Open questions (`OQ`)

Undecided points, and the record of how each was decided. A decided question keeps its ID; the affected requirements carry the decision. OQ-02 (hardware measurements) and OQ-28 (a schedule) are still open.

| ID | Question | Decision | Affects |
|---|---|---|---|
| OQ-01 | Change Wi-Fi/token without a rebuild? | **Decided 2026-11-03: no.** Edit `.env`, rebuild, flash over OTA (serial over JP1 as fallback). No runtime provisioning of any kind. | FW-SEC-019, provisioning flow |
| OQ-02 | Hardware probing results: LED data GPIO, chain and digit order, color order (GRB?), RTC SDA/SCL GPIOs, button GPIOs and pull direction, buzzer GPIO and polarity, JP1 pinout, LDO output. The measurement list is in [firmware/board.md](firmware/board.md#what-must-be-confirmed-oq-02). | **Open, mostly answered 2026-11-07.** Measured: JP1 pinout except pin 5, LED data GPIO 13, GRB, chain and digit order, RTC, button and buzzer GPIOs and levels (FW-DSP-002, FW-HW-010, [firmware/hardware.md](firmware/hardware.md)). Still open: JP1 pin 5 and the LDO output voltage. Already observed: six tubes, LED51–60 under the leftmost tube, RTC is a GC1307. | FW-DSP-002, FW-HW-002..005, FW-TIME-006, FW-ATT-010/011 |
| OQ-03 | Backoff on repeated `401`s? | **Decided 2026-11-03: yes.** Per client IP, after 5 failures in 60 s each further `401` is delayed 1 s; resets 60 s after the last failure; never a permanent lockout. | FW-SEC-014 |
| OQ-04 | RTC support and manual time-set? | **Decided 2026-11-03: use the RTC** (read at boot, write after each NTP sync, stores UTC). No manual time-set route. | FW-TIME-006, FW-TIME-007, FW-API-037 |
| OQ-05 | `POST /api/restart`? | **Decided 2026-11-03: yes**, authenticated. | FW-API-018, HOST-LIB-032, HOST-CLI-025 |
| OQ-06 | May agents change persistent settings over MCP? | **Decided 2026-11-03: no.** Attention and status tools only. | FW-MCP-033 |
| OQ-07 | Port stock animations / digit effects? | **Decided 2026-11-03: superseded.** Instead of porting modes, a data-driven effects engine with built-in and user presets ([firmware/effects.md](firmware/effects.md)); the stock digit effects are optional transitions only. | FW-FX, FW-DSP-010/025, HOST-CLI-013/014/026..028 |
| OQ-08 | Display before the time is known? | **Decided 2026-11-03: blank tubes**, not `00:00:00`. Attention flashes still show. | FW-DSP-007, FW-TIME-004 |
| OQ-09 | Default time zone? | **Decided 2026-11-03: `EST5EDT,M3.2.0,M11.1.0`.** | FW-TIME-002, FW-SET-002 |
| OQ-10 | Which MCP protocol versions to list? | **Decided 2026-11-03: `2025-03-26`, `2025-06-18`, `2025-11-25`.** `2026-07-28` is a different protocol (no `initialize` handshake) and stays out until an agent actually needs it. | FW-MCP-020 |
| OQ-11 | JSON on the device? | **Decided 2026-11-03: ArduinoJson.** | FW-MCP-040, FW-BUD-003 |
| OQ-12 | OTA upload tool without Python? | **Decided 2026-11-03: Node-based** (`firmware/scripts/ota.mjs` espota client, or `arduino-cli upload --protocol network` if Python-free). | FW-OTA-006 |
| OQ-13 | Serial backup/flash tool without Python? | **Decided 2026-11-03: `esptool-js` (Node) or the standalone `esptool.exe` release.** | FW-OTA-008, FW-HW-008 |
| OQ-14 | Commit the saved stock `/admin` page? | **Decided 2026-11-03: no.** Vendor code; `clock/` is git-ignored. | README layout, `.gitignore` |
| OQ-15 | Button behavior in v1? | **Decided 2026-11-03: any button press cancels an active attention flash.** Nothing else. | FW-HW-006, FW-ATT-011 |
| OQ-16 | Beep on attention? | **Decided 2026-11-03: yes, optional `beep` argument, off by default**, on the HTTP route and the MCP tool. | FW-ATT-010, FW-API-016, FW-MCP-030, FW-HW-007 |
| OQ-17 | Host-side token env var? | **Decided 2026-11-03: `CLOCTECK_TOKEN`, falling back to `FW_API_TOKEN`.** | HOST-LIB-030, HOST-CLI-002 |
| OQ-18 | PC MCP server vs the new firmware? | **Decided 2026-11-03: leave it alone.** Separate, stock-only, frozen; no detection, refusal or proxy. May be open-sourced separately. | HOST-MCP-010 (withdrawn) |
| OQ-19 | Does the CT-001 have six tubes? | **Decided 2026-11-03: six.** The `src/client.ts` comment now says six. | FW-DSP-001, FW-API-036 |
| OQ-20 | Firmware versioning? | **Decided 2026-11-03: from git**, `git describe --tags --always --dirty` at build time; no hand-edited `kVersion`. | FW-API-031, FW-OTA-009, VER-013 |

## Effects engine (raised and decided 2026-11-03)

| ID | Question | Decision | Affects |
|---|---|---|---|
| OQ-21 | Where do effects, display options and user presets live? | **Decided 2026-11-03: a 64 KB LittleFS partition** (board layout `1M (FS:64KB OTA:~470KB)`). JSON files are the preset format anyway; 16 × 1 KB presets do not fit the 4 KB EEPROM sector. Cost: ~10 KB of code (measured later: about 26 KB, LittleFS and the FS layer) and an image budget of 470 KB. A file per preset turned out not to fit: see FW-FX-042. | FW-FX-050, FW-BUD-001, FW-HW-009 |
| OQ-22 | Default preset on a fresh device? | **Decided 2026-11-03: `rainbow-wave`.** `nixie` and `classic-blue` are one command away. | FW-FX built-ins table, FW-SET-002 |
| OQ-23 | Speed as `period` in ms or an abstract `speed`? | **Decided 2026-11-03: `period` in ms** (one cycle per tube); the CLI may offer `--speed 1–100` as sugar mapping to 60 s–0.6 s. | FW-FX-005, HOST-CLI-027 |
| OQ-24 | Allow a digit `crossfade` that briefly lights two LEDs in one tube? | **Decided 2026-11-03: yes**, opt-in (`cut` stays default), ≤ 1000 ms per transition; the display rule is "one *digit* per tube". | FW-FX-033, FW-DSP-001 |
| OQ-25 | Night schedule in v1 or v1.1? | **Decided 2026-11-03: v1.1** (needs solid local time first). **Superseded 2026-11-07:** the night schedule was dropped (FW-FX-035 withdrawn); see OQ-28. | FW-FX-035 |
| OQ-26 | JSON request bodies on the effect/preset routes? | **Decided 2026-11-03: yes**, `application/json` ≤ 1024 B on those routes only; everything else stays query-only. | FW-FX §6, FW-BUD-004, FW-API intro, FW-SEC-013 |
| OQ-27 | Which patterns ship in v1 vs v1.1? | **Decided 2026-11-03.** v1: solid, tubes, wave, gradient, breathe, presets, display basics (leading zero, seconds, cut/crossfade). v1.1: sparkle, chase, fire, strobe, per-tube brightness, night, scramble/carry transitions. All of v1.1 shipped in v0.2.0 except night, which was withdrawn; matrix and accents were added. | FW-FX status column |
| OQ-28 | A schedule that changes what the clock shows at chosen times? | **Open (raised 2026-11-07).** Instead of a night brightness cap, the user may want entries the user sets up, each naming a time (or days, or an event) and what to apply then: a preset, an effect, display settings or brightness. Not designed yet: what can trigger an entry, how many entries fit, where they are stored, and how the CLI and API set them. | FW-FX-035 (withdrawn), FW-FX §3 |
