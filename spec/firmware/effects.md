# Effects, display options and presets (`FW-FX`)

Everything the tubes show, apart from the digits themselves and attention flashes, is driven by **data**: an *effect* (a pattern plus parameters), *display options* (what the digits do) and *presets* (named effects, built-in or saved by the user). No pattern is a one-off mode; adding a preset is adding JSON, not code. The status column says what shipped in v1 and what follows in v1.1 (OQ-27, decided); v1.1 rows are still Planned, and until then their fields and patterns are refused as unknown (FW-FX-013). What can only be seen on the tubes waits for checklist 14..16.

Supersedes the `wave` color mode rows FW-DSP-020..028, FW-API-021/041 and FW-SET-014 (their IDs stay, marked superseded). The left-to-right wave the user asked for is the built-in preset `rainbow-wave`, the default (OQ-22, decided). The design choices OQ-21..27 were all decided 2026-11-03 as written here.

## 1. Effect model

An effect is one JSON object. Every field has a default, so `{"pattern":"wave"}` is valid.

```json
{
  "pattern": "wave",
  "palette": ["#ff0000", "#ffa500", "#ffff00"],
  "interpolate": "hsv",
  "direction": "ltr",
  "period": 18000,
  "phase": 8,
  "drift": 0,
  "easing": "linear",
  "pulse": { "min": 64, "max": 255, "period": 4000, "easing": "sine" },
  "density": 20,
  "width": 1,
  "duty": 50
}
```

