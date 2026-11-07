# Memory and size budgets (`FW-BUD`)

The ESP8285 has 1 MB flash and about 80 KB RAM, of which roughly 40–50 KB heap is free once Wi-Fi is up. OTA needs two images to fit in flash.

| ID | Budget | Status |
|---|---|---|
| FW-BUD-001 | Sketch image ≤ 470 KB: the board layout is `1M (FS:64KB OTA:~470KB)` so a 64 KB LittleFS fits beside two OTA slots (FW-FX-050). `build.sh` prints the size; CI/the build fails above the limit. | Done (`build.sh`; with the effects engine images are about 433 KB, about 439 KB with every pin flag set, against the 481280 limit). |
| FW-BUD-002 | Free heap after Wi-Fi + web server + mDNS + OTA is up: ≥ 20 KB at idle, measured by `freeHeap` in the state (FW-API-038) on hardware. | Planned |
| FW-BUD-003 | The largest contiguous allocation per request (JSON document for MCP) ≤ 4 KB. | Done: a 4096-byte JSON buffer, plus the body (≤ 2049) and response (3072) buffers, about 9.2 KB in all, freed after each request. Overflowing the JSON buffer is a `413` (`mcp-test.mjs`). `clock_status` pretty-prints the state without parsing it, since a parsed state (FW-FX-069) would crowd the request out of the buffer. |
| FW-BUD-004 | MCP request body ≤ 2048 bytes; effect/preset JSON bodies ≤ 1024 bytes (`413` otherwise). Other HTTP API requests use no body; one over 1024 bytes is refused the same way (FW-SEC-013). | Done (`mcp-test.mjs` checks 2048 and 2049 bytes, `api-test.mjs` 1024 and 1025, on the JSON routes and on the others, FW-SEC-013). |
| FW-BUD-010 | Effects engine: ≤ 5 ms CPU per 50 ms frame, < 1 KB steady-state RAM, ≤ 24 KB code + 8 KB preset data (FW-FX-072..074). | Partial: RAM and flash met by the build's map (FW-FX-073/074). **Gap:** frame time has been measured only on the emulator, not on the clock (checklist 15). |
| FW-BUD-005 | Any response ≤ 3072 bytes. The state object is ~450 bytes (at most 640); `tools/list` is the largest response and its schema strings live in PROGMEM. | Done for MCP: the response buffer is the limit, and `tools/list` is 1811 bytes. `GET /api/presets` with 16 saved presets stays under the limit (API test). |
| FW-BUD-006 | One HTTP client at a time; the server's accept backlog MAY queue others. No request handler takes longer than 200 ms, so the 50 ms display loop stalls at most 4 frames. | Partial |
| FW-BUD-007 | Settings blob 80 bytes; EEPROM sector reserved 4 KB. | Done |
| FW-BUD-008 | No dynamic `String` concatenation in loops that grow per LED; frame rendering is a fixed 180-byte array. | Done |
| FW-BUD-009 | Stack: handlers avoid >1 KB local arrays (the Arduino loop task stack is 4 KB). | Done (rule; review on each handler) |
