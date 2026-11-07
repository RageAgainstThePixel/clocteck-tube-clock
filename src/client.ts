import http from 'node:http';
import type { Hsv } from './color.js';
import { splitHost } from './detect.js';

/** The clock's address on its own setup hotspot (ClocTeck_xxxx). */
export const DEFAULT_HOST = '192.168.18.1';

export const COLOR_MODES = ['custom', 'rainbow', 'breathing', 'flow'] as const;
export type ColorMode = (typeof COLOR_MODES)[number];

/** How digits change: plain, rolling up through intermediate digits, or a scrambled shuffle. */
export const DIGIT_EFFECTS = ['normal', 'carry', 'scramble'] as const;
export type DigitEffect = (typeof DIGIT_EFFECTS)[number];

/** Tube indexes, 1 = leftmost. The CT-001 has six tubes (FW-DSP-001, OQ-19). */
export const TUBES = [1, 2, 3, 4, 5, 6] as const;

export interface TubeColor {
  tube: number;
  /** 0-360 */
  hue: number;
  /** 0-100 */
  saturation: number;
}

export interface ClockConfig {
  tubes: TubeColor[];
  /** 0-100, shared by all tubes */
  brightness: number;
  mode: ColorMode;
  digitEffect: DigitEffect;
  is24Hour: boolean;
  /** UTC offset in whole hours, -12 to +12 */
  utcOffset: number;
  alarm: { hour: number; minute: number };
  /** The unparsed /config response */
  raw: Record<string, unknown>;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
}

export interface WifiConfig {
  ssid: string;
  password: string;
  /** The clock's address on the home network; empty when it isn't connected */
  ip: string;
}

export interface ClientOptions {
  host?: string;
  timeoutMs?: number;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Client for the HTTP API the clock's /admin page uses. Every call is an unauthenticated GET.
 */
export class ClocTeckClient {
  readonly host: string;
  readonly timeoutMs: number;

  constructor(options: ClientOptions = {}) {
    this.host = options.host ?? DEFAULT_HOST;
    this.timeoutMs = options.timeoutMs ?? 10_000;
  }

  /** Sends a GET and returns the body. The firmware's responses trip strict HTTP parsers, so this uses the lenient one. */
  request(path: string, params: Record<string, string | number> = {}): Promise<string> {
    const query = Object.entries(params)
      .map(([k, v]) => `${k}=${encodeURIComponent(String(v))}`)
      .join('&');
    const fullPath = query ? `${path}?${query}` : path;
    return new Promise((resolve, reject) => {
      const req = http.get(
        { ...splitHost(this.host), path: fullPath, timeout: this.timeoutMs, insecureHTTPParser: true },
        (res) => {
          const chunks: Buffer[] = [];
          res.on('data', (c: Buffer) => chunks.push(c));
          res.on('end', () => {
            const body = Buffer.concat(chunks).toString('utf8');
            // Errors name the path alone: the query of /wificonn holds the Wi-Fi password.
            if (res.statusCode !== 200) reject(new Error(`GET ${path} returned HTTP ${res.statusCode}`));
            else resolve(body);
          });
          res.on('error', reject);
        },
      );
      req.on('timeout', () => req.destroy(new Error(`GET ${path} timed out after ${this.timeoutMs} ms (is the clock reachable at ${this.host}?)`)));
      req.on('error', reject);
    });
  }

  private async requestJson<T>(path: string, params?: Record<string, string | number>): Promise<T> {
    const body = await this.request(path, params);
    try {
      return JSON.parse(body) as T;
    } catch {
      throw new Error(`GET ${path} returned invalid JSON: ${body.slice(0, 200)}`);
    }
  }

  async getConfig(): Promise<ClockConfig> {
    const raw = await this.requestJson<Record<string, number>>('/config');
    const num = (key: string) => Number(raw[key] ?? 0);
    return {
      tubes: TUBES.map((tube) => ({
        tube,
        hue: num(`h${tube}`),
        saturation: Math.round(num(`s${tube}`) / 2.55),
      })),
      brightness: Math.round(num('light') / 2.55),
      mode: COLOR_MODES[num('mode') - 1] ?? 'custom',
      digitEffect: DIGIT_EFFECTS[num('outcarry')] ?? 'normal',
      is24Hour: Boolean(num('is24h')),
      utcOffset: num('tz'),
      // The vendor page reads "ah"/"am", but firmware 2110 sends "alarmh"/"alarmm"
      alarm: { hour: num(raw.alarmh !== undefined ? 'alarmh' : 'ah'), minute: num(raw.alarmm !== undefined ? 'alarmm' : 'am') },
      raw,
    };
  }

  async getVersion(): Promise<string> {
    // Firmware 2110 sends the version as a number
    return String((await this.requestJson<{ ver: string | number }>('/version')).ver);
  }

