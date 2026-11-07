// Pure pieces of the CLI, kept apart from cli.ts (which runs on import) so they can be unit-tested.
import type { FirmwareKind } from './detect.js';
import type { DisplayChange, Effect, TubeClockState } from './tubeclock.js';

export const COMMANDS = [
  'status', 'color', 'brightness', 'mode', 'digits', 'hours', 'timezone', 'tz', 'time',
  'sync', 'wifi', 'firmware', 'attention', 'detect', 'restart', 'preset', 'effect', 'display',
] as const;
export type Command = (typeof COMMANDS)[number];

type Firmware = Exclude<FirmwareKind, 'unknown'>;

/**
 * Which firmware a command needs, or undefined when both support it (HOST-CLI-010..025).
 * `args` and `update` matter for the few commands whose support depends on a sub-command or flag.
 */
export function requiredFirmware(command: Command, args: string[], update: boolean): Firmware | undefined {
  switch (command) {
    case 'mode': // FW-DSP-010: no color modes or digit effects in v1
    case 'digits':
    case 'time': // FW-TIME-007: no manual time-set route
    case 'wifi': // FW-SEC-001: Wi-Fi is compiled in
      return 'stock';
    case 'tz':
    case 'restart':
    case 'preset': // HOST-CLI-026..028: the effects engine is the new firmware's
    case 'effect':
    case 'display':
      return 'tubeclock';
    case 'firmware': // the new firmware updates over OTA (FW-OTA-006), not from the CLI
      return update ? 'stock' : undefined;
    case 'attention': // the stock snapshot lives in the running `attention` process; Ctrl+C there restores it
      return args[0] === 'cancel' ? 'tubeclock' : undefined;
    default:
      return undefined;
  }
}

/** HOST-CLI-007's message for a command the firmware doesn't have. */
export function unsupportedMessage(label: string, needs: Firmware): string {
  return `"${label}" needs the ${needs === 'stock' ? 'stock' : 'new'} firmware`;
}

/** A hint printed after the HOST-CLI-007 message, saying what to do instead. */
export function unsupportedHint(command: Command, needs: Firmware): string | undefined {
  if (needs === 'tubeclock') {
    if (command === 'attention') return 'On the stock firmware, press Ctrl+C in the running "clocteck attention"; it restores your colors.';
    return undefined;
  }
  switch (command) {
    case 'mode': // HOST-CLI-013
      return 'The new firmware has presets and effects instead: "preset use <name>" or "effect set <pattern>".';
    case 'time':
      return 'The new firmware sets its time over NTP and keeps it in the RTC.';
    case 'wifi':
      return 'The new firmware takes its Wi-Fi settings from .env at build time.';
    case 'firmware':
      return 'Update the new firmware over OTA from this repo.';
    default:
      return undefined;
  }
}

/** The label a command goes by in messages: `firmware --update`, `attention cancel`, or the command itself. */
export function commandLabel(command: Command, args: string[], update: boolean): string {
  if (command === 'firmware' && update) return 'firmware --update';
  if (command === 'attention' && args[0] === 'cancel') return 'attention cancel';
  return command;
}

/**
 * A fixed-offset POSIX TZ rule for a whole-hour UTC offset (HOST-CLI-016). POSIX counts hours
 * west of UTC as positive, so UTC-5 is `<-05>5` and UTC+3 is `<+03>-3`.
 */
export function utcOffsetToPosix(offset: number): string {
  if (!Number.isInteger(offset) || offset < -12 || offset > 12) throw new RangeError('UTC offset must be a whole number from -12 to 12');
  const name = `${offset < 0 ? '-' : '+'}${String(Math.abs(offset)).padStart(2, '0')}`;
  return `<${name}>${-offset}`;
}

/** The CLI's 0-100 brightness as the new firmware's 0-255 (HOST-CLI-012). */
export function percentToBrightness(percent: number): number {
  // Not percent * 2.55: that gives 127.4999... for 50
  return Math.round((percent * 255) / 100);
}

/** `--speed 1-100` as a period: 1 is 60 s, 100 is 600 ms, on a log scale (HOST-CLI-027, OQ-23). */
export function speedToPeriod(speed: number): number {
  if (!Number.isInteger(speed) || speed < 1 || speed > 100) throw new RangeError('--speed must be a whole number from 1 to 100');
  return Math.round(60000 * (600 / 60000) ** ((speed - 1) / 99));
}

/**
 * `--palette c1,c2,...`, where a color may be `h,s,v` and so contain commas itself: a bare whole number
 * starts an `h,s,v` triple. `rainbow` stays the string the firmware expects. Colors are not checked
 * here; that is the firmware's `400` (HOST-CLI-027).
 */
