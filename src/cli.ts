#!/usr/bin/env node
import { readFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { AttentionController } from './attention.js';
import { ClocTeckClient, COLOR_MODES, DEFAULT_HOST, DIGIT_EFFECTS, type ColorMode, type DigitEffect } from './client.js';
import { hsvToHex, parseColor, type Hsv } from './color.js';
import {
  COMMANDS,
  commandLabel,
  displayChange,
  effectBody,
  percentToBrightness,
  requiredFirmware,
  timeSynced,
  unsupportedHint,
  unsupportedMessage,
  utcOffsetToPosix,
  type Command,
} from './commands.js';
import { detectFirmware } from './detect.js';
import { TUBECLOCK_DEFAULT_HOST, TubeClockClient, type DisplayOptions, type Effect, type TubeClockState } from './tubeclock.js';

const USAGE = `Usage: clocteck [--host <ip>] [--token <t>] [--firmware stock|tubeclock] [--json] <command>

Works with the stock firmware and the new tubeclock firmware; detects which one answers.

Commands:
  status                              Show settings, firmware and Wi-Fi state
  color <tube 1-6|all> <color>        Set tube color: a name, #rrggbb, #rgb or h,s,v
  brightness <0-100>                  Set brightness for all tubes
  hours <12|24>                       Set 12- or 24-hour display
  timezone <-12..12>                  Set a fixed UTC offset in hours
  attention <color> [--seconds 1-30] [--blink] [--beep]
                                      Flash the tubes, then go back to your colors
  firmware                            Show the firmware version
  detect                              Show which firmware answers
  sync                                Stock: sync from the internet. New: show whether NTP has synced

Stock firmware only:
  mode <${COLOR_MODES.join('|')}>      Set the color mode
  digits <${DIGIT_EFFECTS.join('|')}>          Set how digits change
  time [HH:MM[:SS]]                   Set the time (default: this computer's time)
  wifi scan                           List networks the clock can see
  wifi connect <ssid> <password>      Join the clock to a 2.4 GHz network
  firmware --update                   Update online

New firmware only:
  tz <posix rule>                     Set the time zone, e.g. EST5EDT,M3.2.0,M11.1.0
  attention cancel                    End a flash early
  restart                             Reboot the clock
  preset list                         List the built-in and saved presets
  preset use <name>                   Show a preset
  preset show <name>                  Print a preset as JSON (preset export <name> > file.json too)
  preset save <name> [--from file.json]
                                      Save a preset (default: the active effect)
  preset delete <name>                Delete a saved preset
  effect                              Show the active effect
  effect set <pattern> [--palette c1,c2,...|rainbow] [--direction ltr|rtl|center|edges]
             [--period ms | --speed 1-100] [--phase 0-100] [--drift ms] [--easing e] [--pulse min,max[,ms]]
                                      Change the active effect; what you leave out stays
  effect json <file|->                Change it from raw JSON
  display [--hours 12|24] [--leading-zero on|off] [--seconds on|off]
          [--transition cut|crossfade] [--fade ms]
                                      Change what the digits do; no flags prints the options

Options:
  --host <ip>       Clock address (default: $CLOCTECK_HOST, else ${DEFAULT_HOST},
                    or ${TUBECLOCK_DEFAULT_HOST} with --firmware tubeclock)
  --token <t>       New firmware API token (default: $CLOCTECK_TOKEN, else $FW_API_TOKEN)
  --firmware <f>    stock or tubeclock: skip detection
  --timeout <ms>    Request timeout (default: 10000)
  --json            Print machine-readable output
  -h, --help        Show this help`;

// parseArgs treats "-5" as an option, so shield negative numbers (e.g. `timezone -5`) and restore them after
const NEGATIVE = '\u0000';
const OPTIONS = {
  args: process.argv.slice(2).map((a) => (/^-\d+$/.test(a) ? NEGATIVE + a : a)),
  allowPositionals: true,
  options: {
    host: { type: 'string' },
    token: { type: 'string' },
    firmware: { type: 'string' },
    timeout: { type: 'string' },
    json: { type: 'boolean', default: false },
    update: { type: 'boolean', default: false },
    seconds: { type: 'string' },
    blink: { type: 'boolean', default: false },
    beep: { type: 'boolean', default: false },
    from: { type: 'string' },
    palette: { type: 'string' },
    direction: { type: 'string' },
    period: { type: 'string' },
    speed: { type: 'string' },
    phase: { type: 'string' },
    drift: { type: 'string' },
    easing: { type: 'string' },
    pulse: { type: 'string' },
    density: { type: 'string' },
    width: { type: 'string' },
    count: { type: 'string' },
    duty: { type: 'string' },
    hours: { type: 'string' },
    'leading-zero': { type: 'string' },
    transition: { type: 'string' },
    fade: { type: 'string' },
    'tube-brightness': { type: 'string' },
    night: { type: 'string' },
    help: { type: 'boolean', short: 'h', default: false },
  },
} as const;
const { values, positionals: rawPositionals } = (() => {
  try {
    return parseArgs(OPTIONS);
  } catch (err) {
    // HOST-CLI-005: a bad flag is a plain message, not a stack trace.
    fail(err instanceof Error ? err.message : String(err));
  }
})();
const positionals = rawPositionals.map((p) => (p.startsWith(NEGATIVE) ? p.slice(1) : p));

function fail(message: string, code = 1): never {
  console.error(`clocteck: ${message}`);
  process.exit(code);
}

function oneOf<T extends string>(value: string | undefined, options: readonly T[], what: string): T {
  if (!value || !options.includes(value as T)) fail(`${what} must be one of: ${options.join(', ')}`);
  return value as T;
}

function print(json: unknown, text: string) {
  console.log(values.json ? JSON.stringify(json, null, 2) : text);
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function parseTime(input: string): Date {
  const m = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(input);
  if (!m) fail(`Invalid time "${input}": expected HH:MM or HH:MM:SS`);
  const [h, min, s] = [Number(m[1]), Number(m[2]), Number(m[3] ?? 0)];
  if (h > 23 || min > 59 || s > 59) fail(`Invalid time "${input}"`);
  const date = new Date();
  date.setHours(h, min, s, 0);
  return date;
}

/** Argument checks shared by both firmwares, so a typo fails the same way on either. */
function colorArgs(args: string[]): { target: number | 'all'; color: Hsv } {
  const [target, colorArg] = args;
  if (!target || !colorArg) fail('usage: color <tube 1-6|all> <color>');
  const tube = target === 'all' ? 'all' : Number(target);
  if (tube !== 'all' && (!Number.isInteger(tube) || tube < 1 || tube > 6)) fail('Tube must be 1-6 or "all"');
  return { target: tube, color: parseColor(colorArg) };
}

function brightnessArg(args: string[]): number {
  const level = Number(args[0]);
  if (!args[0] || !Number.isFinite(level) || level < 0 || level > 100) fail('usage: brightness <0-100>');
  return level;
}

function hoursArg(args: string[]): 12 | 24 {
  const format = Number(args[0]);
  if (format !== 12 && format !== 24) fail('usage: hours <12|24>');
  return format;
}

function offsetArg(args: string[]): number {
  const offset = Number(args[0]);
  if (args[0] === undefined || !Number.isInteger(offset) || offset < -12 || offset > 12) fail('usage: timezone <-12..12>');
  return offset;
}

function attentionArgs(args: string[]): { color: Hsv; seconds: number } {
  if (!args[0]) fail('usage: attention <color> [--seconds 1-30] [--blink] [--beep]');
  const seconds = values.seconds === undefined ? 5 : Number(values.seconds);
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 30) fail('--seconds must be a whole number from 1 to 30');
  return { color: parseColor(args[0]), seconds };
}

async function runStock(clock: ClocTeckClient, command: Command, args: string[]) {
  switch (command) {
    case 'status': {
      const config = await clock.getConfig();
      const version = await clock.getVersion();
      const wifi = await clock.getWifiConfig();
      const { password: _, ...wifiPublic } = wifi;
      const sign = config.utcOffset >= 0 ? '+' : '';
      const pad = (n: number) => String(n).padStart(2, '0');
      print(
        { host: clock.host, firmware: version, wifi: wifiPublic, ...config },
        [
          `Clock      ${clock.host} (firmware ${version})`,
          `Wi-Fi      ${wifi.ssid ? `${wifi.ssid} ${wifi.ip ? `connected at ${wifi.ip}` : 'not connected'}` : 'not configured'}`,
          `Mode       ${config.mode}, digits ${config.digitEffect}`,
          `Display    ${config.is24Hour ? '24' : '12'}-hour, UTC${sign}${config.utcOffset}`,
          `Brightness ${config.brightness}%`,
          `Alarm      ${pad(config.alarm.hour)}:${pad(config.alarm.minute)}`,
          'Tubes',
          ...config.tubes.map(
            (t) => `  ${t.tube}  ${hsvToHex({ h: t.hue, s: t.saturation, v: 100 })}  (h ${t.hue}, s ${t.saturation})`,
          ),
        ].join('\n'),
      );
      break;
    }
    case 'color': {
      const { target, color } = colorArgs(args);
      if (target === 'all') await clock.setAllTubes(color);
      else await clock.setTubeColor(target, color);
      print({ tube: target, color }, `Tube ${target} set to ${hsvToHex(color)}`);
      break;
    }
    case 'brightness': {
      const level = brightnessArg(args);
      await clock.setBrightness(level);
      print({ brightness: level }, `Brightness set to ${level}%`);
      break;
    }
    case 'mode': {
      const mode = oneOf<ColorMode>(args[0], COLOR_MODES, 'Mode');
      await clock.setMode(mode);
      print({ mode }, `Mode set to ${mode}`);
      break;
    }
    case 'digits': {
      const effect = oneOf<DigitEffect>(args[0], DIGIT_EFFECTS, 'Digit effect');
      await clock.setMode(undefined, effect);
      print({ digitEffect: effect }, `Digit effect set to ${effect}`);
      break;
    }
    case 'hours': {
      const format = hoursArg(args);
      await clock.setHourFormat(format);
      print({ hours: format }, `Display set to ${format}-hour`);
      break;
    }
    case 'timezone': {
      const offset = offsetArg(args);
      await clock.setUtcOffset(offset);
      print({ utcOffset: offset }, `Time zone set to UTC${offset >= 0 ? '+' : ''}${offset}`);
      break;
    }
    case 'time': {
      const date = args[0] ? parseTime(args[0]) : new Date();
      await clock.setTime(date);
      print({ time: date.toISOString() }, `Clock set to ${date.toLocaleTimeString()} (shown in the clock's own time zone)`);
      break;
    }
    case 'sync': {
      await clock.syncTime();
      print({ synced: true }, 'Asked the clock to sync its time from the internet');
      break;
    }
    case 'wifi': {
      const [sub, ssid, password] = args;
      if (sub === 'scan') {
        const networks = await clock.scanWifi();
        print(networks, networks.map((n) => `${String(n.rssi).padStart(4)} dBm  ${n.ssid}`).join('\n') || 'No networks found');
      } else if (sub === 'connect') {
        if (!ssid || password === undefined) fail('usage: wifi connect <ssid> <password>');
        const ip = await clock.connectWifi(ssid, password);
        print({ ssid, ip }, `Connected to ${ssid}; the clock is now at ${ip}`);
      } else {
        fail('usage: wifi <scan|connect>');
      }
      break;
    }
    case 'firmware': {
      const current = await clock.getVersion();
      if (!values.update) {
        let latest: { version: string; notes: string } | undefined;
        try {
          latest = await clock.getLatestVersion();
        } catch {
          // Only answers when the clock is online
        }
        print(
          { current, latest },
          `Installed ${current}` + (latest?.version ? `\nLatest    ${latest.version}${latest.notes ? ` (${latest.notes})` : ''}` : '\nLatest    unknown (clock is not online)'),
        );
        break;
      }
      console.error('Updating firmware. Do not unplug the clock until this finishes.');
      await clock.updateFirmware((p) => process.stderr.write(`\r${p}%   `));
      process.stderr.write('\n');
      print({ updated: true }, `Firmware update finished (was ${current})`);
      break;
    }
    case 'attention': {
      // HOST-CLI-022: the PC holds the snapshot, so this process stays up until it has restored the colors
      const { color, seconds } = attentionArgs(args);
      const controller = new AttentionController(clock);
      const stop = () => void controller.restore().finally(() => process.exit(130));
      process.once('SIGINT', stop);
      process.once('SIGTERM', stop);
      const { restoreAt } = await controller.flash({ color, durationMs: seconds * 1000, blink: values.blink });
      print(
        { firmware: 'stock', color, seconds, blink: values.blink, restoreAt: restoreAt.toISOString() },
        `Flashing ${hsvToHex(color)} for ${seconds} s; Ctrl+C restores your colors early`,
      );
      await sleep(seconds * 1000);
      await controller.restore();
      if (controller.active) fail('could not restore the clock\'s colors; run the same attention command again to retry');
      break;
    }
    case 'detect': {
      const version = await clock.getVersion();
      print({ firmware: 'stock', version }, `stock ${version}`);
      break;
    }
    default:
      // Unreachable: requiredFirmware() routes tubeclock-only commands away first
      fail(unsupportedMessage(command, 'tubeclock'), 2);
  }
}

/** Text lines for the new firmware's state. `--json` prints the state itself instead. */
function describeState(host: string, state: TubeClockState): string {
  const pct = Math.round(state.brightness / 2.55);
  const lines = [
    `Clock      ${host} (tubeclock ${state.version})`,
    `Time       ${state.time === 'unset' ? 'not known yet' : state.time}  (${state.tz})`,
    `Display    ${state.is24h ? '24' : '12'}-hour`,
    `Brightness ${state.brightness}/255 (${pct}%)`,
    `Attention  ${state.attention ? 'flashing' : 'off'}`,
  ];
  if (state.effect) lines.push(`Effect     ${state.effect.pattern}${state.preset ? ` (preset ${state.preset})` : ''}`);
  if (state.timeSource) lines.push(`Source     ${state.timeSource}${state.lastSync ? `, last NTP sync ${state.lastSync}` : ''}`);
  lines.push('Tubes', ...state.tubes.map((t, i) => `  ${i + 1}  ${hsvToHex({ h: t.h, s: t.s, v: 100 })}  (h ${t.h}, s ${t.s})`));
  return lines.join('\n');
}

/** An effect from a file or, for `-`, standard input (HOST-CLI-026, HOST-CLI-027). */
function readEffect(path: string): Effect {
  let text: string;
  try {
    text = readFileSync(path === '-' ? 0 : path, 'utf8');
  } catch (err) {
    fail(`could not read ${path === '-' ? 'standard input' : path}: ${err instanceof Error ? err.message : String(err)}`);
  }
  try {
    return JSON.parse(text) as Effect;
  } catch {
    fail(`${path === '-' ? 'standard input' : path} is not JSON`);
  }
}

function describeEffect(effect: Effect): string {
  const palette = Array.isArray(effect.palette) ? effect.palette.map((c) => (typeof c === 'string' ? c : `${c.color}@${c.at}`)).join(' ') : effect.palette;
  const pulse = effect.pulse ? `\nPulse      ${effect.pulse.min}..${effect.pulse.max} over ${effect.pulse.period} ms, ${effect.pulse.easing}` : '';
  return (
    `Pattern    ${effect.pattern}${effect.preset ? ` (preset ${effect.preset})` : ''}\n` +
    `Palette    ${palette} (${effect.interpolate})\n` +
    `Motion     ${effect.drift ? `drift ${effect.drift} ms` : effect.direction}, period ${effect.period} ms, phase ${effect.phase}%, ${effect.easing}` +
    pulse
  );
}

function describeDisplay(d: DisplayOptions): string {
  return [
    `Hours        ${d.hours}`,
    `Leading zero ${d.leadingZero ? 'on' : 'off'}`,
    `Seconds      ${d.seconds ? 'on' : 'off'}`,
    `Transition   ${d.transition}${d.transition === 'crossfade' ? `, ${d.fadeMs} ms` : ''}`,
  ].join('\n');
}

async function runPreset(clock: TubeClockClient, args: string[]) {
  const [sub, name] = args;
  if (sub === 'list') {
    const presets = await clock.listPresets();
    print(presets, presets.map((p) => `${p.active ? '*' : ' '} ${p.name.padEnd(24)} ${p.pattern.padEnd(9)} ${p.builtin ? 'built-in' : 'saved'}`).join('\n'));
    return;
  }
  if (!name || !['use', 'show', 'export', 'save', 'delete'].includes(sub ?? '')) fail('usage: preset <list|use|show|export|save|delete> [name]');
  switch (sub) {
    case 'use': {
      const state = await clock.applyPreset(name);
      print(state, `Showing ${state.preset ?? name}`);
      break;
    }
    case 'show':
    case 'export':
      // Plain JSON in every case, so `preset export x > file.json` round-trips (FW-FX-045).
      console.log(JSON.stringify(await clock.getPreset(name), null, 2));
      break;
    case 'save': {
      const saved = await clock.savePreset(name, values.from === undefined ? undefined : readEffect(values.from));
      print(saved, `Saved preset ${name}`);
      break;
    }
    default: {
      const result = await clock.deletePreset(name);
      print(result, `Deleted preset ${name}`);
    }
  }
}

async function runEffect(clock: TubeClockClient, args: string[]) {
  const [sub, target] = args;
  if (sub === undefined) {
    const effect = await clock.getEffect();
    print(effect, describeEffect(effect));
    return;
  }
  if ((sub !== 'set' && sub !== 'json') || !target) fail('usage: effect [set <pattern> [flags] | json <file|->]');
  const effect = await clock.setEffect(sub === 'json' ? readEffect(target) : effectBody(target, values));
  print(effect, describeEffect(effect));
}

async function runDisplay(clock: TubeClockClient) {
  const change = displayChange({
    hours: values.hours,
    leadingZero: values['leading-zero'],
    seconds: values.seconds,
    transition: values.transition,
    fade: values.fade,
    tubeBrightness: values['tube-brightness'],
    night: values.night,
  });
  const options = Object.keys(change).length ? await clock.setDisplay(change) : await clock.getDisplay();
  print(options, describeDisplay(options));
}

async function runTubeClock(clock: TubeClockClient, command: Command, args: string[]) {
  switch (command) {
    case 'status': {
      const state = await clock.getState();
      print(state, describeState(clock.host, state));
      break;
    }
    case 'color': {
      // HOST-CLI-011: one request, `all` is native; the color's value is ignored (brightness is global)
      const { target, color } = colorArgs(args);
      const state = await clock.setTube(target, color.h, color.s);
      print(state, `Tube ${target} set to ${hsvToHex({ ...color, v: 100 })}`);
      break;
    }
    case 'brightness': {
      const level = brightnessArg(args);
      const state = await clock.setBrightness(percentToBrightness(level));
      print(state, `Brightness set to ${level}% (${state.brightness}/255)`);
      break;
    }
    case 'hours': {
      const state = await clock.setHourFormat(hoursArg(args));
      print(state, `Display set to ${state.is24h ? 24 : 12}-hour`);
      break;
    }
    case 'timezone': {
      // HOST-CLI-016: a fixed offset has no daylight saving, so point at `tz`
      const offset = offsetArg(args);
      const state = await clock.setTimeZone(utcOffsetToPosix(offset));
      print(
        state,
        `Time zone set to UTC${offset >= 0 ? '+' : ''}${offset} (${state.tz}), with no daylight saving.\n` +
          'For DST use a full rule, e.g. clocteck tz EST5EDT,M3.2.0,M11.1.0',
      );
      break;
    }
    case 'tz': {
      if (!args[0]) fail('usage: tz <posix rule>, e.g. EST5EDT,M3.2.0,M11.1.0');
      const state = await clock.setTimeZone(args[0]);
      print(state, `Time zone set to ${state.tz}`);
      break;
    }
    case 'sync': {
      // HOST-CLI-019: NTP is automatic; report whether it has synced
      const state = await clock.getState();
      const synced = timeSynced(state);
      print(
        state,
        `The new firmware syncs its time over NTP by itself. ` +
          (synced ? `Time is synced${state.lastSync ? ` (last sync ${state.lastSync})` : ''}: ${state.time}` : 'It has not synced yet.'),
      );
      break;
    }
    case 'firmware': {
      const state = await clock.getState();
      print(state, `Installed tubeclock ${state.version}`);
      break;
    }
    case 'attention': {
      // HOST-CLI-022/023: the clock runs and ends the flash itself, so this returns at once
      if (args[0] === 'cancel') {
        const state = await clock.cancelAttention();
        print(state, 'Attention flash cancelled');
        break;
      }
      const { color, seconds } = attentionArgs(args);
      const state = await clock.attention(color, { ms: seconds * 1000, blink: values.blink, beep: values.beep });
      print(state, `Flashing ${hsvToHex({ ...color, v: 100 })} for ${seconds} s`);
      break;
    }
    case 'detect': {
      const state = await clock.getState();
      print({ firmware: 'tubeclock', version: state.version }, `tubeclock ${state.version}`);
      break;
    }
    case 'restart': {
      const result = await clock.restart();
      print(result, 'Restarting the clock; it is back in about 15 s');
      break;
    }
    case 'preset':
      await runPreset(clock, args);
      break;
    case 'effect':
      await runEffect(clock, args);
      break;
    case 'display':
      await runDisplay(clock);
      break;
    default:
      // Unreachable: requiredFirmware() routes stock-only commands away first
      fail(unsupportedMessage(command, 'stock'), 2);
  }
}

async function main() {
  const [rawCommand, ...args] = positionals;
  if (values.help || !rawCommand) {
    console.log(USAGE);
    return;
  }
  if (!COMMANDS.includes(rawCommand as Command)) fail(`unknown command "${rawCommand}"\n\n${USAGE}`);
  const command = rawCommand as Command;

  const forced = values.firmware;
  if (forced !== undefined && forced !== 'stock' && forced !== 'tubeclock') fail('--firmware must be stock or tubeclock');
  const timeoutMs = values.timeout ? Number(values.timeout) : undefined;
  const host = values.host ?? process.env.CLOCTECK_HOST ?? (forced === 'tubeclock' ? TUBECLOCK_DEFAULT_HOST : DEFAULT_HOST);

  // HOST-CLI-003: detect once per invocation unless --firmware says which
  const firmware = forced ?? (await detectFirmware(host, timeoutMs ?? 5000));
  if (firmware === 'unknown') fail(`could not tell which firmware answers at ${host}; is the clock reachable? (--firmware skips detection)`);
  if (command === 'detect' && firmware === 'tubeclock' && !values.token && !process.env.CLOCTECK_TOKEN && !process.env.FW_API_TOKEN) {
    // Detection itself needs no token; only the version does
    print({ firmware }, 'tubeclock (set CLOCTECK_TOKEN to see its version)');
    return;
  }

  // HOST-CLI-007: refuse commands the firmware lacks, with exit code 2
  const needs = requiredFirmware(command, args, values.update);
  if (needs && needs !== firmware) {
    console.error(`clocteck: ${unsupportedMessage(commandLabel(command, args, values.update), needs)}`);
    const hint = unsupportedHint(command, needs);
    if (hint) console.error(hint);
    process.exit(2);
  }

  if (firmware === 'stock') await runStock(new ClocTeckClient({ host, timeoutMs }), command, args);
  else await runTubeClock(new TubeClockClient({ host, token: values.token, timeoutMs }), command, args);
}

main().catch((err: unknown) => fail(err instanceof Error ? err.message : String(err)));
