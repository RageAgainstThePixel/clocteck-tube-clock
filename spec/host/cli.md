# CLI `clocteck` (`HOST-CLI`)

One binary for both firmwares. The command set depends on which firmware answers.

## Options

| ID | Option | Status |
|---|---|---|
| HOST-CLI-001 | `--host <ip or name>` (default `$CLOCTECK_HOST`, else `192.168.18.1`), `--timeout <ms>`, `--json`, `-h/--help`. | Done |
| HOST-CLI-002 | `--token <t>` / `$CLOCTECK_TOKEN` (then `$FW_API_TOKEN`, OQ-17) for the new firmware. Never echoed. | Done |
| HOST-CLI-003 | `--firmware stock\|tubeclock` to skip detection (HOST-LIB-001); default is detect once per invocation. With `--firmware tubeclock` and no `--host`/`$CLOCTECK_HOST`, the host defaults to `tubeclock.local`. `unknown` fails with exit code 1. | Done |
| HOST-CLI-004 | Negative numeric arguments (`timezone -5`) work despite `parseArgs`. | Done |
| HOST-CLI-005 | Errors go to stderr as `clocteck: <message>` with exit code 1; `--json` output is a single JSON document on stdout. | Done |
| HOST-CLI-006 | `status` never prints the Wi-Fi password, even with `--json`. | Done |
| HOST-CLI-007 | A command that the detected firmware does not support fails with `clocteck: "<cmd>" needs the <stock\|new> firmware` and exit code 2. A hint saying what to do instead MAY follow on a second stderr line. | Done |

## Command matrix

| ID | Command | Stock 2110 | New firmware | Status |
|---|---|---|---|---|
| HOST-CLI-010 | `status` | `/config`, `/version`, `/wificonf` (ssid, ip only) | `/api/state` | Done |
| HOST-CLI-011 | `color <1-6\|all> <color>` | `/tubecolor` per tube, `v` from the color | `/api/tube` once (`all` is native); `v` ignored | Done |
| HOST-CLI-012 | `brightness <0-100>` | `/tubecolor` on tube 1 with `v` | `/api/brightness` with `v*2.55` rounded | Done |
| HOST-CLI-013 | `mode <custom\|rainbow\|breathing\|flow>` | `/mode` | unsupported; the hint names `preset use` / `effect set` (HOST-CLI-026/027) | Done |
| HOST-CLI-014 | `digits <normal\|carry\|scramble>` | `/mode` | unsupported | Done |
| HOST-CLI-015 | `hours <12\|24>` | `/tm` | `/api/hours` | Done |
| HOST-CLI-016 | `timezone <-12..12>` | `/time` | converts the offset to a fixed POSIX rule (`UTC-5` → `<-05>5`) and calls `/api/tz`; prints a hint to use `tz` for DST | Done |
| HOST-CLI-017 | `tz <posix rule>` | unsupported | `/api/tz` | Done |
| HOST-CLI-018 | `time [HH:MM[:SS]]` | `/uptm?t=0` | unsupported (FW-TIME-007) | Done |
| HOST-CLI-019 | `sync` | `/uptm?t=1` | unsupported (NTP is automatic); prints whether the time is synced: the state's `lastSync` when reported (FW-API-040), else `time` not `unset` (the state has no `timeSynced` field) | Done |
| HOST-CLI-020 | `wifi scan`, `wifi connect` | `/wifiscan`, `/wificonn` | unsupported (FW-SEC-001) | Done |
| HOST-CLI-021 | `firmware [--update]` | `/version`, `/nVersion`, `/update` | prints `version`; `--update` unsupported (use OTA, FW-OTA-006) | Done |
| HOST-CLI-022 | `attention <color> [--seconds 1-30] [--blink] [--beep]` | via `AttentionController` (snapshot/restore on the PC; `--beep` ignored) | `/api/attention`; returns at once | Done. Stock: the command stays running for the flash and restores on exit or Ctrl+C. |
| HOST-CLI-023 | `attention cancel` | restores the snapshot | `DELETE /api/attention` | Partial. **Gap:** on stock the snapshot lives only in the running `attention` process, so a separate `attention cancel` has nothing to restore; it fails with HOST-CLI-007's message and points at Ctrl+C in the running command. |
| HOST-CLI-024 | `detect` | prints `stock 2110` | prints `tubeclock <version>` (just `tubeclock` when no token is set) | Done |
| HOST-CLI-025 | `restart` | unsupported | `POST /api/restart` | Done (the client side; the route itself is FW-API-018) |
| HOST-CLI-026 | `preset list`, `preset use <name>`, `preset show <name>`, `preset save <name> [--from file.json]` (default: the active effect), `preset delete <name>`, `preset export <name> > file.json` | unsupported | FW-FX-063..067 | Done. `show` and `export` print the JSON either way. |
| HOST-CLI-027 | `effect` (show active), `effect set <pattern> [--palette c1,c2,… \| rainbow] [--direction ltr\|rtl\|center\|edges] [--period ms] [--phase 0-100] [--drift ms] [--easing …] [--pulse min,max[,ms]] [--density n] [--seed n] [--width n] [--count n] [--duty n] [--glyphs on\|off] [--accent style[,ms][,tubes=…][,color=…][,spread=…][,strength=…] \| off]`, `effect json <file\|->` (raw JSON); `--speed 1-100` is sugar for `--period` (100 → 600 ms, 1 → 60 s, log scale; OQ-23) | unsupported | `PUT /api/effect` (FW-FX-061); unknown flags are not sent, the firmware's `400` is shown verbatim | Done. `--speed` with `--period` is an error. In `--palette` a bare whole number starts an `h,s,v` triple. `--density`, `--seed`, `--width`, `--count` and `--duty` are for sparkle, fire, chase and strobe; firmware before them refuses them. The output lists them on an `Options` line when the pattern has them. `--accent` (FW-FX-016) sends only what it names, so the rest of the accent stays; `off` sends `null`. It is shown on an `Accent` line. |
| HOST-CLI-028 | `display [--hours 12\|24] [--leading-zero on\|off] [--seconds on\|off] [--transition cut\|crossfade\|scramble\|carry] [--fade ms] [--tube-brightness a,b,c,d,e,f] [--night HH:MM,HH:MM,level\|off]`; with no flags prints the options | unsupported | `/api/display` (FW-FX-068); `--hours` goes to `/api/hours` | Done. `--tube-brightness` works since the firmware's v1.1 and is shown as `Tube levels`; `--night` is sent, and until it exists the firmware refuses it. |

`--json` output for the new firmware is the state object unchanged, except `detect` (`{firmware, version}`), `restart` (`{ok:true}`) and the commands that have their own answer: `effect`, `preset list`, `preset save`, `preset delete` and `display` print what their route returned.
