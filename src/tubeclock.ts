/** The new firmware's compiled-in mDNS name. */
export const TUBECLOCK_DEFAULT_HOST = 'tubeclock.local';

/** One tube's color. Value is always 100 on the new firmware; brightness is global. */
export interface TubeHs {
  /** 0-360 */
  h: number;
  /** 0-100 */
  s: number;
}

/** The new firmware's state object (FW-API-030). Fields the firmware adds later are kept as they come. */
export interface TubeClockState {
  /** `git describe` of the build, e.g. `0.2.0-3-gabc1234` */
  version: string;
  /** Local time `HH:MM:SS` (24-hour), or `unset` while the time is unknown */
  time: string;
  /** The saved POSIX TZ rule */
  tz: string;
  is24h: boolean;
  /** 0-255 */
  brightness: number;
  /** The applied preset, or null for a custom effect (FW-API-041) */
  preset?: string | null;
  effect?: { pattern: string };
  display?: { hours: 12 | 24; leadingZero: boolean; seconds: boolean; transition: string };
  /** True while an attention flash is showing */
  attention: boolean;
  /** Six entries, index 0 = leftmost tube */
  tubes: TubeHs[];
  timeSource?: 'ntp' | 'rtc' | 'none';
  ip?: string;
  rssi?: number;
  uptimeS?: number;
  freeHeap?: number;
  hostname?: string;
  /** ISO-8601 UTC of the last accepted NTP result, or null */
  lastSync?: string | null;
  rejectedSyncs?: number;
  [key: string]: unknown;
}

export interface AttentionOptions {
  /** 1-30000, firmware default 5000 */
  ms?: number;
  blink?: boolean;
  beep?: boolean;
}

/** A palette stop placed by hand (FW-FX-002). */
export interface PaletteStop {
  /** 0-100, percent of the cycle */
  at: number;
  color: string;
}

/** A brightness envelope (FW-FX-008). `period` 0 holds the level at `max`. */
export interface EffectPulse {
  min?: number;
  max?: number;
  period?: number;
  easing?: string;
}

/** What a tube does when its digit changes (FW-FX-016). Missing fields keep their current value. */
export interface EffectAccent {
  /** flicker, flash, dip, glow or shift */
  style?: string;
  /** 50-3000 */
  ms?: number;
  /** A color, or null for the palette's last stop */
  color?: string | null;
  /** all, hm (hours and minutes) or six of 0/1, left to right */
  tubes?: string;
  /** Milliseconds per tube the accent takes to reach a neighbour, 0-1000 */
  spread?: number;
  /** 0-100 */
  strength?: number;
}

/** An effect as the firmware takes it (FW-FX-001..008). The firmware's `400` says what it refused. */
export interface Effect {
  /** solid, tubes, wave, gradient, breathe, sparkle, chase, fire, strobe or matrix */
  pattern?: string;
  /** `rainbow`, or 1-8 colors: a name, `#rrggbb`, `#rgb` or `h,s,v` */
  palette?: 'rainbow' | (string | PaletteStop)[];
  /** hsv or rgb */
  interpolate?: string;
  /** ltr, rtl, center or edges */
  direction?: string;
  /** Milliseconds per cycle: 0 or 100-600000 */
  period?: number;
  /** 0-100 */
  phase?: number;
  /** A wave's sway between rtl, center and ltr, in milliseconds: 0 (off) or 100-600000 */
  drift?: number;
  /** linear, sine, ease-in, ease-out, ease-in-out or step */
  easing?: string;
  pulse?: EffectPulse | null;
  /** sparkle, fire and matrix: how busy, 1-100 */
  density?: number;
  /** sparkle, fire and matrix: which random sequence, 0-65535 */
  seed?: number;
  /** matrix: random digits while a drop passes */
  glyphs?: boolean;
  /** Any pattern: what a tube does when its digit changes; null for nothing */
  accent?: EffectAccent | null;
  /** chase: tubes per runner, 1-6 */
  width?: number;
  /** chase: runners, 1-6 */
  count?: number;
  /** strobe: percent of each period on the first color, 0-100 */
  duty?: number;
  [key: string]: unknown;
}

/** What `GET /api/effect` answers: every field filled in, and the applied preset's name. */
export type ActiveEffect = Effect & { preset: string | null };