  /** Asks the clock for the newest firmware available online. Only works once it is on the internet. */
  async getLatestVersion(): Promise<{ version: string; notes: string }> {
    const json = await this.requestJson<{ nv?: string | number; ni?: string }>('/nVersion');
    return { version: String(json.nv ?? ''), notes: json.ni ?? '' };
  }

  /** Saved Wi-Fi settings. Note the password comes back in plain text. */
  async getWifiConfig(): Promise<WifiConfig> {
    const json = await this.requestJson<{ ssid?: string; pwd?: string; ip?: string }>('/wificonf');
    return { ssid: json.ssid ?? '', password: json.pwd ?? '', ip: json.ip ?? '' };
  }

  async scanWifi(timeoutMs = 20_000): Promise<WifiNetwork[]> {
    await this.request('/wifiscan');
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await sleep(500);
      const body = await this.request('/wifiscanover');
      if (body.trim() !== 'wait') {
        const list = JSON.parse(body) as { ssid: string; rssi: number | string }[];
        return list.map((n) => ({ ssid: n.ssid, rssi: Number(n.rssi) })).sort((a, b) => b.rssi - a.rssi);
      }
    }
    throw new Error('Wi-Fi scan did not finish in time');
  }

  /** Joins the clock to a 2.4 GHz network and resolves with its new IP address. */
  async connectWifi(ssid: string, password: string, timeoutMs = 60_000): Promise<string> {
    await this.request('/wificonn', { s: ssid, p: password });
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await sleep(1000);
      const status = (await this.request('/wificonn', { st: 1 })).trim();
      if (status === '3') return (await this.getWifiConfig()).ip;
      if (status === '4') throw new Error(`Wrong password for "${ssid}"`);
      if (status === '1') throw new Error(`Could not connect to "${ssid}"`);
    }
    throw new Error(`Timed out connecting to "${ssid}"`);
  }

  /**
   * Sets one tube's color. The value (brightness) applies to every tube, as in the web UI.
   * Only takes visible effect in the "custom" color mode.
   */
  async setTubeColor(tube: number, color: Hsv): Promise<void> {
    if (!Number.isInteger(tube) || tube < 1 || tube > TUBES.length) throw new Error(`Tube must be 1-${TUBES.length}`);
    await this.request('/tubecolor', {
      t: tube,
      h: Math.round(color.h),
      s: Math.round(color.s),
      v: Math.round(color.v),
    });
  }

  async setAllTubes(color: Hsv): Promise<void> {
    for (const tube of TUBES) await this.setTubeColor(tube, color);
  }

  /** Changes brightness (0-100) while keeping tube 1's current color. */
  async setBrightness(brightness: number): Promise<void> {
    if (brightness < 0 || brightness > 100) throw new Error('Brightness must be 0-100');
    const { tubes } = await this.getConfig();
    const first = tubes[0]!;
    await this.setTubeColor(1, { h: first.hue, s: first.saturation, v: brightness });
  }

  /** The clock sets mode and digit effect together; whichever is omitted keeps its current value. */
  async setMode(mode?: ColorMode, digitEffect?: DigitEffect): Promise<void> {
    if (!mode || !digitEffect) {
      const config = await this.getConfig();
      mode ??= config.mode;
      digitEffect ??= config.digitEffect;
    }
    await this.request('/mode', { m: COLOR_MODES.indexOf(mode) + 1, s: DIGIT_EFFECTS.indexOf(digitEffect) + 1 });
  }

  async setHourFormat(format: 12 | 24): Promise<void> {
    await this.request('/tm', { s: format });
  }

  /** Sets the time zone as a whole-hour UTC offset, -12 to +12. */
  async setUtcOffset(offset: number): Promise<void> {
    if (!Number.isInteger(offset) || offset < -12 || offset > 12) throw new Error('UTC offset must be a whole number from -12 to 12');
    await this.request('/time', { t: offset + 13 });
  }

  /** Sets the clock from a Date. The clock takes UTC time and applies its own time zone setting. */
  async setTime(date: Date = new Date()): Promise<void> {
    await this.request('/uptm', { t: 0, h: date.getUTCHours(), m: date.getUTCMinutes(), s: date.getUTCSeconds() });
  }

  /** Has the clock fetch the time from the internet. Only works once it is on your Wi-Fi. */
  async syncTime(): Promise<void> {
    await this.request('/uptm', { t: 1 });
  }

  /**
   * Starts an online firmware update and reports progress until it finishes.
   * Never cut power while this runs: the vendor warns it can brick the clock.
   */
  async updateFirmware(onProgress?: (percent: number) => void, timeoutMs = 10 * 60_000): Promise<void> {
    await this.request('/update');
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await sleep(1000);
      let body: string;
      try {
        body = (await this.request('/progress')).trim();
      } catch {
        continue; // the clock is busy flashing; keep polling
      }
      const percent = Number(body);
      if (percent === 255) throw new Error('Firmware update failed');
      if (Number.isFinite(percent)) onProgress?.(percent);
      if (percent === 100) return;
    }
    throw new Error('Timed out waiting for the firmware update to finish');
  }
}
