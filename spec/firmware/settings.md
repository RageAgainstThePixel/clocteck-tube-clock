# Settings persistence (`FW-SET`)

Settings are the values a user changes and expects back after a power cycle. They live in the EEPROM emulation sector (4 KB of flash) as one 80-byte blob.

| ID | Requirement | Status |
|---|---|---|
| FW-SET-001 | `Settings` = six tube `{h 0–360, s 0–100}`, `brightness` 0–255, `is24Hour`, `tz` (≤ 47 chars + NUL). Effects and display options live elsewhere (FW-SET-014). | Done |
| FW-SET-002 | Defaults: every tube `h=241 s=100`, `brightness=99`, 12-hour, `tz=EST5EDT,M3.2.0,M11.1.0`. The default *effect* on a device with no `/effect.json` is the `rainbow-wave` preset (OQ-22, decided), so the blue tube colors only show after `preset use classic-blue` or a `/api/tube` write. | Done |
| FW-SET-003 | Every API write that changes a setting encodes and commits the blob before the response is sent. Attention flashes never write. | Done |
| FW-SET-004 | Blob layout (80 bytes, little-endian): `'T' 'C' <version=1>`, 6 × (`h` lo, `h` hi, `s`), `brightness`, `is24Hour` (0/1), `tz[48]`, zero padding, FNV-1a-32 checksum of bytes 0–75 in bytes 76–79. | Done |
| FW-SET-005 | Decoding MUST reject: wrong magic or version, checksum mismatch, any field out of range, `tz` not NUL-terminated or failing `isValidTz`. On rejection the firmware uses defaults and logs `[settings] none saved, using defaults`. | Done |
| FW-SET-006 | A blob version bump MUST come with a migration or an explicit "reset to defaults" note in this spec. | Done (rule) |
| FW-SET-007 | Flash wear: a commit happens only on a successful write that changed a value. Clients that spam identical writes SHOULD still not cause commits. | Done: handlers snapshot the settings, `sameSettings` (host-tested) compares the encoded blobs, and only a change commits and logs `[settings] saved`. (The ESP8266 EEPROM library already skipped writes of unchanged bytes, so the old gap was smaller than stated; the check is now explicit and covers the emulator.) |
| FW-SET-010 | `tz` is stored as given, up to 47 characters. | Done |
| FW-SET-011 | `isValidTz`: 1–47 characters, each in `!`..`~` excluding `"`, `'`, `\`. That covers every POSIX rule and keeps the value safe to echo in JSON without escaping. | Done |
| FW-SET-012 | `parseInt`: whole decimal integers, optional leading `-`, no whitespace, rejects overflow, range-checked. | Done |
| FW-SET-013 | Settings are stored at EEPROM offset 0. Any future blob (e.g. an LED layout override, OQ-02) gets its own offset and magic so it can be reset independently. | Done (rule) |
| FW-SET-014 | Effects, display options and presets are **not** in the EEPROM blob: they are JSON files on LittleFS (FW-FX-050, OQ-21). The blob stays at version 1 with tz, hours, brightness and the six tube colors (which are the `tubes` pattern's palette, FW-FX-021). The earlier plan to add a color mode to a blob v2 is withdrawn. | Superseded by FW-FX-050 |