| ID | Field | Rule | Status |
|---|---|---|---|
| FW-FX-001 | `pattern` | One of the patterns in §2. Required. | v1. Done |
| FW-FX-002 | `palette` | Either the string `"rainbow"` (the full hue wheel, saturation 100) or an array of 1–8 colors. A color is `"#rrggbb"`, `"#rgb"`, `"h,s,v"` or a name from the shared table (HOST-LIB-020). Default `["#ffa030"]` (nixie amber). A gradient stop MAY be written as `{"at": 0-100, "color": ...}`; without `at`, stops are evenly spaced with the last wrapping to the first. | v1. Done. The colors are kept and answered as `#rrggbb`; with a mix of placed and unplaced stops an unplaced one takes its even place, and places must not decrease (`400`). |
| FW-FX-003 | `interpolate` | `hsv` (default, shortest hue path) or `rgb`. How colors between stops are mixed. | v1. Done |
| FW-FX-004 | `direction` | `ltr` (default), `rtl`, `center` (starts at tubes 3–4 and moves outward), `edges` (starts at tubes 1 and 6 and moves inward). Defines each tube's *distance* `d(i)`: `ltr` 0..5, `rtl` 5..0, `center` 2.5,1.5,0.5,0.5,1.5,2.5, `edges` the reverse. | v1. Done |
| FW-FX-005 | `period` | Milliseconds for one full cycle of the pattern on one tube, 100–600000, default 18000. `0` means static (no motion) where a pattern allows it. The only time parameter (OQ-23, decided); any `speed` is CLI sugar. | v1. Done |
| FW-FX-006 | `phase` | Offset between tubes as percent of `period` per unit of distance, 0–100, default 8 (≈ 30° of a rainbow). Tube `i`'s position in the cycle is `pos(i,t) = (t / period − d(i) × phase / 100) mod 1`. | v1. Done |
| FW-FX-007 | `easing` | How `pos` maps to the palette: `linear` (default), `sine`, `ease-in`, `ease-out`, `ease-in-out`, `step` (hard switches at stop boundaries, no fade). | v1. Done. A `step` palette switches to within 1/64 of a cycle (the lookup's resolution). |
| FW-FX-008 | `pulse` | Optional brightness envelope on any pattern: `{min 0–255, max 0–255, period ms, easing}`; the global brightness is scaled by the envelope. Default none. `min ≤ max`. | v1. Done. A missing field takes `min` 0, `max` 255, `period` 4000, `easing` sine; `period` 0 holds `max`, `null` removes the pulse. The envelope runs min → max → min over `period`. |
| FW-FX-009 | `density` | 1–100, sparkle/fire only: how many events per second per tube, scaled. Default 20. | v1.1 |
| FW-FX-010 | `width` | 1–6, chase only: how many adjacent tubes a runner covers. Default 1. `count` 1–6 runners, default 1. | v1.1 |
| FW-FX-011 | `duty` | 0–100, strobe only: percent of each period the first color is on. Default 50. | v1.1 |
| FW-FX-012 | `seed` | 0–65535, optional: fixes the pseudo-random sequence of sparkle/fire so the emulator tests are deterministic. Default: from `millis()` at apply. | v1.1 |
| FW-FX-013 | Unknown fields are rejected (`400`), not ignored, so typos in presets are caught. Values out of range are `400` with the field name in the message. | v1. Done |
| FW-FX-014 | Serialized size of an effect ≤ 1024 bytes; palette ≤ 8 stops. | v1. Done |
| FW-FX-015 | `drift` | `wave` only: milliseconds for one full sway of the wave's direction, 0 or 100–600000, default 0 (off). While it is on, `direction` is not read: the wave moves smoothly from `rtl` through `center` to `ltr` and back, at an uneven rate, with no jump in color. A clock's effect saved before this field existed loads with drift off. | Done (host-tested, `testEffectMath`; running on the clock since 2026-11-07, used by `aurora` and `lava`). |

## 2. Patterns

Each pattern says which fields it reads; the others are accepted and ignored.

| ID | Pattern | What it does | Reads | Status |
|---|---|---|---|---|
| FW-FX-020 | `solid` | Every tube shows `palette[0]`. With `period > 0` the color cycles through the palette over time (all tubes together). | palette, period, easing, pulse | v1. Done (host-tested; seen on tubes: checklist 14) |
| FW-FX-021 | `tubes` | Tube `i` shows `palette[i]` (a short palette repeats). The stock "custom" colors. `/api/tube` writes land here (FW-FX-061). | palette, pulse | v1. Done (host-tested; seen on tubes: checklist 14) |
| FW-FX-022 | `wave` | The palette travels across the tubes: tube `i` shows `palette(ease(pos(i,t)))`. `rainbow-wave` is this with a rainbow palette evened out in brightness (a `palette:"rainbow"` wave stays available). Adjacent tubes fade into each other continuously; nothing steps. | all of §1 except density/width/duty | v1. Done (host-tested; seen on tubes: checklist 14) |
| FW-FX-023 | `gradient` | A static spread of the palette across the six tubes in `direction` order (`phase` ignored, spread fixed at one palette length across the tubes); `period > 0` drifts it slowly. | palette, direction, period, easing, pulse | v1. Done (host-tested; seen on tubes: checklist 14). With `center` and `edges` the palette length runs from the middle (or the ends) over the three distances, mirrored. |
| FW-FX-024 | `breathe` | `solid` with a built-in pulse: brightness swings `pulse.min..max` (default 32..255) over `period` with `easing` (default `sine`); the color advances one palette stop per breath. | palette, period, easing, pulse | v1. Done (host-tested; seen on tubes: checklist 14). `pulse.min`/`max` are the swing; `pulse.period` is not read. |
| FW-FX-025 | `sparkle` | Base color `palette[0]`; random tubes flash `palette[1..]` for ~150 ms at `density` events/s. | palette, density, seed, pulse | v1.1 |
| FW-FX-026 | `chase` | `count` runners of `width` tubes in `palette[1..]` move over a `palette[0]` base in `direction`, one lap per `period`. | palette, direction, period, width, count, pulse | v1.1 |
| FW-FX-027 | `fire` | Each tube flickers independently between the palette stops with a random walk at `density` steps/s; `period` sets the slowest drift. Looks like flame or candle depending on the palette and density. | palette, period, density, seed, pulse | v1.1 |
| FW-FX-028 | `strobe` | Hard switch between `palette[0]` (on for `duty` % of `period`) and `palette[1]`; `phase` offsets tubes so `phase:50` with `direction:edges` makes the two halves alternate (police). | palette, period, phase, direction, duty | v1.1 |
| FW-FX-029 | New patterns are added by name in this table and in the core's pattern registry; they MUST be pure functions of `(effect, tube, t, seed)` so they are host-testable (VER-019). | | rule |

## 3. Display options

Separate from the effect and persisted on their own (FW-FX-050). They control the digits, not the colors.

| ID | Field | Rule | Status |
|---|---|---|---|
| FW-FX-030 | `hours` | `12` or `24` (the existing `/api/hours`, FW-API-014). | Done |
| FW-FX-031 | `leadingZero` | `true` (default) shows `09:05:00`; `false` blanks the hours-tens tube when it would be 0. | v1. Done |
| FW-FX-032 | `seconds` | `true` (default) shows seconds on tubes 5–6; `false` blanks them (the effect still colors them, so the wave stays continuous across all six). | v1. Done |
| FW-FX-033 | `transition` | How a tube changes digit: `cut` (default, instant) or `crossfade` (old digit LED fades out while the new fades in over `fadeMs`, 50–1000, default 200). `scramble` and `carry` (the stock effects) are **optional** values for v1.1 and are only ever used when the user chooses them; no pattern triggers a digit effect (FW-DSP-025). Crossfade lights two LEDs in one tube for at most `fadeMs` (OQ-24, decided: allowed, opt-in). | v1. Done (host-tested; seen on tubes: checklist 16); scramble/carry v1.1 |
| FW-FX-034 | `tubeBrightness` | Six values 0–100 %, default all 100, multiplied into each tube after the effect and the global brightness. Lets the user tame an uneven tube. | v1.1 |
| FW-FX-035 | `night` | Optional `{from:"22:00", to:"07:00", brightness:0-255}` in local time; between `from` and `to` the global brightness is capped at `brightness`. Attention flashes ignore the cap. | v1.1 (OQ-25, decided) |
| FW-FX-036 | Global `brightness` 0–255 (FW-API-013) stays the master level; effect `pulse`, `tubeBrightness` and `night` only ever scale it down. | v1. Done |

## 4. Presets

A preset is a named effect. Built-in presets are JSON in PROGMEM; user presets are JSON files on the device.

| ID | Requirement | Status |
|---|---|---|
| FW-FX-040 | Preset names: 1–24 chars of `A-Z a-z 0-9 - _ space`, compared case-insensitively. Built-in names are reserved. | v1. Done |
| FW-FX-041 | Built-in presets are read-only data in flash (one PROGMEM string each, ≤ 512 B). They are listed with `builtin:true`. | v1. Done |
| FW-FX-042 | User presets: up to **16**, each ≤ 1024 B, stored on LittleFS (OQ-21, decided) in one file, `/presets.tsv`, a line of `<name>\t<effect JSON>` per preset, rewritten through a temporary file. Saving over an existing name replaces it. `507` when full. A file per preset, as first planned, cannot hold 16: LittleFS gives each file over 64 B a 4 KB block and the partition has 16 blocks, and measured on that geometry 10 files fit beside `/effect.json` and `/display.json`. | v1. Done (API test: 16 full-size presets and the 17th, on the device's block size) |
| FW-FX-043 | Applying a preset copies its effect into the active effect and records `preset: <name>`. Any later change to the active effect clears `preset` to `null` (it is now a custom effect). | v1. Done. Deleting the applied preset, or saving another effect over its name, also clears `preset`; the effect stays. |
| FW-FX-044 | `GET /api/presets` lists `[{name, builtin, pattern}]`; the active one is marked `active:true`. | v1. Done |
| FW-FX-045 | Presets are exported and imported as plain JSON, so a user can keep them in the repo and the CLI can `preset save <name> --from file.json`. | v1. Done (`GET /api/preset`, `PUT /api/preset`; the CLI's `preset export` and `save --from`) |

### Built-in presets

| Name | Pattern | Palette | Notes | Needs |
|---|---|---|---|---|
| `rainbow-wave` | wave | `#ff2030 #d04c00 #807000 #00bc00 #00a8a8 #3070ff #a838ff #ff08e0` | rgb, ltr, period 18 s, phase 8. Even brightness, so blue doesn't read as a blink. **Default** (OQ-22, decided). | v1 |
| `nixie` | solid | `#ffa030` | The warm amber of a real nixie. | v1 |
| `classic-blue` | tubes | `241,100,100` ×6 | Today's firmware default. | v1 |
| `ocean` | wave | `#101a78@0 #3c10a8@20 #0a48d0@35 #00a8b8@45 #0a48d0@55 #2a18a0@70 #1040c0@80 #101a78@90` | rgb, ltr, period 5 s, phase 25, pulse 170–255 / 7 s. Uneven ripples and a swell. | v1 |
| `sunset` | solid | `#ffc000 #ff8000 #ff4000 #ff0000 #ff4000 #ff8000` | period 96 s: one color across, 16 s per stop. | v1 |
| `synthwave` | wave | `#ff08e0 #00a0b0 #a838ff` | rgb, rtl, period 10 s, phase 15. Even brightness. | v1 |
| `aurora` | wave | `#141458@0 #00c050@14 #00e0a0@22 #2a1490@30 #00ff66@42 #00b0b0@50 #1a2090@62 #301080@78` | rgb, period 18 s, phase 10, drift 90 s. Green and teal wisps over a dark sky. | v1 |
| `lava` | wave | `#500000@0 #c01000@22 #ff5000@32 #ffa000@40 #ff5000@48 #900800@60 #e02000@72 #600400@84` | rgb, period 20 s, phase 14, drift 75 s. A lava lamp's wandering blobs. | v1 |
| `candy` | wave | `#ff6fb5 #ffffff #4de3c1` | ltr, period 12 s. | v1 |
| `ice` | gradient | `#e0f7ff #8ad4ff #ffffff` | static (period 0). | v1 |
| `night` | solid | `#ff8c1a` | pulse max 40: a dim amber for bedrooms. | v1 |
| `vaporwave` | solid | `#c05400 #d64300 #f23500 #ff1888 #ff08e0 #a838ff #00a0b0 #a838ff` | rgb, period 16 s: one color across, even brightness. | v1 |
| `fire` | fire | `#4a0000 #ff2a00 #ff8c00 #ffd27f` | density 40. | v1.1 |
| `candle` | fire | `#3a1a00 #ff9a3c #ffd9a0` | density 12, period 4 s. | v1.1 |
| `police` | strobe | `#ff0000 #0000ff` | period 400 ms, duty 50, phase 50, edges. | v1.1 |
| `matrix` | chase | `#021a02 #00ff41 #0a8a2a` | rtl, period 1.2 s, count 2, width 1. | v1.1 |
| `cyberpunk` | chase | `#ff00a0 #fcee0a #00f0ff` | ltr, period 2 s, count 1, width 2. | v1.1 |
| `twinkle` | sparkle | `#101030 #ffffff #a0c8ff` | density 30. | v1.1 |

## 5. Persistence

| ID | Requirement | Status |
|---|---|---|
| FW-FX-050 | The active effect (`/effect.json`), display options (`/display.json`) and user presets (`/presets.tsv`, FW-FX-042) live on a **64 KB LittleFS** partition (board layout `1M (FS:64KB OTA:~470KB)`, OQ-21 decided). The EEPROM blob (FW-SET) keeps tz, hours, brightness and the six tube colors unchanged. | v1. Done on the emulator (API test across a restart). **Gap:** the clock now runs on the real partition, but nothing saved there has been checked across an update (checklist 7). |
| FW-FX-051 | Writes to `/effect.json` are **debounced 2 s** after the last change, so a client sliding a speed control does not wear the flash; presets and display options save at once. Attention flashes never write. | v1. Done (API test, from the `[fx] /effect.json saved` log line). A restart writes a pending change first. |
| FW-FX-052 | Missing or invalid files fall back to the default preset / default display options and log `[fx] <file> invalid, using defaults`. A failed LittleFS mount formats it once and logs that. | v1. Partial: a missing file and an empty file system are what every first boot exercises; invalid text is host-tested on the parsers. **Gap:** no end-to-end run with a corrupt file. |
| FW-FX-053 | Boot order: EEPROM → LittleFS → first frame. The first frame is drawn before Wi-Fi is attempted. | v1. Done (API test reads the order in the log) |

## 6. API

JSON **request bodies** are allowed on these routes only (an exception to the query-only rule in [http-api.md](http-api.md); OQ-26, decided), limited to 1024 bytes, `Content-Type: application/json`; anything else is `415`/`413`.

| ID | Method, path | Body / args | Success | Errors | Status |
|---|---|---|---|---|---|
| FW-FX-060 | `GET /api/effect` | — | `200` the active effect with every field filled in, plus `preset` | 401 | v1. Done |
| FW-FX-061 | `PUT /api/effect` | effect JSON (partial allowed: missing fields keep their current value; `pattern` switches reset pattern-specific fields to defaults) | `200` the resulting effect | 400, 401, 413 | v1. Done. In v1 the fields a pattern switch resets are `breathe`'s `easing` and `pulse`, going in or out of it. |
| FW-FX-062 | `POST /api/tube` (existing) | — | now also sets the active effect to `tubes` with the saved tube colors (preset `null`) | | v1. Done |
| FW-FX-063 | `GET /api/presets` | — | `200` list (FW-FX-044) | 401 | v1. Done |
| FW-FX-064 | `GET /api/preset?name=` | — | `200` that preset's effect | 401, 404 | v1. Done |
| FW-FX-065 | `POST /api/preset?name=` | — | `200` state; the preset is now active | 401, 404 | v1. Done |
| FW-FX-066 | `PUT /api/preset?name=` | effect JSON, or empty body = save the current active effect | `200` the saved effect | 400, 401, 403 (built-in name), 413, 507 (full) | v1. Done. A body needs `pattern` (FW-FX-001), unlike `PUT /api/effect`. A bad name is `400`. |
| FW-FX-067 | `DELETE /api/preset?name=` | — | `200 {"ok":true}` | 401, 403 (built-in), 404 | v1. Done |
| FW-FX-068 | `GET /api/display` / `POST /api/display` | query args `leadingZero=0\|1`, `seconds=0\|1`, `transition=cut\|crossfade`, `fadeMs=50-1000`, later `tubeBrightness=a,b,c,d,e,f`, `night=HH:MM,HH:MM,level` or `night=off` | `200` display options | 400, 401 | v1. Done. `tubeBrightness` and `night` are `400` until v1.1, rather than ignored. The answer also has `hours` (FW-FX-030), set through `/api/hours`. |
| FW-FX-069 | The state object (FW-API-030) gains `preset` (name or `null`), `effect: {pattern}` and `display: {hours, leadingZero, seconds, transition}`; the full effect is only on `/api/effect` to keep the state small (FW-BUD-005). | v1. Done |

## 7. Rendering and budgets

| ID | Requirement | Status |
|---|---|---|
| FW-FX-070 | At each 50 ms frame (FW-DSP-005) the engine computes six tube colors from `(effect, t)`, applies `pulse`, `tubeBrightness`, `night` and the global brightness, then the digits (FW-FX-030..033) pick which LED in each tube gets that color. Attention (FW-ATT) replaces the six colors when active; the engine still advances because everything is a function of `millis()`, so the effect resumes seamlessly (was FW-DSP-026). | v1. Done (`composeFrame`; host-tested, and through the `[leds]` log; on tubes: checklist 14). `EffectClock` extends `millis()` past its wrap. |
| FW-FX-071 | Integer/fixed-point math only in the per-frame path. The palette is expanded once at apply time into a 64-entry RGB lookup (192 B); easing uses a 64-entry table. No `float`, no heap allocation per frame. | v1. Done (no allocation host-tested). The only easing table is the sine one, 64 entries in flash; the other easings are integer formulas, which also serve `pulse`. So no easing RAM. |
| FW-FX-072 | CPU per frame ≤ **5 ms** at 80 MHz for any pattern (NeoPixel `show()` for 60 LEDs takes ~1.8 ms on top). Measured in the emulator with `clock_gettime` and on the device with `micros()`; logged once a minute as `[fx] frame max=<us>`. | v1. Partial: measured on the emulator, under 100 µs for each pattern (VER-021). The line is also logged when the effect changes. **Gap:** no figure from the clock yet (checklist 15). |
| FW-FX-073 | RAM: active effect ≤ 512 B, palette LUT 192 B, easing LUT 64 B, display options ≤ 64 B, sparkle/fire state ≤ 64 B; parsing uses one 2 KB ArduinoJson document that is freed after apply. Total steady-state < 1 KB. | v1. Done by the build's map: about 380 B of variables (effect, LUT, display options, digit fades, log state) plus 661 B of constant strings, which the budget's list leaves out. Free heap on the device: checklist 15. |
| FW-FX-074 | Flash: engine + patterns ≤ 24 KB of code, built-in presets ≤ 8 KB PROGMEM, LittleFS 64 KB. Image budget becomes 470 KB (FW-BUD-001). | v1. Done by the build's map: `effect.cpp` is 18.3 KB of flash, including the ArduinoJson template code it instantiates and about 1.2 KB of preset data; none of it in IRAM. LittleFS and its FS layer add about 26 KB. |
| FW-FX-075 | The emulator's `[leds]` log is rate-limited to one line per second while an effect is animating (was FW-DSP-027), plus one line on every digit change. | v1. Done. Every line goes out at most once a second while colors move, or at once when digits, `lit` or a flash change. |

## 8. MCP

Unchanged: attention and status only (OQ-06). `clock_status` returns the state object, which now carries `preset` and `effect.pattern`, so an agent can see what is showing. There is no `list_presets` tool and no tool that changes effects.
