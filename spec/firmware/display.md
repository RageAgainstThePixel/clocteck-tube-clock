# Display, LEDs and attention flashes (`FW-DSP`, `FW-ATT`)

## Display

| ID | Requirement | Status |
|---|---|---|
| FW-DSP-001 | Six tubes (confirmed physically, OQ-19), ten LEDs each (digits 0–9), 60 LEDs in one WS2812 chain. One digit per tube is shown at a time; all other LEDs in that tube are off, except during an opt-in `crossfade` digit transition (FW-FX-033; OQ-24, decided) when the outgoing and incoming digit LEDs overlap for ≤ 1000 ms. | Done |
| FW-DSP-002 | The mapping tube×digit → chain index is a `LedLayout` table. It MUST be a permutation of 0–59 (`isValid()`). Observed on the clock: the viewer's leftmost tube (index 0) sits over silkscreen LED51–60 and the rightmost over LED1–10, so tube `i` uses block `5 - i` ([board.md](board.md)). Digit `d` is the block's lowest silkscreen label plus `d`. The chain visits LED10→1, 20→11, 30→21, 40→31, then 51→60, 41→50, so `index[tube][digit]` is 40–49, 50–59, 39–30, 29–20, 19–10, 9–0 for tubes 0–5 (`LedLayout::ct001()`). | Done (host-tested, `testLayoutAndRender`). Chain order read from the pin-finder on 2026-11-06; digits seen on the two end tubes only, the middle four follow by pattern. **Gap:** this table has not been checked showing the right digits on the assembled clock (checklist 3). |
| FW-DSP-003 | Tubes show `HH MM SS` left to right as the user faces the clock; tube index 0 / API tube 1 is the leftmost (hours tens), matching the stock firmware's numbering (observed 2026-11-03). In 12-hour mode hour 0 shows as 12. Hours keep their leading zero (`01`..`12`) unless `leadingZero` is off (FW-FX-031). | Done |
| FW-DSP-004 | Each tube has its own hue and saturation (saved); value is fixed at 100 and the single saved brightness (0–255) scales every tube's RGB linearly (`c * (brightness+1) >> 8`). | Done |
| FW-DSP-005 | A frame is rendered every 50 ms from the `loop()`; rendering never blocks on network I/O. | Done |
| FW-DSP-006 | Without a known LED pin (`TUBECLOCK_LED_PIN` undefined) the device build drives no GPIO. The emulator and such builds log `[leds] lit=<0|1> brightness=<n> digits=<6 digits, - for blank> tubes=#rrggbb,...` as FW-FX-075 says. `brightness` is after `pulse`; `tubes` are the colors before brightness. `lit=0` means no LED is lit: the dark half of a blink, or blank tubes while the time is unknown. | Done |
| FW-DSP-007 | While the time is unknown (FW-TIME-004: no RTC time and no NTP yet) every tube is blank (`kBlank`, no LED lit). An attention flash still shows on blank tubes by lighting digit 0 of each tube in the flash color, so agents can reach the user before the clock has a time. | Done (core `tubeDigits`, host-tested; on the clock: manual checklist 10). The emulator always has the host's time, so it cannot show this state. |
| FW-DSP-008 | HSV→RGB is the standard sector conversion, rounding to nearest; `h=360` equals `h=0`. | Done |
| FW-DSP-009 | Brightness 0 turns every LED off but the clock keeps running. | Done |
| FW-DSP-010 | Colors and animations are the data-driven effects engine in [effects.md](effects.md) (`FW-FX`). The stock digit effects (carry, scramble) exist only as optional, user-chosen digit transitions there (FW-FX-033); no pattern ever triggers them. | Superseded by FW-FX |
| FW-DSP-011 | The tube colors the engine produces each frame are what FW-DSP-004's "saved colors" used to be: the saved per-tube colors are now the `tubes` pattern's palette (FW-FX-021). Brightness scaling and HSV→RGB (FW-DSP-004/008) are unchanged. | Done (`/api/tube` rewrites the `tubes` palette from the saved colors; `classic-blue` is the same colors as data). |
| FW-DSP-025 | **Digits never animate because of colors.** A tube's digit changes only when the time does (FW-DSP-003), using the user's chosen transition (FW-FX-033, default `cut`). No pattern, preset or color change causes a scramble, carry or flicker. | Done (`testComposeFrame`; the API test watches the `[leds]` digits through a wave and a flash). |

## Color modes

The one-off `wave` mode (FW-DSP-020..024, 026..028) was folded into the effects engine the same day it was specified: it is the `wave` pattern with the `rainbow` palette, preset `rainbow-wave` (FW-FX-022). Those IDs are **superseded** and not reused; FW-DSP-025 above is kept because it is a display rule, not an effect.

## Attention flashes

An attention flash is an overlay: the saved colors are never written, so there is nothing to restore when it ends. This is the main behavioral difference from the PC MCP server, which has to snapshot and restore.

| ID | Requirement | Status |
|---|---|---|
| FW-ATT-001 | `Attention::start(color, nowMs, durationMs, blink)` begins a flash; `durationMs` is clamped to `kMaxDurationMs` = 30000; `durationMs = 0` starts nothing. | Done |
| FW-ATT-002 | While active, every tube shows the flash color at the saved brightness, whatever the effect or its `pulse` (FW-FX-070). Digits keep showing the time, including those `leadingZero` or `seconds` would blank, since the flash has to light every tube. | Done |
| FW-ATT-003 | `blink` alternates lit/unlit with a 1000 ms period, 50 % duty, lit first. Unlit means the tube's digit LED is off, not dimmed. | Done |
| FW-ATT-004 | The flash ends exactly when `nowMs - startMs >= durationMs`, using unsigned wrap-safe arithmetic (correct across the 49-day `millis()` rollover). The loop also forgets an ended flash and beep, so neither replays when `millis()` comes round again. | Done (host-tested, `testAttention`; the rollover itself not run on hardware). |
| FW-ATT-005 | `cancel()` ends a flash at once; calling it with none active is a no-op. | Done |
| FW-ATT-006 | A new flash replaces an active one (new color, new timer). The replaced flash's color is never shown again. | Done |
| FW-ATT-007 | Flashes survive nothing: a reboot ends them. They are never persisted. | Done |
| FW-ATT-008 | The HTTP (`/api/attention`) and MCP (`get_attention`) entry points share the same `Attention` object; whichever came last wins. | Done |
| FW-ATT-009 | The state object reports `attention:true` only while a flash is active, so a client polling after `durationMs` sees `false` without any cancel. | Done |
| FW-ATT-010 | `beep` (off by default) sounds the buzzer once when the flash starts: two 80 ms pulses 80 ms apart, non-blocking, never repeated during the flash. Compiled out when `TUBECLOCK_BUZZER_PIN` is undefined (the argument is then accepted and ignored). | Partial. The pattern is core `BeepPattern`, host-tested, and the argument works on both entry points. The driver is FW-HW-007. The buzzer is GPIO 15, on when high, and a plain level sounds it (FW-HW-010). **Gap:** the attention beep itself has not been heard on the clock (checklist 11). |
| FW-ATT-011 | Any button press cancels an active flash (FW-HW-006). Pressing with no flash active does nothing. A button held since before the flash started is not a new press. | Partial (OQ-15): the rule is host-tested over `ButtonDebouncer` and `Attention` (`testButtons`), and the sketch does the same in `pollButtons`. The pins and pressed level are measured (FW-HW-010). **Gap:** a press cancelling a flash has not been tried on the clock (checklist 11). |
