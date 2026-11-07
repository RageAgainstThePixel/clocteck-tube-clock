# Security policy

## Reporting a vulnerability

Please report security problems privately through [GitHub's vulnerability reporting](https://github.com/RageAgainstThePixel/clocteck-tube-clock/security/advisories/new), not in a public issue.

Include what you found, how to reproduce it, and which version you tested. Run `clocteck firmware` to see the firmware version.

This is a hobby project, so fixes are best-effort and there's no promised response time.

## What's covered

Only the latest version is supported.

- The custom firmware
- The on-device MCP server
- The CLI and TypeScript library
- The PC-based MCP server

## What isn't covered

Bugs in the stock firmware itself aren't ours to fix, because we don't have its source code. Please report those to the manufacturer or to [CERT/CC](https://www.kb.cert.org/vuls/report/). The known stock firmware problems are listed on the [stock firmware page](docs/stock-firmware.md).

## Known limits of the custom firmware

These come from the design, so they don't need a report:

- **Plain HTTP:** traffic isn't encrypted, so someone who can capture your Wi-Fi traffic could read the API token and control the clock.
- **Wi-Fi password in flash:** anyone with the clock and a serial adapter can read the Wi-Fi password and API token.
- **Local network only:** the clock isn't built to face the internet, so don't expose it.

Stock firmware backups contain your Wi-Fi password. Keep them private, and never attach one to a report.
