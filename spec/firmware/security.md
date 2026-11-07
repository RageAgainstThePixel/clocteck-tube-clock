# Wi-Fi, provisioning and security model (`FW-SEC`)

## Threat model

The attacker is on the same LAN or within radio range. They can send any HTTP request, sniff unencrypted traffic, and try DNS rebinding through a browser on the user's machine. They do not have the board in hand: anyone with physical access can read flash, and that is accepted (FW-SEC-010).

## Requirements

| ID | Requirement | Status |
|---|---|---|
| FW-SEC-001 | The firmware runs in station mode only. It MUST never start an access point, including during provisioning or after a failed connection. | Done |
| FW-SEC-002 | Wi-Fi credentials and the API token are compiled in from `firmware/tubeclock/secrets.h`, which `scripts/gen-secrets.sh` generates from `.env`. `secrets.h` and `.env` are git-ignored. The build MUST fail when `secrets.h` is missing. | Done |
| FW-SEC-003 | No route, log line, MCP response or error message ever contains the Wi-Fi password or the token. The SSID MAY appear in the serial log. | Done |
| FW-SEC-004 | `WiFi.persistent(false)` so the SDK does not copy credentials into its own flash sector. | Done |
| FW-SEC-005 | Every route under `/api/` and the `/mcp` endpoint MUST require `Authorization: Bearer <token>`. `/health` is the only open route. | Done |
| FW-SEC-006 | Token comparison MUST be constant-time and MUST use the header's explicit length. (The ESP8266 core's `String::clear()` keeps old bytes, so a request with no header must not match the previous request's token.) | Done |
| FW-SEC-007 | A missing or wrong token returns `401` with `WWW-Authenticate: Bearer` and body `{"error":"unauthorized"}`. The response MUST NOT say whether the header was missing, malformed or wrong. | Done |
| FW-SEC-008 | The token MUST NOT be accepted from a query parameter, cookie or request body. | Done (not implemented, so not accepted). `mcp-test.mjs` checks the query parameter; cookie and body still have no test. |
| FW-SEC-009 | An empty compiled-in token MUST reject every authenticated request (never "no token means open"). | Done |
| FW-SEC-010 | Physical access is out of scope: credentials are in flash in plain form. The README says so. | Done |
| FW-SEC-011 | Every stock route (`/admin`, `/config`, `/wificonf`, `/wifiscan`, `/wificonn`, `/tubecolor`, `/mode`, `/tm`, `/time`, `/uptm`, `/version`, `/nVersion`, `/update`, `/progress`) MUST answer `404` with no data. | Done |
| FW-SEC-012 | Requests to `/mcp` MUST pass a Host/Origin check (see FW-MCP-010, FW-MCP-011) before the token check, so a browser on the LAN cannot be used as a relay. | Done |
| FW-SEC-013 | The web server serves one client at a time; request bodies over the limit in FW-BUD-004 are rejected with `413` before parsing. Bodies are only used on `/mcp` and the effect/preset routes (OQ-26); every other route ignores any body up to the 1024-byte limit, which the web server reads and drops. A body the server would read without regard to its length is refused: chunked or with a malformed length `411`, multipart `415`. Headers the check cannot see in the first TCP segment, or over 768 bytes, are `431`. | Done, on every path and for every method that makes `ESP8266WebServer` read a body (POST, PUT, PATCH, DELETE; a GET reads none and is not checked). `/mcp` reads the request itself; the others peek at the headers from the server's hook (`bodyFits`, core `McpHeaders::addLines` and `checkBodyHeaders`, which read lines as the server will, so a stray CR cannot hide a length; host-tested, API test). Under `/api/` the token is checked first, `401` before `413`/`415`; elsewhere no token is asked for, so a stock route (FW-SEC-011) with a large body answers `413` instead of `404`. A form-encoded body within the limit is still merged into the arguments by the web server, as it always was. |
| FW-SEC-014 | Auth backoff, per client IP: after 5 failed auth attempts within 60 s, every further `401` from that IP is delayed by 1 s (the handler sleeps before responding; the display loop still runs because the delay is yielded). The counter resets 60 s after the last failure; a successful auth clears it. Never a permanent lockout. The table holds at most 4 IPs (oldest evicted). A missing header counts as a failure like a wrong one (FW-SEC-007 does not tell them apart). | Done (core `AuthBackoff`, host-tested; API test VER-014). The wait keeps rendering frames, so the tubes keep ticking; it is the one deliberate exception to FW-BUD-006's 200 ms handler limit. |
| FW-SEC-015 | OTA MUST require the same token as the API (see FW-OTA-002). | Done |
| FW-SEC-016 | mDNS advertises only `_http._tcp` for the web server and `_arduino._tcp` for OTA (FW-OTA-001). It MUST NOT include TXT records with any secret or the Wi-Fi SSID. | Done |
| FW-SEC-017 | The firmware MUST reconnect by itself after Wi-Fi drops (`setAutoReconnect(true)`), and MUST keep driving the display meanwhile. | Done |
| FW-SEC-018 | Serial output is for debugging and MAY be left enabled; it never prints secrets (FW-SEC-003). | Done |

## Provisioning flow (intended)

1. User fills `.env` with SSID, password, token (a random string of 24+ characters; the README SHOULD show how to generate one).
2. `scripts/build.sh` regenerates `secrets.h` and builds the image.
3. First flash over JP1 serial (see [hardware.md](hardware.md)); later flashes over OTA ([ota.md](ota.md)).
4. The clock joins the network, answers at `tubeclock.local` and prints its IP on serial.

Changing Wi-Fi or the token means editing `.env`, rebuilding and flashing over OTA (serial over JP1 as the fallback). That is intentional: no runtime provisioning path means no provisioning attack surface (OQ-01, decided).

| ID | Requirement | Status |
|---|---|---|
| FW-SEC-019 | There is no runtime provisioning: no route, serial command, button combo or hotspot can change Wi-Fi credentials or the token. | Done |
| FW-SEC-020 | No security decision depends on wall-clock time (NTP is unauthenticated; see [time.md](time.md#time-sync-hardening)). Auth, OTA and backoff use the token and `millis()` only. | Done (rule) |
