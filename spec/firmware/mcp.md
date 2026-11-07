# On-device MCP server (`FW-MCP`)

Agents connect straight to the clock. The device implements the MCP **Streamable HTTP** transport in its simplest allowed form: one endpoint, JSON responses, no SSE, no sessions, no server-initiated messages. Every requirement here is **Done** on the emulator (protocol logic in the core `clock_core/mcp.h`, tested by `firmware/test/core_tests.cpp` and `firmware/scripts/mcp-test.mjs`) except as listed under [Status](#status); the firmware runs on a real clock, but MCP has not been tried there yet (checklist 17).

## Transport

| ID | Requirement |
|---|---|
| FW-MCP-001 | Endpoint is `POST /mcp` on port 80, the same server as the HTTP API. |
| FW-MCP-002 | `GET /mcp` returns `405` (no server-to-client stream). `DELETE /mcp` returns `405` (no sessions). |
| FW-MCP-003 | The request body is one JSON-RPC 2.0 message. JSON-RPC batch arrays are rejected with `400` and a JSON-RPC error `-32600`. |
| FW-MCP-004 | A request (has `id`) is answered with `200 Content-Type: application/json` and one JSON-RPC response object. The server never answers with `text/event-stream`. |
| FW-MCP-005 | A notification or a client response (no `id`, or a `result`/`error` message) is answered with `202 Accepted` and an empty body. |
| FW-MCP-006 | The `Accept` header is not enforced (clients send `application/json, text/event-stream`; the server always picks JSON). |
| FW-MCP-007 | Header `MCP-Protocol-Version`: if present it MUST be one of the supported versions (FW-MCP-020), else `400`. If absent the server assumes `2025-03-26`, as the spec requires. |
| FW-MCP-008 | No `Mcp-Session-Id` is issued; one sent by a client is ignored. The server is stateless between requests. |
| FW-MCP-009 | Auth: `Authorization: Bearer <token>` is required, checked exactly as for `/api/*` (FW-SEC-005..009). Missing or wrong → `401` + `WWW-Authenticate: Bearer`, before the body is parsed. |
| FW-MCP-010 | `Host` header MUST be the device's IP, `<hostname>`, or `<hostname>.local` (any port suffix stripped). Otherwise `403`. |
| FW-MCP-011 | If an `Origin` header is present it MUST be `http://` + an allowed Host (FW-MCP-010). Otherwise `403`. Requests without `Origin` (non-browser clients) are allowed. |
| FW-MCP-012 | Body larger than FW-BUD-004 → `413`. Unparseable JSON → `400` with a JSON-RPC error `-32700`, `id: null`. |
| FW-MCP-013 | Each tool call is logged on serial as `[mcp] <client ip> <tool> <summary>`; never the token. |

## Protocol

| ID | Requirement |
|---|---|
| FW-MCP-020 | Supported protocol versions: `2025-03-26`, `2025-06-18`, `2025-11-25`, the handshake-based versions. The list is a single constant, `kMcpVersions` in the core. `2026-07-28` is a different protocol (no `initialize`; `server/discover`, per-request `_meta`, mirrored headers, new error codes) and is deliberately **not** offered until an agent the user runs actually needs it (OQ-10, decided). Dual-era clients fall back to `2025-11-25`. |
| FW-MCP-021 | `initialize` → `result.protocolVersion` = the client's requested version if supported, else the newest supported one; `capabilities: { "tools": {} }` (no `listChanged`); `serverInfo: { "name": "tubeclock", "version": <firmware version> }`; `instructions`: one paragraph telling agents the clock is a physical light on the user's desk and to use `get_attention` sparingly. |
| FW-MCP-022 | `notifications/initialized` → `202`. The server does not require it before serving other methods. |
| FW-MCP-023 | `ping` → `result: {}`. |
| FW-MCP-024 | `tools/list` → the fixed tool list in the next section. `cursor` is ignored; `nextCursor` is never set. |
| FW-MCP-025 | `tools/call` → `result.content` is one `text` item; `result.isError: true` for a tool-level failure (bad color, clock unavailable). Protocol-level failures (unknown tool, missing required argument of the wrong type) are JSON-RPC errors `-32602`. |
| FW-MCP-026 | Unknown method → JSON-RPC error `-32601`. `resources/*`, `prompts/*`, `logging/*`, `completion/*` are not advertised and return `-32601`. |
| FW-MCP-027 | JSON-RPC errors use HTTP `200` except parse errors (`400`, FW-MCP-012). `id` is echoed; strings and integers are both accepted. |

## Tools

| ID | Tool | Arguments | Result text | Annotations |
|---|---|---|---|---|
| FW-MCP-030 | `get_attention` | `color` string, default `orange`: a name (`red orange yellow green cyan blue purple magenta pink white`), `#rrggbb`, `#rgb`, or `h,s,v`; `durationSeconds` number 1–30, default 5; `blink` boolean, default false; `beep` boolean, default false (FW-ATT-010); `message` string ≤ 200 chars, logged on serial | `Clock is showing #ff6a00[, blinking,][ with a beep,] for 5 s.` | `readOnlyHint:false, destructiveHint:false, idempotentHint:true, openWorldHint:false` |
| FW-MCP-031 | `cancel_attention` | — | `Attention cancelled.` (also when none was showing) | `idempotentHint:true` |
| FW-MCP-032 | `clock_status` | — | The state object from FW-API-030 as pretty JSON | `readOnlyHint:true` |
| FW-MCP-033 | MCP exposes attention and status only. There are no tools that change saved settings (colors, brightness, hours, tz) and none that restart the device (OQ-06, decided). | |
| FW-MCP-034 | `inputSchema` for each tool is a JSON Schema object with `type: "object"`, `properties`, `required` and `additionalProperties: false`; served from PROGMEM as a literal string, not built at runtime. | |
| FW-MCP-035 | A `get_attention` while a flash is showing replaces it (same rule as FW-ATT-006). | |
| FW-MCP-036 | `color` parsing lives in the core (`parseColor`), shares the named-color table with the host software (HOST-LIB-020), and is unit-tested. | |

## Example exchange

```http
POST /mcp HTTP/1.1
Host: tubeclock.local
Authorization: Bearer <token>
Content-Type: application/json
MCP-Protocol-Version: 2025-06-18

{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_attention","arguments":{"color":"red","durationSeconds":3}}}
```

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"Clock is showing #ff0000 for 3 s."}]}}
```

Client registration (Claude Code):

```bash
claude mcp add --transport http tube-clock http://tubeclock.local/mcp --header "Authorization: Bearer <token>"
```

## Limits

See [budgets.md](budgets.md): request body ≤ 2048 bytes, response ≤ 3072 bytes, one request in flight.

| ID | Requirement |
|---|---|
| FW-MCP-040 | JSON is parsed and produced with **ArduinoJson** (OQ-11, decided), using a fixed-capacity document sized for the request limit (FW-BUD-003). The HTTP API's hand-built state string MAY stay as it is. |

## Status

ArduinoJson 7.4.3 is vendored at `firmware/third_party/ArduinoJson` (MIT). Version 7 has no fixed-capacity document, so the documents use a bump allocator over a 4 KB per-request buffer (`FixedArena`), which gives the same bound.

| ID | Status |
|---|---|
| FW-MCP-013 | Partial. The summary text is core-tested; no automated check reads the serial log. Checked by hand on the emulator: `[mcp]` lines appear and the token does not. The emulator prints the client IP as `(IP unset)`. |
| FW-MCP-020 | Done. `2026-07-28`-only clients cannot connect, by decision (OQ-10); the official client in `auto` mode ends on `2025-11-25` (`mcp-test.mjs`). |
| FW-MCP-030 | Done. `beep` is accepted and logged, and drives nothing while `TUBECLOCK_BUZZER_PIN` is unset (FW-ATT-010). |
| FW-MCP-036 | Done. The table is a C++ copy, not shared code; `mcp-test.mjs` compares it with `src/color.ts`, allowing 1 per channel (the two round `.5` differently). The `h,s,v` form takes whole numbers only on the clock. |
| FW-MCP-040 | Done for parsing and the `initialize` and `clock_status` results. The envelope and tool text are written directly, and `tools/list` is copied from PROGMEM (FW-MCP-034). |
| Hardware | MCP not yet run on a clock (checklist 17): request timing (FW-BUD-006) and peak heap (FW-BUD-002) are unmeasured. |

Decisions where this spec was silent or two rows pulled apart:

- A body that is not a JSON-RPC 2.0 message (not an object, no `"jsonrpc":"2.0"`, a bad `id`) is `400` with `-32600`, like a batch (FW-MCP-003). FW-MCP-027's `200` applies once the message is a valid request.
- A body under the size limit that still overflows the JSON buffer is `413`, not a parse error.
- Requests with `Transfer-Encoding` or no `Content-Length` are `411`; a header line over 255 bytes or more than 40 lines is `431`. String ids are limited to 64 characters of JSON.
- A wrong type is `-32602`; a bad value of the right type (color, `durationSeconds` out of range, `message` too long) is `isError`. `durationSeconds` may be fractional. Unknown arguments are ignored.
- `get_attention` reports the color shown, always at value 100: the `v` of `h,s,v` and the darkness of a hex color are dropped, because brightness is global (FW-DSP-004).
- With `beep:true` and no buzzer the reply replaces the template's "with a beep" with `No beep: this clock has no buzzer configured.`, so an agent is not told something happened that did not.
- `/mcp` is handled from the web server's hook, before the server reads the request, because the server would otherwise buffer a body of any size. Each answer closes the connection. Host and Origin are checked for every method, so a bad Origin on `GET /mcp` is `403`, not `405`.