/** One entry of `GET /api/presets` (FW-FX-044). */
export interface Preset {
  name: string;
  builtin: boolean;
  pattern: string;
  active?: boolean;
}

/** What the digits do (FW-FX-030..033). */
export interface DisplayOptions {
  hours: 12 | 24;
  leadingZero: boolean;
  seconds: boolean;
  /** cut, crossfade, scramble or carry */
  transition: string;
  /** 50-1000 */
  fadeMs: number;
  /** Percent per tube, left to right (FW-FX-034); missing on firmware before it */
  tubeBrightness?: number[];
}

/** A change of display options. */
export type DisplayChange = Partial<DisplayOptions>;

export interface TubeClockOptions {
  host?: string;
  /** Defaults to $CLOCTECK_TOKEN, then $FW_API_TOKEN */
  token?: string;
  timeoutMs?: number;
}

/** Any non-200 answer from the firmware. */
export class HttpError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
    this.name = 'HttpError';
  }
}

/** `401`: the token is missing or wrong. */
export class AuthError extends HttpError {
  constructor(message: string) {
    super(message, 401);
    this.name = 'AuthError';
  }
}

/** `400`: the firmware rejected an argument; the message names the valid form (FW-API-002). */
export class ValidationError extends HttpError {
  constructor(message: string) {
    super(message, 400);
    this.name = 'ValidationError';
  }
}

type Params = Record<string, string | number>;

/**
 * Client for the new firmware's authenticated `/api/*` routes (spec/firmware/http-api.md).
 * Every call sends the bearer token; every write resolves with the full state.
 */
export class TubeClockClient {
  readonly host: string;
  readonly timeoutMs: number;
  // A private field keeps the token out of util.inspect, JSON.stringify and error messages (HOST-LIB-035)
  readonly #token: string;

  constructor(options: TubeClockOptions = {}) {
    this.host = options.host ?? TUBECLOCK_DEFAULT_HOST;
    this.timeoutMs = options.timeoutMs ?? 10_000;
    const token = options.token || process.env.CLOCTECK_TOKEN || process.env.FW_API_TOKEN;
    if (!token) throw new Error('No API token: pass one, or set CLOCTECK_TOKEN or FW_API_TOKEN');
    this.#token = token;
  }

