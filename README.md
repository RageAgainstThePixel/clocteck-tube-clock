# clocteck-tube-clock

Custom firmware with an on-device MCP server, CLI and TypeScript library for the **ClocTeck CT-001 RGB tube clock** (FCC ID 2BGKQ-CT001, made by Ningbo Qianjie Electronic Technology, board "Nixie Clock V4.5.1"). There's no app or cloud service involved: everything runs over your local network.

> [!WARNING]
> **Stock firmware:** the API has no authentication, and `/wificonf` returns your Wi-Fi password in plain text to anyone on the same network or within range of the open setup hotspot.
>
> **Custom firmware:**
>
> - `/wificonf`, `/admin`, `/update` and the setup hotspot are gone. Wi-Fi details are built in from `.env`, so there is no Wi-Fi page or route.
> - `/api/*`, `/mcp` and Wi-Fi updates need the API token (`FW_API_TOKEN`). Only `/health` is open.
> - Traffic is plain HTTP, so someone who can capture your Wi-Fi traffic could read the token and control the clock. Don't expose the clock to the internet.
> - The Wi-Fi password is still in the clock's flash, so anyone with the clock and a serial adapter can read it. The same goes for a stock backup image, so keep those private.

## Contents

- [What's here](#whats-here)
- [Setup](#setup)
- [Installing and updating the firmware](#installing-and-updating-the-firmware)
- [CLI](#cli)
- [MCP server: let agents get your attention](#mcp-server-let-agents-get-your-attention)
- [Library](#library)
- [Still on the stock firmware?](#still-on-the-stock-firmware)
- [License and disclaimer](#license-and-disclaimer)

**More detail:** [firmware, API and effects](firmware/README.md) · [spec](spec/README.md) · [stock firmware](docs/stock-firmware.md)

## What's here

| Part | Where | What it does |
|---|---|---|
| Custom firmware | [`firmware/`](firmware/README.md) | Replaces the stock firmware on the clock's ESP8285: token login, effects and presets, time sync, an MCP server on the clock, and updates over Wi-Fi |
| CLI and library | [`src/`](src/) | Control the clock from a computer |
| Spec | [`spec/`](spec/README.md) | The source of truth for how everything should behave |

The clock's USB-C port only supplies power. All control goes over Wi-Fi, and the first install of the custom firmware goes over the board's JP1 serial header.

## Setup

Requires Node 20 or newer.

```sh
npm install
npm run build
npm link    # optional: puts `clocteck` on your PATH
```

Create a `.env` file in the repo root. It's git-ignored.

```sh
FW_WIFI_SSID=your-2.4-ghz-network
FW_WIFI_PASSWORD=your-wifi-password
FW_API_TOKEN=a-long-random-string
CLOCTECK_HOST=tubeclock.local
```

## Installing and updating the firmware

The first install means opening the case and using a serial adapter on JP1. Back up the stock firmware first, so you have a copy to go back to. The steps, the JP1 pinout and the build details are in [`firmware/README.md`](firmware/README.md).

After that, updates go over Wi-Fi:

```sh
npm run fw:build
npm run fw:ota
```

The tubes scramble in random colors during the upload and lock in green from the left, then the clock restarts. `npm run fw:ota -- --check` checks the image and sends nothing.

The Wi-Fi name and password are built into the firmware, and there's no setup hotspot. To change them, update `.env`, then build and upload **before** you change the network. If the clock has already lost Wi-Fi, recovery is over JP1, which means opening the case.

## CLI

```text
clocteck [--host <ip>] [--token <t>] [--json] <command>

  status                              Show settings and firmware
  color <tube 1-6|all> <color>        Set tube color: a name, #rrggbb, #rgb or h,s,v
  brightness <0-100>                  Set brightness for all tubes
  hours <12|24>
  timezone <-12..12>                  UTC offset in whole hours
  tz <posix rule>                     Time zone with DST, e.g. EST5EDT,M3.2.0,M11.1.0
  sync                                Show whether NTP has synced
  attention <color> [--seconds 1-30] [--blink] [--beep]
                                      Flash the tubes, then go back to your colors
  attention cancel                    End a flash early
  firmware                            Show the firmware version
  detect                              Show which firmware answers
  restart                             Reboot the clock
  preset list | use <name> | show <name> | export <name> | delete <name>
  preset save <name> [--from file.json]
                                      Named effects; save defaults to the active effect
  effect                              Show the active effect
  effect set <pattern> [--palette c1,c2,...|rainbow] [--direction ltr|rtl|center|edges]
             [--period ms | --speed 1-100] [--phase 0-100] [--drift ms] [--easing e] [--pulse min,max[,ms]]
             [--density 1-100] [--seed n] [--width 1-6] [--count 1-6] [--duty 0-100] [--glyphs on|off]
             [--fade 50-3000] [--splash 0-100] [--splash-fade 50-3000]
             [--accent style[,ms][,tubes=all|hm|110110][,color=c][,spread=ms][,strength=0-100] | off]
                                      Change it; what you leave out stays. Patterns: solid, tubes,
                                      wave, gradient, breathe, sparkle, chase, fire, strobe, matrix.
                                      An accent is what a tube does when its digit changes:
                                      flicker, flash, dip, glow or shift
  effect json <file|->                Change it from raw JSON
  display [--hours 12|24] [--leading-zero on|off] [--seconds on|off]
          [--transition cut|crossfade|scramble|carry] [--fade ms] [--tube-brightness a,b,c,d,e,f]
                                      What the digits do; no flags prints the options.
                                      --tube-brightness dims each tube, 0-100, left to right
```

The CLI needs the API token: `--token`, else `$CLOCTECK_TOKEN`, else `$FW_API_TOKEN`. It doesn't read `.env` by itself; `node --env-file=.env dist/cli.js ...` uses the repo's. With `--firmware tubeclock` and no host given, the host is `tubeclock.local`.

`--json` prints the clock's state object, `brightness` is sent as 0–255 (`50` becomes `128`), `timezone -5` sets the fixed rule `<-05>5` (use `tz` for daylight saving), and `attention` returns at once because the clock ends the flash itself.

The CLI checks which firmware answers once per run, and `--firmware stock|tubeclock` skips that check. A command the clock's firmware doesn't have fails with exit code 2. Other errors exit with 1.

Examples:

```sh
clocteck --host tubeclock.local status
clocteck --host tubeclock.local preset use ocean
clocteck --host tubeclock.local color all "#ff6a00"
clocteck --host tubeclock.local effect set wave --palette "#ff00ff,#00e5ff" --direction rtl --speed 60
clocteck --host tubeclock.local effect set chase --palette "#000010,#00e5ff" --count 2 --period 3000
clocteck --host tubeclock.local effect set wave --accent dip,300,spread=80
clocteck --host tubeclock.local display --transition scramble --fade 400
clocteck --host tubeclock.local display --tube-brightness 100,100,80,100,100,100
clocteck --host tubeclock.local tz EST5EDT,M3.2.0,M11.1.0
clocteck --host tubeclock.local attention orange --seconds 10 --blink
```

## MCP server: let agents get your attention

The custom firmware runs an MCP server on the clock itself, over Streamable HTTP at `/mcp`, with no computer in between. It has been tested on the emulator, but not yet on a real clock.

```sh
claude mcp add --transport http tube-clock http://tubeclock.local/mcp --header "Authorization: Bearer <FW_API_TOKEN>"
```

| Tool | Arguments |
|---|---|
| `get_attention` | `color` (a name such as red/orange/green/blue/purple/white, `#rrggbb`, `#rgb` or `h,s,v`; default orange), `durationSeconds` (1–30, default 5), `blink`, `beep`, `message` (written to the serial log) |
| `cancel_attention` | None. Ends a flash early |
| `clock_status` | None. Read-only: the clock's current state |

A second flash during the first replaces it. When a flash ends, the clock goes back to its own effect.

## Library

```ts
import { TubeClockClient, detectFirmware } from 'clocteck-tube-clock';

console.log(await detectFirmware('tubeclock.local')); // 'tubeclock' | 'stock' | 'unknown'
const clock = new TubeClockClient({ host: 'tubeclock.local' }); // token from CLOCTECK_TOKEN, else FW_API_TOKEN
await clock.setTube('all', 25, 100);
await clock.attention({ h: 0, s: 100 }, { ms: 3000, blink: true });
```

## Still on the stock firmware?

The CLI and library also work with the original firmware, and a separate MCP server runs on a computer for clocks that can't host their own. Stock-only commands, the PC-based MCP server and the stock HTTP API are on the [stock firmware page](docs/stock-firmware.md).

## License and disclaimer

This project's code is under the [MIT License](LICENSE). The vendored [ArduinoJson](firmware/third_party/ArduinoJson/LICENSE.txt) keeps its own MIT license.

This is an independent, unofficial project for people who own this clock. It is not affiliated with, endorsed by or supported by ClocTeck, Ningbo Qianjie Electronic Technology or any other manufacturer or seller. All product names, trademarks and logos belong to their owners and are used here only to identify the hardware this project works with.

The repository contains no manufacturer firmware or code. Details of the stock firmware's behavior come from using and examining a clock I own, and are published to help owners protect their networks.

Replacing or modifying a device's firmware may void its warranty, may break the device, and may affect its compliance with local regulations. You do this at your own risk, and only on hardware you own. The software comes with no warranty, as the license says.
