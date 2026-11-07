/** HSV color as the clock's web UI sends it: hue 0-360, saturation and value 0-100. */
export interface Hsv {
  h: number;
  s: number;
  v: number;
}

export function hexToHsv(hex: string): Hsv {
  const m = /^#?([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(hex.trim());
  if (!m || !m[1]) throw new Error(`Invalid hex color: ${hex}`);
  let digits = m[1];
  if (digits.length === 3) digits = [...digits].map((c) => c + c).join('');
  const n = parseInt(digits, 16);
  const r = ((n >> 16) & 0xff) / 255;
  const g = ((n >> 8) & 0xff) / 255;
  const b = (n & 0xff) / 255;
  const max = Math.max(r, g, b);
  const delta = max - Math.min(r, g, b);
  let h = 0;
  if (delta > 0) {
    if (max === r) h = 60 * (((g - b) / delta) % 6);
    else if (max === g) h = 60 * ((b - r) / delta + 2);
    else h = 60 * ((r - g) / delta + 4);
  }
  if (h < 0) h += 360;
  return {
    h: Math.round(h),
    s: Math.round(max === 0 ? 0 : (delta / max) * 100),
    v: Math.round(max * 100),
  };
}

export function hsvToHex({ h, s, v }: Hsv): string {
  const sat = s / 100;
  const val = v / 100;
  const sector = Math.floor(h / 60) % 6;
  const f = h / 60 - Math.floor(h / 60);
  const p = val * (1 - sat);
  const q = val * (1 - f * sat);
  const t = val * (1 - (1 - f) * sat);
  const [r, g, b] = [
    [val, t, p],
    [q, val, p],
    [p, val, t],
    [p, q, val],
    [t, p, val],
    [val, p, q],
  ][sector]!;
  const byte = (x: number) => Math.round(x * 255).toString(16).padStart(2, '0');
  return `#${byte(r!)}${byte(g!)}${byte(b!)}`;
}

export const NAMED_COLORS: Record<string, string> = {
  red: '#ff0000',
  orange: '#ff6a00',
  yellow: '#ffd000',
  green: '#00ff00',
  cyan: '#00ffff',
  blue: '#0000ff',
  purple: '#8000ff',
  magenta: '#ff00ff',
  pink: '#ff4080',
  white: '#ffffff',
};

/** Parses a color name, "#rrggbb", "#rgb" or "h,s,v" (hue 0-360, saturation/value 0-100). */
export function parseColor(input: string): Hsv {
  const named = NAMED_COLORS[input.trim().toLowerCase()];
  if (named) return hexToHsv(named);
  const parts = input.split(',').map((p) => p.trim());
  if (parts.length === 3) {
    const [h, s, v] = parts.map(Number) as [number, number, number];
    if ([h, s, v].some((x) => !Number.isFinite(x)) || h < 0 || h > 360 || s < 0 || s > 100 || v < 0 || v > 100) {
      throw new Error(`Invalid HSV color "${input}": expected h 0-360, s 0-100, v 0-100`);
    }
    return { h, s, v };
  }
  return hexToHsv(input);
}
