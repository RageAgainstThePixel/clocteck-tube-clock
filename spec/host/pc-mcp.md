# PC MCP server `clocteck-mcp` (`HOST-MCP`)

Runs on a PC and drives a **stock-firmware** clock. With the new firmware, agents connect to the device's own MCP endpoint ([firmware/mcp.md](../firmware/mcp.md)) and this server is not needed.

**Frozen (OQ-18, decided 2026-11-03):** this server stays as it is, for the stock firmware only. It gets no new-firmware detection, refusal or proxying, and no further features; only bug fixes. It may be split out and open-sourced separately later. Nothing in the new-firmware specs depends on it.

| ID | Requirement | Status |
|---|---|---|
| HOST-MCP-001 | Streamable HTTP endpoint at `http://<bind>:<port>/mcp`; `GET /health` → `ok`; other paths `404`. Defaults: bind `0.0.0.0`, port `3939` (`$CLOCTECK_MCP_PORT`). Built on `@modelcontextprotocol/server`. | Done |
| HOST-MCP-002 | `--token` / `$CLOCTECK_MCP_TOKEN` requires `Authorization: Bearer`; `401` + `WWW-Authenticate: Bearer` otherwise. Without a token the server prints a warning that anyone on the LAN can flash the clock. | Done |
| HOST-MCP-003 | DNS-rebinding guard: `Host` must be an IP, `localhost`, the machine's hostname (plus `.local`/`.lan`) or a `--allow-host` value; otherwise `403`. Checked before auth. | Done |
| HOST-MCP-004 | Tool `get_attention(color='orange', durationSeconds 1–30 = 5, blink=false, message ≤ 500)`: snapshots `/config` when idle, switches to `custom` mode if needed, sets all tubes, optional 500 ms blink by toggling `v` between the color's value and 5, restores after the duration. Result text names the restore time. | Done |
| HOST-MCP-005 | Tool `clock_status()`: `/config` normalized plus `flashing`. | Done |
| HOST-MCP-006 | A flash during a flash takes over the timer; the restore target is always the user's own settings, never a previous flash. Clock requests are serialized. | Done |
| HOST-MCP-007 | Restore retries 3 times, 2 s apart; on final failure the snapshot is kept so the next flash still restores correctly. | Done |
| HOST-MCP-008 | `SIGINT`/`SIGTERM` restore the clock before exit. | Done |
| HOST-MCP-009 | Tool calls are logged with caller IP, color, duration and message; the token is never logged. | Done |
| HOST-MCP-010 | The server knows nothing about the new firmware: no detection, refusal or proxying. Pointed at a new-firmware clock it simply fails on the first stock request. | Withdrawn (OQ-18) |
| HOST-MCP-011 | The device's tools (FW-MCP-030..032) keep the PC server's `get_attention` and `clock_status` names, argument names and defaults so existing agent prompts carry over. The PC server is not changed to match the device (it lacks `cancel_attention`, and that is accepted). | Done (rule on the device side) |