export function parsePalette(text: string): 'rainbow' | string[] {
  if (text === 'rainbow') return 'rainbow';
  const parts = text.split(',').map((p) => p.trim());
  const whole = (part: string | undefined) => part !== undefined && /^\d+$/.test(part);
  const colors: string[] = [];
  for (let i = 0; i < parts.length; i++) {
    const triple = whole(parts[i]) && whole(parts[i + 1]) && whole(parts[i + 2]);
    colors.push(parts.slice(i, triple ? i + 3 : i + 1).join(','));
    if (triple) i += 2;
  }
  return colors;
}

/** A number when it is one; anything else goes as it came, for the firmware's `400` to name. */
function numeric(text: string): number | string {
  return text.trim() !== '' && Number.isFinite(Number(text)) ? Number(text) : text;
}

/** `--pulse min,max[,ms]` (HOST-CLI-027). */
export function parsePulse(text: string): Effect['pulse'] {
  const [min, max, period, ...rest] = text.split(',');
  if (min === undefined || max === undefined || rest.length) throw new RangeError('--pulse is min,max or min,max,ms');
  const value = (part: string) => numeric(part) as number;
  return { min: value(min), max: value(max), ...(period === undefined ? {} : { period: value(period) }) };
}

export interface EffectFlags {
  palette?: string;
  direction?: string;
  period?: string;
  speed?: string;
  phase?: string;
  drift?: string;
  easing?: string;
  pulse?: string;
  density?: string;
  width?: string;
  count?: string;
  duty?: string;
}

/**
 * The body of `effect set` (HOST-CLI-027): only what was asked for, so the rest of the active effect
 * stays (FW-FX-061). density, width, count and duty are v1.1 fields; they go along anyway and the
 * firmware's `400` says whether it has them.
 */
export function effectBody(pattern: string, flags: EffectFlags): Effect {
  if (flags.speed !== undefined && flags.period !== undefined) throw new RangeError('--speed and --period set the same thing: use one');
  const body: Effect = { pattern };
  if (flags.palette !== undefined) body.palette = parsePalette(flags.palette);
  if (flags.direction !== undefined) body.direction = flags.direction;
  if (flags.speed !== undefined) body.period = speedToPeriod(Number(flags.speed));
  if (flags.period !== undefined) body.period = numeric(flags.period) as number;
  if (flags.phase !== undefined) body.phase = numeric(flags.phase) as number;
  if (flags.drift !== undefined) body.drift = numeric(flags.drift) as number;
  if (flags.easing !== undefined) body.easing = flags.easing;
  if (flags.pulse !== undefined) body.pulse = parsePulse(flags.pulse);
  for (const key of ['density', 'width', 'count', 'duty'] as const) {
    const value = flags[key];
    if (value !== undefined) body[key] = numeric(value);
  }
  return body;
}

function onOff(text: string, flag: string): boolean {
  if (text !== 'on' && text !== 'off') throw new RangeError(`${flag} must be on or off`);
  return text === 'on';
}

export interface DisplayFlags {
  hours?: string;
  leadingZero?: string;
  seconds?: string;
  transition?: string;
  fade?: string;
  tubeBrightness?: string;
  night?: string;
}

/** What `display` was asked to change (HOST-CLI-028); empty means it only prints the options. */
export function displayChange(flags: DisplayFlags): DisplayChange {
  const change: DisplayChange = {};
  if (flags.hours !== undefined) {
    if (flags.hours !== '12' && flags.hours !== '24') throw new RangeError('--hours must be 12 or 24');
    change.hours = flags.hours === '24' ? 24 : 12;
  }
  if (flags.leadingZero !== undefined) change.leadingZero = onOff(flags.leadingZero, '--leading-zero');
  if (flags.seconds !== undefined) change.seconds = onOff(flags.seconds, '--seconds');
  if (flags.transition !== undefined) change.transition = flags.transition;
  if (flags.fade !== undefined) change.fadeMs = numeric(flags.fade) as number;
  if (flags.tubeBrightness !== undefined) change.tubeBrightness = flags.tubeBrightness.split(',').map((v) => numeric(v) as number);
  if (flags.night !== undefined) change.night = flags.night;
  return change;
}

/** Whether the new firmware has the time from NTP (HOST-CLI-019): `lastSync` when reported, else a known time. */
export function timeSynced(state: TubeClockState): boolean {
  if (state.lastSync !== undefined) return state.lastSync !== null;
  return state.time !== 'unset';
}
