# Stock firmware

This page is only for a clock still running the original firmware. If yours runs the custom firmware, none of this applies: go back to the [main README](../README.md).

> [!WARNING]
> The stock API has no authentication, and `/wificonf` returns your Wi-Fi password in plain text to anyone on the same network or within range of the open setup hotspot. The [custom firmware](../firmware/README.md) removes these routes and the hotspot.

## Contents

- [Reaching the clock](#reaching-the-clock)
- [CLI](#cli)
- [PC-based MCP server](#pc-based-mcp-server)
- [Library](#library)
- [HTTP API](#http-api)

## Reaching the clock

- **Setup hotspot:** the clock broadcasts an open network called `ClocTeck_xxxx` (the suffix is the end of its MAC address). Join it, and the clock is at `192.168.18.1`, which is the CLI's default host.
- **Home network:** run `clocteck wifi connect <ssid> <password>` (2.4 GHz only). After that, use `--host <ip>` or set `CLOCTECK_HOST` to the IP it reports.

Phones often drop the hotspot because it has no internet. If the setup page won't load, turn off mobile data.

## CLI

Install the CLI as in [Setup](../README.md#setup). You don't need the firmware values in `.env`. It detects the firmware by itself, and `--firmware stock` skips that check.

```text
clocteck [--host <ip>] [--firmware stock] [--json] <command>

  status                              Show settings and firmware
  color <tube 1-6|all> <color>        Set tube color: a name, #rrggbb, #rgb or h,s,v
  brightness <0-100>                  Set brightness for all tubes
  hours <12|24>
  timezone <-12..12>                  UTC offset in whole hours
  attention <color> [--seconds 1-30] [--blink]
                                      Flash the tubes, then go back to your colors
  firmware                            Show the firmware version
  detect                              Show which firmware answers
  sync                                Sync from the internet
  mode <custom|rainbow|breathing|flow>
  digits <normal|carry|scramble>      Set how digits change
  time [HH:MM[:SS]]                   Set the time (default: this computer's time)
  wifi scan
  wifi connect <ssid> <password>
  firmware --update                   Update online
```

Examples:

```sh
clocteck mode rainbow
clocteck timezone -5 && clocteck time
```

On firmware 2110, `time` and `sync` return OK but the display can stay at `00:00:00`. Unplugging the clock and plugging it back in makes it sync on boot, using the saved Wi-Fi and time zone.

Tube colors only show in `custom` mode. The clock has one brightness setting shared by every tube. `attention` keeps running for the flash and puts your colors back when it ends or on Ctrl+C.

## PC-based MCP server

The stock firmware can't run an MCP server, so `clocteck-mcp` runs one on a computer instead, and agents anywhere on your network can flash the clock through it. That computer has to stay on. After the flash (5 seconds by default), the clock goes back to whatever colors, brightness and mode it had before.

```sh
npm run mcp -- --clock <clock-ip> --token "<secret>"
# or: clocteck-mcp --clock <clock-ip> --token "<secret>"
```

It listens on `0.0.0.0:3939` and prints its LAN URLs. Options: `--port`, `--bind`, `--token`, `--allow-host`. Matching environment variables: `CLOCTECK_HOST`, `CLOCTECK_MCP_PORT`, `CLOCTECK_MCP_TOKEN`.

| Tool | Arguments |
|---|---|
| `get_attention` | `color` (a name such as red/orange/green/blue/purple/white, `#rrggbb` or `h,s,v`; default orange), `durationSeconds` (1–30, default 5), `blink`, `message` (written to the server log) |
| `clock_status` | Read-only: current settings, and whether a flash is showing |

If a second flash arrives during the first, it takes over the timer, and the clock still returns to your own colors, never to the earlier flash's color.

Adding it to Claude Code on another machine:

```sh
claude mcp add --transport http tube-clock http://<pc-ip>:3939/mcp --header "Authorization: Bearer <secret>"
```

Agents on other computers can only connect once Windows Firewall allows the port (run from an admin PowerShell, on a Private network):

```powershell
New-NetFirewallRule -DisplayName "ClocTeck MCP" -Direction Inbound -Protocol TCP -LocalPort 3939 -Action Allow -Profile Private
```

Without `--token`, anyone on your network can flash the clock. To block DNS rebinding, requests are only accepted when the `Host` header is an IP address, `localhost`, this computer's name, or a name passed with `--allow-host`.

## Library

```ts
import { ClocTeckClient } from 'clocteck-tube-clock';

const clock = new ClocTeckClient({ host: '192.168.18.1' });
console.log(await clock.getConfig());
await clock.setAllTubes({ h: 25, s: 100, v: 80 });
await clock.setMode('custom', 'carry');
```

## HTTP API

These are unauthenticated GET requests, reverse-engineered from the `/admin` page on firmware 2110. Every other path redirects (307) to `/admin`.

| Endpoint | Purpose |
|---|---|
| `/config` | JSON: `h1`–`h6` hue (0–360), `s1`–`s6` saturation (0–255), `light` brightness (0–255), `mode` (1–4), `outcarry` (0–2), `is24h`, `tz` (UTC offset), `alarmh`/`alarmm` |
| `/tubecolor?t=<1-6>&h=<0-360>&s=<0-100>&v=<0-100>` | Set a tube's color; `v` sets the brightness of all tubes |
| `/mode?m=<1-4>&s=<1-3>` | Mode: 1 custom, 2 rainbow, 3 breathing, 4 flow. Digit effect: 1 normal, 2 carry, 3 scramble |
| `/tm?s=<12\|24>` | Hour format |
| `/time?t=<1-25>` | Time zone, where `t` = UTC offset + 13 |
| `/uptm?t=0&h=&m=&s=` | Set the time manually, in **UTC** (the clock applies `tz`) |
| `/uptm?t=1` | Sync time from the internet |
| `/wifiscan`, then poll `/wifiscanover` | Scan. Returns `wait` until it finishes, then `[{ssid, rssi}]` |
| `/wificonn?s=<ssid>&p=<pwd>`, then poll `/wificonn?st=1` | Join Wi-Fi. Status `3` connected, `4` wrong password, `1` failed |
| `/wificonf` | `{ssid, pwd, ip}`. The saved password comes back in plain text |
| `/version`, `/nVersion` | Installed firmware `{ver}`, and the latest available online `{nv, ni}` |
| `/update`, then poll `/progress` | Online firmware update. `100` means done, `255` means failed. **Don't cut power while it runs.** |

The firmware's HTTP responses fail strict parsers (.NET's, for example), so the client uses Node's lenient parser.
