# HTTP API (`FW-API`)

Plain HTTP on port 80. All responses except `/health` are `application/json`. Query-string arguments only, except the effect and preset routes in [effects.md](effects.md) §6, which take a JSON body ≤ 1024 bytes (OQ-26). Every successful write returns the full state (FW-API-003), so clients never need a second round trip; the effect/preset routes return the effect instead (they say so).

## Routes

| ID | Method, path | Arguments | Success | Errors | Status |
|---|---|---|---|---|---|
| FW-API-010 | `GET /health` | — | `200 text/plain` body `ok` | — | Done |
| FW-API-011 | `GET /api/state` | — | `200` state object | 401 | Done |
| FW-API-012 | `POST /api/tube` | `t` = `1`–`6` or `all`; `h` 0–360; `s` 0–100 | `200` state, saved | 400, 401 | Done |
| FW-API-013 | `POST /api/brightness` | `v` 0–255 | `200` state, saved | 400, 401 | Done |
| FW-API-014 | `POST /api/hours` | `format` = `12` or `24` | `200` state, saved | 400, 401 | Done |
| FW-API-015 | `POST /api/tz` | `tz` = POSIX TZ rule, 1–47 printable ASCII chars without space, `"`, `'`, `\` | `200` state, saved, NTP reconfigured | 400, 401 | Done |
| FW-API-021 | `POST /api/mode` | — | — | — | Superseded by the effect and preset routes in [effects.md](effects.md) §6 (FW-FX-060..068) |
| FW-API-016 | `POST /api/attention` | `h` 0–360; `s` 0–100; `ms` 1–30000 (default 5000); `blink` 0 or 1 (default 0); `beep` 0 or 1 (default 0, FW-ATT-010) | `200` state with `attention:true`; nothing saved | 400, 401 | Done. `beep` is accepted and validated. The default build drives the buzzer, but the beep has not been heard on the clock (FW-ATT-010, checklist 11). |
| FW-API-017 | `DELETE /api/attention` | — | `200` state with `attention:false` | 401 | Done |
| FW-API-018 | `POST /api/restart` | — | `200 {"ok":true}` then reboot ~100 ms after the response is flushed | 401 | Done (API test; the reboot itself is manual checklist 12; the emulator exits) |
| FW-API-019 | any other path | — | — | `404 {"error":"not found"}` | Done |
| FW-API-020 | known path, wrong method | — | — | `405 {"error":"method not allowed"}` with `Allow` header | Done. Paths under `/api/` check the token first, so without one the answer is `401`, not `405` (FW-SEC-005). |

## State object (FW-API-030)

```json
{
  "version": "0.2.0-3-gabc1234",
  "time": "13:05:09",
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "is24h": false,
  "brightness": 99,
  "preset": "rainbow-wave",
  "effect": { "pattern": "wave" },
  "display": { "hours": 12, "leadingZero": true, "seconds": true, "transition": "cut" },
  "attention": false,
  "tubes": [{ "h": 241, "s": 100 }, { "h": 241, "s": 100 }, { "h": 241, "s": 100 },
            { "h": 241, "s": 100 }, { "h": 241, "s": 100 }, { "h": 241, "s": 100 }],
  "timeSource": "ntp",
  "lastSync": "2026-11-03T17:04:12Z",
  "rejectedSyncs": 0
}
```

| ID | Field | Rule | Status |
|---|---|---|---|
| FW-API-031 | `version` | The build's `git describe --tags --always --dirty` string without the leading `v`, e.g. `0.2.0`, `0.2.0-3-gabc1234` or `0.2.0-dirty`. Passed in by the build scripts as `-DTUBECLOCK_VERSION="..."`; a build without it fails rather than defaulting. The same string is used in `serverInfo.version` (FW-MCP-021) and the boot log. Tag `v0.1.0` exists, so builds read like `0.1.0-7-g1055b61`; a checkout that cannot see a `v` tag gets the bare commit hash, e.g. `fcd978d-dirty`. | Done (VER-013) |
| FW-API-032 | `time` | Local time `HH:MM:SS` (24-hour, regardless of `is24h`), or the string `unset` while the time is unknown (FW-TIME-004). | Done |
| FW-API-033 | `tz` | The saved POSIX TZ rule, echoed verbatim. | Done |
| FW-API-034 | `brightness` | 0–255, the saved value. | Done |
| FW-API-035 | `attention` | `true` while a flash is showing. | Done |
| FW-API-036 | `tubes` | Exactly 6 entries (the clock has six tubes, OQ-19), index 0 = leftmost tube as seen from the front. `h` 0–360, `s` 0–100. Value is always 100; brightness is global. | Done |
| FW-API-037 | `timeSource` | `"ntp"`, `"rtc"` or `"none"`: where the current time came from (FW-TIME-006). | Done (`TimeSync::source`, host-tested; API test). The default build sets the RTC's pins, but only the host test has been seen to reach `"rtc"` (checklist 10). |
| FW-API-038 | `ip`, `rssi`, `uptimeS`, `freeHeap` | Diagnostics. `rssi` in dBm. | Planned |
| FW-API-039 | `hostname` | The compiled-in hostname (`tubeclock`). | Done (`mcp-test.mjs` needs it to send a valid `Host` to the emulator, since VER-005 keeps it from reading `FW_HOSTNAME`) |
| FW-API-041 | `preset`, `effect`, `display` | Per FW-FX-069: `preset` name or `null`, `effect.pattern`, and the display options summary. `tubes` always holds the saved per-tube colors (the `tubes` pattern's palette), not what the effect is showing now. | Done |
| FW-API-040 | `lastSync`, `rejectedSyncs` | Time-sync health (FW-TIME-016): `lastSync` is `"YYYY-MM-DDTHH:MM:SSZ"` or `null`, `rejectedSyncs` an integer. | Done |

## General rules

| ID | Requirement | Status |
|---|---|---|
| FW-API-001 | All numbers in arguments MUST be whole decimal integers within range; anything else (empty, sign only, junk, overflow) is `400`. | Done |
| FW-API-002 | A `400` body is `{"error":"<message>"}` where the message names the valid form, e.g. `need v=0-255`. Messages never echo the offending value (keeps responses bounded and safe to log). | Done |
| FW-API-003 | Every successful write returns the state object exactly as `GET /api/state` would. | Done |
| FW-API-004 | Writes that change saved settings persist them before responding (FW-SET-003). `/api/attention` never persists. | Done |
| FW-API-005 | Argument values are percent-decoded by the server; the `tz` argument MUST be sent URL-encoded by clients (it contains commas and `+`/`-`). | Done |
| FW-API-006 | Responses carry no `Access-Control-*` headers. Browsers are not a supported client (see FW-SEC-012). | Done |
| FW-API-007 | Unknown arguments are ignored. | Done |
| FW-API-008 | `/api/tube` with `t=all` sets all six tubes to the same `h`,`s`. With `t=N` only that tube changes. | Done |
| FW-API-009 | The HTTP responses MUST be well-formed (parsable by strict parsers such as .NET's), unlike the stock firmware. | Done (ESP8266WebServer; the API test's strict `fetch` cites it, VER-012) |

## Example

```bash
curl -H "Authorization: Bearer $FW_API_TOKEN" -X POST "http://tubeclock.local/api/attention?h=30&s=100&ms=3000&blink=1"
```
