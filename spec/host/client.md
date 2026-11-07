# TypeScript client library (`HOST-LIB`)

Package `clocteck-tube-clock` (`src/`), Node ≥ 20, ESM. It talks to both firmwares: `ClocTeckClient` (stock), `TubeClockClient` (new firmware) and `detectFirmware`. Host tests run against a local fake server (`src/fake-server.ts`), not hardware.

## Firmware detection

| ID | Requirement | Status |
|---|---|---|
| HOST-LIB-001 | `detectFirmware(host, timeoutMs)` → `'tubeclock' \| 'stock' \| 'unknown'`: `GET /health` answering `200` with body `ok` means the new firmware; otherwise `GET /version` answering JSON with `ver` means stock; anything else (including a `307` to `/admin` with no `/version`) is `unknown`. A host that doesn't answer `/health` at all is `unknown` without trying `/version`. `host` MAY carry a port (`localhost:9080`). | Done |
| HOST-LIB-002 | Detection never sends the token and never calls a write route. | Done |
| HOST-LIB-003 | Callers MAY skip detection by constructing the specific client directly. | Done |

## Stock client (`ClocTeckClient`)

Wraps the stock firmware 2110 API, every call an unauthenticated GET.

| ID | Requirement | Status |
|---|---|---|
| HOST-LIB-010 | Default host `192.168.18.1` (the setup hotspot); `timeoutMs` default 10 000. | Done |
| HOST-LIB-011 | Uses Node's lenient HTTP parser (`insecureHTTPParser`): the stock responses fail strict parsers. | Done |
| HOST-LIB-012 | `getConfig()` normalizes `/config`: saturation and brightness from 0–255 to 0–100, `mode` 1–4 → `custom\|rainbow\|breathing\|flow`, `outcarry` 0–2 → `normal\|carry\|scramble`, `tz` as UTC offset, `alarm` from `alarmh/alarmm` (or `ah/am`); keeps `raw`. | Done |
| HOST-LIB-013 | `setTubeColor(tube 1–6, {h,s,v})` → `/tubecolor`; `v` sets the global brightness. `setAllTubes` loops the six tubes. `setBrightness(0–100)` keeps tube 1's hue/saturation. | Done |
| HOST-LIB-014 | `setMode(mode?, effect?)` sends both; a missing one is read from `/config` first. | Done |
| HOST-LIB-015 | `setHourFormat`, `setUtcOffset(-12..12)` (sent as `t = offset + 13`), `setTime(Date)` (UTC fields to `/uptm?t=0`), `syncTime()` (`/uptm?t=1`). | Done |
| HOST-LIB-016 | `scanWifi()` polls `/wifiscanover` every 500 ms until not `wait`, sorted by RSSI; `connectWifi(ssid, pwd)` polls `/wificonn?st=1` every 1 s: `3` connected (returns IP from `/wificonf`), `4` wrong password, `1` failed. | Done |
| HOST-LIB-017 | `getWifiConfig()` returns the password the stock firmware leaks; the CLI never prints it (HOST-CLI-006). | Done |
| HOST-LIB-018 | `updateFirmware(onProgress)` starts `/update`, polls `/progress` each second; `255` = failed, `100` = done; connection errors while polling are retried. | Done |
| HOST-LIB-019 | Requests are serialized by callers that need it (the firmware handles one connection at a time); the client itself does not queue. | Done |

## New-firmware client (`TubeClockClient`)

| ID | Requirement | Status |
|---|---|---|
| HOST-LIB-030 | Constructor `{ host, token, timeoutMs }`. `host` defaults to `tubeclock.local`; `token` from the argument, else `CLOCTECK_TOKEN`, else `FW_API_TOKEN` (so a repo `.env` works). Missing token → the constructor throws. The library reads only `process.env`; it does not load `.env` itself (run with `node --env-file=.env` or export the variable). | Done (OQ-17) |
| HOST-LIB-031 | Every `/api` call sends `Authorization: Bearer <token>`; `401` surfaces as `AuthError`, `400` as `ValidationError` with the firmware's message, other non-200 as `HttpError` (`AuthError` and `ValidationError` extend it and carry `status`). | Done |
| HOST-LIB-032 | Methods mirror the routes: `getState()`, `setTube(tube \| 'all', h, s)`, `setBrightness(0–255)`, `setHourFormat(12\|24)`, `setTimeZone(posixRule)`, `attention({h,s}, { ms?, blink?, beep? })`, `cancelAttention()`, `restart()`. Each write resolves with the returned state (`restart()` with `{ok:true}`). `setTube` refuses a tube outside 1–6 before sending; other ranges are left to the firmware's `400`. | Done |
| HOST-LIB-036 | Effects (FW-FX §6): `getEffect()`, `setEffect(partialEffect)`, `listPresets()`, `getPreset(name)`, `applyPreset(name)`, `savePreset(name, effect?)`, `deletePreset(name)`, `getDisplay()`, `setDisplay(options)`. `Effect`, `Preset` and `DisplayOptions` types mirror effects.md; `setEffect` sends JSON and surfaces the firmware's field-level `400` message unchanged. `setDisplay` sends `hours` to `/api/hours` first, since the firmware keeps the hour format there (FW-FX-030). | Done |
| HOST-LIB-033 | `State` type (exported as `TubeClockState`) matches FW-API-030 field for field, including the FW-API-037..040 fields as optional (of these only FW-API-038 is still Planned); unknown extra fields are kept. | Done |
| HOST-LIB-034 | Uses `fetch` (strict parser is fine: FW-API-009). | Done |
| HOST-LIB-035 | The token never appears in error messages or thrown stacks. It is held in a private class field, so `util.inspect` and `JSON.stringify` of the client don't show it either. | Done |

## Shared

| ID | Requirement | Status |
|---|---|---|
| HOST-LIB-020 | `color.ts`: `hexToHsv`, `hsvToHex`, `parseColor` accepting a name from `NAMED_COLORS` (`red orange yellow green cyan blue purple magenta pink white`), `#rrggbb`, `#rgb` or `h,s,v`. The same table and grammar are used by the firmware (FW-MCP-036). | Done |
| HOST-LIB-021 | Public exports: everything in `attention.ts`, `client.ts`, `color.ts`, `tubeclock.ts` and `detect.ts` (`src/index.ts`). | Done |