  /** Sends one request and parses the JSON answer. The new firmware's responses are well-formed, so plain fetch works. */
  async request<T>(method: 'GET' | 'POST' | 'PUT' | 'DELETE', path: string, params: Params = {}, body?: unknown): Promise<T> {
    // encodeURIComponent matters for tz rules, which contain commas, '+' and '<>' (FW-API-005)
    const query = Object.entries(params)
      .map(([k, v]) => `${k}=${encodeURIComponent(String(v))}`)
      .join('&');
    const what = `${method} ${path}`;
    let res: Response;
    try {
      res = await fetch(`http://${this.host}${path}${query ? `?${query}` : ''}`, {
        method,
        headers: { Authorization: `Bearer ${this.#token}`, ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
        ...(body === undefined ? {} : { body: JSON.stringify(body) }),
        signal: AbortSignal.timeout(this.timeoutMs),
      });
    } catch (err) {
      const reason = err instanceof Error && err.name === 'TimeoutError' ? `timed out after ${this.timeoutMs} ms` : 'failed';
      throw new Error(`${what} ${reason} (is the clock reachable at ${this.host}?)`);
    }
    const text = await res.text();
    if (res.status === 401) throw new AuthError(`${what} returned 401: the token was rejected`);
    if (res.status === 400) throw new ValidationError(`${what}: ${errorMessage(text) ?? 'bad request'}`);
    if (res.status !== 200) {
      const message = errorMessage(text);
      throw new HttpError(`${what} returned HTTP ${res.status}${message ? `: ${message}` : ''}`, res.status);
    }
    try {
      return JSON.parse(text) as T;
    } catch {
      throw new HttpError(`${what} returned invalid JSON: ${text.slice(0, 200)}`, res.status);
    }
  }

  getState(): Promise<TubeClockState> {
    return this.request('GET', '/api/state');
  }

  /** Sets one tube (1-6) or all six at once. There is no per-tube value: brightness is global. */
  setTube(tube: number | 'all', h: number, s: number): Promise<TubeClockState> {
    if (tube !== 'all' && (!Number.isInteger(tube) || tube < 1 || tube > 6)) throw new RangeError('Tube must be 1-6 or "all"');
    return this.request('POST', '/api/tube', { t: tube, h: Math.round(h), s: Math.round(s) });
  }

  /** 0-255 */
  setBrightness(value: number): Promise<TubeClockState> {
    return this.request('POST', '/api/brightness', { v: Math.round(value) });
  }

  setHourFormat(format: 12 | 24): Promise<TubeClockState> {
    return this.request('POST', '/api/hours', { format });
  }

  /** A POSIX TZ rule such as `EST5EDT,M3.2.0,M11.1.0`. */
  setTimeZone(rule: string): Promise<TubeClockState> {
    return this.request('POST', '/api/tz', { tz: rule });
  }

  /** Starts a flash that ends by itself and never touches saved settings. Resolves at once with `attention: true`. */
  attention(color: TubeHs, options: AttentionOptions = {}): Promise<TubeClockState> {
    const params: Params = { h: Math.round(color.h), s: Math.round(color.s) };
    if (options.ms !== undefined) params.ms = Math.round(options.ms);
    if (options.blink !== undefined) params.blink = options.blink ? 1 : 0;
    if (options.beep !== undefined) params.beep = options.beep ? 1 : 0;
    return this.request('POST', '/api/attention', params);
  }

  cancelAttention(): Promise<TubeClockState> {
    return this.request('DELETE', '/api/attention');
  }

  /** Reboots the clock shortly after answering. */
  restart(): Promise<{ ok: boolean }> {
    return this.request('POST', '/api/restart');
  }

  getEffect(): Promise<ActiveEffect> {
    return this.request('GET', '/api/effect');
  }

  /** Changes the active effect. What `effect` leaves out keeps its value; the answer is the whole result (FW-FX-061). */
  setEffect(effect: Effect): Promise<ActiveEffect> {
    return this.request('PUT', '/api/effect', {}, effect);
  }

  listPresets(): Promise<Preset[]> {
    return this.request('GET', '/api/presets');
  }

  /** The preset's effect, every field filled in. */
  getPreset(name: string): Promise<Effect> {
    return this.request('GET', '/api/preset', { name });
  }

  /** Makes the preset the active effect. */
  applyPreset(name: string): Promise<TubeClockState> {
    return this.request('POST', '/api/preset', { name });
  }

  /** Saves `effect` under `name`, or the active effect when there is none (FW-FX-066). Resolves with what was saved. */
  savePreset(name: string, effect?: Effect): Promise<Effect> {
    return this.request('PUT', '/api/preset', { name }, effect);
  }

  deletePreset(name: string): Promise<{ ok: boolean }> {
    return this.request('DELETE', '/api/preset', { name });
  }

  getDisplay(): Promise<DisplayOptions> {
    return this.request('GET', '/api/display');
  }

  /** Changes the display options. The firmware keeps the hour format on its own route, so `hours` goes there first (FW-FX-030). */
  async setDisplay(options: DisplayChange): Promise<DisplayOptions> {
    const { hours, leadingZero, seconds, transition, fadeMs, tubeBrightness } = options;
    if (hours !== undefined) await this.setHourFormat(hours);
    const params: Params = {};
    if (leadingZero !== undefined) params.leadingZero = leadingZero ? 1 : 0;
    if (seconds !== undefined) params.seconds = seconds ? 1 : 0;
    if (transition !== undefined) params.transition = transition;
    if (fadeMs !== undefined) params.fadeMs = fadeMs;
    if (tubeBrightness !== undefined) params.tubeBrightness = tubeBrightness.join(',');
    if (hours !== undefined && Object.keys(params).length === 0) return this.getDisplay();
    return this.request('POST', '/api/display', params);
  }
}

/** The `error` field of a `{"error": "..."}` body, if that is what it is. */
function errorMessage(body: string): string | undefined {
  try {
    const json = JSON.parse(body) as { error?: unknown };
    return typeof json.error === 'string' ? json.error : undefined;
  } catch {
    return undefined;
  }
}
