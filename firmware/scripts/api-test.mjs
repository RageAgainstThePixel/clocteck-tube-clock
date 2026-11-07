// End-to-end test of the firmware's web API, against the emulator (default) or a real clock:
//   node firmware/scripts/api-test.mjs [http://localhost:9080]
// Reads FW_API_TOKEN from the repo's .env without printing it, and puts the settings back afterwards.
// Check names start with the requirement IDs they cover (spec/verification.md, VER-002). The test
// restarts the clock (FW-API-018) and checks what survived (FW-FX-050); against localhost it starts
// the emulator again for that, and the last request makes it exit. The checks that read the [leds]
// and [fx] log (VER-020, VER-021) only run against the emulator.
import { execFileSync, execSync, spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { connect } from 'node:net';
import { fileURLToPath } from 'node:url';

const base = process.argv[2] || 'http://localhost:9080';
const env = readFileSync(new URL('../../.env', import.meta.url), 'utf8');
const token = [...env.matchAll(/^FW_API_TOKEN=(.*)$/gm)].at(-1)?.[1].trim().replace(/^"(.*)"$/, '$1');
if (!token) throw new Error('FW_API_TOKEN is empty in .env');
const auth = { Authorization: `Bearer ${token}` };

let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${ok ? '' : ` ${detail}`}`);
  if (!ok) failed++;
};
const req = async (method, path, headers = auth) => {
  const r = await fetch(base + path, { method, headers });
  const text = await r.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {}
  return { status: r.status, text, json, headers: r.headers };
};
const timed = async (...args) => {
  const start = performance.now();
  const r = await req(...args);
  return { ...r, ms: performance.now() - start };
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// A request with a body; a string goes as it is, anything else as JSON (OQ-26).
const send = async (method, path, body, headers = { ...auth, 'Content-Type': 'application/json' }) => {
  const r = await fetch(base + path, { method, headers, body: typeof body === 'string' ? body : JSON.stringify(body) });
  const text = await r.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {}
  return { status: r.status, text, json, headers: r.headers };
};

const repo = fileURLToPath(new URL('../..', import.meta.url));
const local = /^http:\/\/(localhost|127\.0\.0\.1)[:/]/.test(`${base}/`);
const inWsl = (...args) => (process.platform === 'win32' ? ['wsl', ['-d', 'Ubuntu-24.04', '--', ...args]] : [args[0], args.slice(1)]);
// The emulator's serial log so far, or '' when there is none to read (a real clock).
const emulatorLog = () => {
  if (!local) return '';
  try {
    const [cmd, args] = inWsl('sh', 'firmware/scripts/emulator-log.sh');
    return execFileSync(cmd, args, { cwd: repo, encoding: 'utf8', maxBuffer: 1 << 26, stdio: ['ignore', 'pipe', 'ignore'] });
  } catch {
    return '';
  }
};
const ledLines = (log) => [...log.matchAll(/\[leds\] lit=(\d) brightness=(\d+) digits=(\S+) tubes=(\S+)/g)].map((m) => ({ lit: m[1] === '1', brightness: Number(m[2]), digits: m[3], tubes: m[4].trim() }));

let r = await req('GET', '/health', {});
check('FW-API-010 FW-SEC-005 health is open', r.status === 200 && r.text === 'ok', r.status);

r = await req('GET', '/api/state', {});
check('FW-SEC-005 FW-SEC-007 state without token is 401', r.status === 401 && r.headers.get('www-authenticate') === 'Bearer' && r.json?.error === 'unauthorized', r.status);
r = await req('GET', '/api/state', { Authorization: 'Bearer wrong' });
check('FW-SEC-007 state with wrong token is 401', r.status === 401 && r.json?.error === 'unauthorized', r.status);
r = await req('POST', '/api/brightness?v=10', {});
check('FW-SEC-005 write without token is 401', r.status === 401, r.status);

// FW-API-009, VER-012: fetch (undici) is a strict HTTP parser.
r = await req('GET', '/api/state');
check('FW-API-011 FW-API-009 FW-API-036 state with token', r.status === 200 && r.json?.tubes?.length === 6, r.text);
const before = r.json;
const beforeEffect = (await req('GET', '/api/effect')).json;
const beforeDisplay = (await req('GET', '/api/display')).json;
const restoreEffect = async () => {
  if (beforeEffect.preset) return req('POST', `/api/preset?name=${encodeURIComponent(beforeEffect.preset)}`);
  const { preset: _, ...effect } = beforeEffect;
  return send('PUT', '/api/effect', effect);
};
const restoreDisplay = () =>
  req('POST', `/api/display?leadingZero=${beforeDisplay.leadingZero ? 1 : 0}&seconds=${beforeDisplay.seconds ? 1 : 0}&transition=${beforeDisplay.transition}&fadeMs=${beforeDisplay.fadeMs}`);
r = await req('GET', '/api/state', {});
check('FW-SEC-006 no token right after a valid one is still 401', r.status === 401, r.status);
r = await req('GET', '/api/state', { Authorization: 'Bearer' });
check('FW-SEC-006 bare "Bearer" right after a valid one is 401', r.status === 401, r.status);

// FW-API-031, VER-013: the version is `git describe`. Untagged checkouts describe as a bare hash.
const version = before.version ?? '';
check(
  'FW-API-031 version is a git describe string',
  /^\d+\.\d+\.\d+(-\d+-g[0-9a-f]+)?(-dirty)?$/.test(version) || /^[0-9a-f]{7,40}(-dirty)?$/.test(version),
  version,
);
if (local) {
  const describe = execSync('git describe --tags --always --dirty', { cwd: repo })
    .toString()
    .trim()
    .replace(/^v/, '');
  check('FW-API-031 emulator version matches this checkout', version === describe, `${version} vs ${describe}`);
}
// FW-API-040, FW-TIME-016
check(
  'FW-API-040 FW-TIME-016 state has lastSync and rejectedSyncs',
  (before.lastSync === null || /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(before.lastSync)) && Number.isInteger(before.rejectedSyncs),
  JSON.stringify({ lastSync: before.lastSync, rejectedSyncs: before.rejectedSyncs }),
);
// FW-API-037, FW-TIME-006: the emulator has no NTP and no RTC, whatever its host's clock says.
check('FW-API-037 FW-TIME-004 state says where the time came from', local ? before.timeSource === 'none' : ['ntp', 'rtc', 'none'].includes(before.timeSource), before.timeSource);
check('FW-API-039 state names the hostname', typeof before.hostname === 'string' && before.hostname.length > 0, before.hostname);
check('FW-API-032 FW-TIME-004 time is HH:MM:SS or unset', /^(\d\d:\d\d:\d\d|unset)$/.test(before.time), before.time);

for (const path of ['/wificonf', '/config', '/wifiscan', '/admin']) {
  r = await req('GET', path, {});
  check(`FW-SEC-011 stock route ${path} is gone`, r.status === 404 && !r.text.includes('pwd'), r.status);
}
r = await req('GET', '/nope');
check('FW-API-019 unknown path is 404', r.status === 404 && r.json?.error === 'not found', r.status);

r = await req('POST', '/api/tube?t=3&h=0&s=100');
check('FW-API-012 set tube 3 red', r.status === 200 && r.json.tubes[2].h === 0 && r.json.tubes[0].h === before.tubes[0].h, r.text);
r = await req('POST', '/api/tube?t=all&h=120&s=50');
check('FW-API-012 FW-API-008 set all tubes', r.status === 200 && r.json.tubes.every((t) => t.h === 120 && t.s === 50), r.text);
r = await req('POST', '/api/tube?t=all&h=120&s=50');
check('FW-API-003 FW-SET-007 repeated identical write still answers with the state', r.status === 200 && r.json.tubes.every((t) => t.h === 120), r.text);
r = await req('POST', '/api/tube?t=7&h=0&s=100');
check('FW-API-012 FW-API-002 tube 7 rejected', r.status === 400 && /^need /.test(r.json?.error ?? ''), r.status);
r = await req('POST', '/api/tube?t=1&h=999&s=100');
check('FW-API-012 hue 999 rejected', r.status === 400, r.status);
r = await req('GET', '/api/state');
check('FW-FX-062 FW-API-041 /api/tube switches to the tubes pattern, preset null', r.json?.effect?.pattern === 'tubes' && r.json?.preset === null && r.json?.tubes[0].h === 120, r.text);
r = await req('GET', '/api/effect');
check('FW-FX-062 FW-DSP-011 the saved colors are the tubes palette', r.json?.palette?.length === 6 && r.json.palette.every((c) => c === r.json.palette[0]), r.text);

// FW-API-020: known path, wrong method.
r = await req('GET', '/api/tube?t=1&h=0&s=100');
check('FW-API-020 GET on a write route is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'POST', `${r.status} ${r.headers.get('allow')}`);
r = await req('PUT', '/api/attention');
check('FW-API-020 PUT /api/attention is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'POST, DELETE', `${r.status} ${r.headers.get('allow')}`);
r = await req('POST', '/health', {});
check('FW-API-020 POST /health is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'GET', `${r.status} ${r.headers.get('allow')}`);
r = await req('GET', '/api/restart', {});
check('FW-API-020 FW-SEC-005 wrong method on /api without token is 401', r.status === 401, r.status);

r = await req('POST', '/api/brightness?v=200');
check('FW-API-013 brightness 200', r.json?.brightness === 200, r.text);
r = await req('POST', '/api/brightness?v=256');
check('FW-API-013 FW-API-001 brightness 256 rejected', r.status === 400, r.status);
r = await req('POST', '/api/brightness?v=-');
check('FW-API-001 brightness sign only rejected', r.status === 400, r.status);

r = await req('POST', '/api/hours?format=24');
check('FW-API-014 24-hour', r.json?.is24h === true, r.text);
r = await req('POST', '/api/hours?format=13');
check('FW-API-014 format 13 rejected', r.status === 400, r.status);

r = await req('POST', '/api/tz?tz=UTC0');
check('FW-API-015 FW-API-033 tz UTC0', r.json?.tz === 'UTC0', r.text);
r = await req('POST', `/api/tz?tz=${encodeURIComponent('bad"tz')}`);
check('FW-API-015 FW-SET-011 tz with quote rejected', r.status === 400, r.status);

r = await req('POST', '/api/attention?h=30&s=100&ms=1500');
check('FW-API-016 FW-ATT-009 attention starts', r.json?.attention === true, r.text);
check('FW-API-004 FW-ATT-002 attention leaves saved colors alone', r.json?.tubes.every((t) => t.h === 120), r.text);
await sleep(2000);
r = await req('GET', '/api/state');
check('FW-ATT-009 attention ends by itself', r.json?.attention === false, r.text);

r = await req('POST', '/api/attention?h=0&s=100&ms=30000&blink=1');
check('FW-API-016 FW-ATT-003 blinking attention starts', r.json?.attention === true, r.text);
r = await req('DELETE', '/api/attention');
check('FW-API-017 FW-ATT-005 attention cancel', r.json?.attention === false, r.text);
r = await req('POST', '/api/attention?h=0&s=100&ms=60000');
check('FW-API-016 attention over 30 s rejected', r.status === 400, r.status);
// FW-ATT-010, VER-015: the sound itself is checklist 11; here the argument is accepted and checked.
r = await req('POST', '/api/attention?h=0&s=100&ms=100&beep=1');
check('FW-API-016 FW-ATT-010 beep=1 accepted', r.status === 200 && r.json?.attention === true, r.text);
r = await req('POST', '/api/attention?h=0&s=100&ms=100&beep=2');
check('FW-API-016 FW-ATT-010 beep=2 rejected', r.status === 400, r.status);

await req('POST', `/api/tube?t=all&h=${before.tubes[0].h}&s=${before.tubes[0].s}`);
await req('POST', `/api/brightness?v=${before.brightness}`);
await req('POST', `/api/hours?format=${before.is24h ? 24 : 12}`);
r = await req('POST', `/api/tz?tz=${encodeURIComponent(before.tz)}`);
check('VER-004 settings restored', JSON.stringify(r.json?.tubes) === JSON.stringify(before.tubes) && r.json?.tz === before.tz, r.text);
console.log(`clock time: ${r.json?.time} (${r.json?.tz})`);

// ---- Effects (VER-020) ----

const fullEffect = (e) => ['pattern', 'palette', 'interpolate', 'direction', 'period', 'phase', 'drift', 'easing', 'pulse'].every((k) => k in (e ?? {}));
const putEffect = (body) => send('PUT', '/api/effect', body);

r = await req('GET', '/api/effect');
check('FW-FX-060 GET /api/effect fills in every field and the preset', r.status === 200 && fullEffect(r.json) && 'preset' in r.json, r.text);
r = await req('GET', '/api/effect', {});
check('FW-FX-060 FW-SEC-005 effect without token is 401', r.status === 401, r.status);
r = await send('PUT', '/api/effect', { period: 1000 }, { 'Content-Type': 'application/json' });
check('FW-FX-061 FW-SEC-005 PUT without token is 401', r.status === 401, r.status);
r = await req('POST', '/api/effect');
check('FW-API-020 POST /api/effect is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'GET, PUT', `${r.status} ${r.headers.get('allow')}`);
r = await req('PATCH', '/api/preset?name=ocean');
check('FW-API-020 PATCH /api/preset is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'GET, POST, PUT, DELETE', `${r.status} ${r.headers.get('allow')}`);
r = await req('POST', '/api/presets');
check('FW-API-020 POST /api/presets is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'GET', `${r.status} ${r.headers.get('allow')}`);
r = await req('PUT', '/api/display');
check('FW-API-020 PUT /api/display is 405 with Allow', r.status === 405 && r.headers.get('allow') === 'GET, POST', `${r.status} ${r.headers.get('allow')}`);

// FW-FX-061: a partial body keeps the rest.
r = await putEffect({ pattern: 'wave', palette: ['red', '#00f', { at: 80, color: '120,100,100' }], interpolate: 'rgb', direction: 'rtl', period: 5000, phase: 20, easing: 'sine', pulse: { min: 10, max: 200, period: 3000, easing: 'linear' } });
check('FW-FX-061 FW-FX-001..008 PUT a whole effect', r.status === 200 && r.json?.pattern === 'wave' && r.json.palette[1] === '#0000ff' && r.json.palette[2].at === 80 && r.json.direction === 'rtl' && r.json.pulse?.max === 200, r.text);
r = await putEffect({ period: 7000 });
check('FW-FX-061 a partial body keeps the other fields', r.status === 200 && r.json?.period === 7000 && r.json.direction === 'rtl' && r.json.phase === 20 && r.json.easing === 'sine' && r.json.palette[0] === '#ff0000' && r.json.pulse?.min === 10, r.text);
r = await putEffect({ drift: 60000 });
check('FW-FX-015 drift is set and answered', r.status === 200 && r.json?.drift === 60000 && r.json.direction === 'rtl', r.text);
r = await putEffect({ drift: 0 });
check('FW-FX-015 drift 0 turns it off', r.status === 200 && r.json?.drift === 0, r.text);
r = await putEffect({ pulse: null });
check('FW-FX-008 pulse null removes it', r.status === 200 && r.json?.pulse === null, r.text);
r = await putEffect({ pattern: 'breathe' });
check('FW-FX-061 FW-FX-024 switching to breathe resets its own fields', r.json?.pattern === 'breathe' && r.json.easing === 'sine' && r.json.pulse?.min === 32 && r.json.pulse?.max === 255 && r.json.period === 7000, r.text);
r = await req('GET', '/api/state');
check('FW-FX-069 state carries effect.pattern', r.json?.effect?.pattern === 'breathe' && r.json?.preset === null, r.text);

// FW-FX-013, VER-020: every bad body is a 400 naming the field, and changes nothing.
const unchanged = (await req('GET', '/api/effect')).text;
for (const [name, body, field] of [
  ['unknown field', { pattern: 'wave', bogus: 1 }, 'unknown field'],
  ['v1.1 field', { pattern: 'wave', density: 20 }, 'unknown field'],
  ['v1.1 pattern', { pattern: 'fire' }, 'pattern'],
  ['bad palette entry', { palette: ['#ff0000', 'nope'] }, 'palette'],
  ['palette of the wrong type', { palette: 5 }, 'palette'],
  ['9-stop palette', { palette: Array(9).fill('#ff0000') }, 'palette'],
  ['stops out of order', { palette: [{ at: 50, color: 'red' }, { at: 10, color: 'blue' }] }, 'palette'],
  ['period 99', { period: 99 }, 'period'],
  ['period as a string', { period: '1000' }, 'period'],
  ['phase 101', { phase: 101 }, 'phase'],
  ['drift 99', { drift: 99 }, 'drift'],
  ['bad easing', { easing: 'bounce' }, 'easing'],
  ['bad direction', { direction: 'up' }, 'direction'],
  ['bad interpolate', { interpolate: 'lab' }, 'interpolate'],
  ['pulse min over max', { pulse: { min: 9, max: 8 } }, 'pulse'],
  ['not an object', '[1,2]', 'JSON object'],
  ['not JSON', '{"pattern":', 'JSON object'],
  ['no body', '', 'JSON object'],
]) {
  r = await putEffect(body);
  check(`FW-FX-013 FW-API-002 ${name} is 400 naming ${field}`, r.status === 400 && r.json?.error?.includes(field), `${r.status} ${r.text}`);
}
r = await req('GET', '/api/effect');
check('FW-FX-013 a refused body changes nothing', r.text === unchanged, r.text);
r = await send('PUT', '/api/effect', { period: 1000 }, { ...auth, 'Content-Type': 'text/plain' });
check('FW-FX-061 OQ-26 a body that is not application/json is 415', r.status === 415, r.status);
r = await send('PUT', '/api/effect', '{"period":1000}', { ...auth, 'Content-Type': 'application/json; charset=utf-8' });
check('FW-FX-061 application/json with a charset is fine', r.status === 200 && r.json?.period === 1000, r.text);
const padded = (n) => {
  const json = '{"period":2000}';
  return json + ' '.repeat(n - json.length);
};
r = await putEffect(padded(1024));
check('FW-FX-014 FW-BUD-004 a 1024-byte body is read', r.status === 200 && r.json?.period === 2000, `${r.status} ${r.text}`);
r = await putEffect(padded(1025));
check('FW-FX-014 FW-BUD-004 FW-SEC-013 a 1025-byte body is 413', r.status === 413 && /1024/.test(r.json?.error ?? ''), `${r.status} ${r.text}`);
r = await send('PUT', '/api/preset?name=toobig', padded(1025));
check('FW-FX-066 FW-SEC-013 the same on PUT /api/preset', r.status === 413, r.status);
r = await send('PUT', '/api/effect', padded(4000), { 'Content-Type': 'application/json' });
check('FW-SEC-005 FW-SEC-013 an oversized body without a token is 401', r.status === 401, r.status);
r = await req('GET', '/api/state');
check('FW-SEC-006 the oversized requests left nothing behind', r.status === 200 && r.json?.effect?.pattern === 'breathe', r.text);

// FW-SEC-013, FW-BUD-004: the other routes refuse a large body too, unread and without a stall.
const raw = (text) =>
  new Promise((resolve) => {
    const { hostname, port } = new URL(base);
    const start = performance.now();
    let got = '';
    const socket = connect(Number(port) || 80, hostname, () => socket.write(text));
    socket.setEncoding('utf8');
    socket.setTimeout(8000, () => socket.destroy(new Error('no answer in 8 s')));
    socket.on('data', (chunk) => (got += chunk));
    socket.on('error', () => {});  // 'close' follows, and the check reports what arrived
    socket.on('close', () => resolve({ status: Number(got.match(/^HTTP\/1\.1 (\d+)/)?.[1]), text: got, ms: performance.now() - start }));
  });
const brightnessNow = (await req('GET', '/api/state')).json?.brightness;
const plain = { ...auth, 'Content-Type': 'text/plain' };
const notSet = (brightnessNow + 1) % 256;
r = await send('POST', `/api/brightness?v=${notSet}`, 'x'.repeat(1025), plain);
check('FW-SEC-013 FW-BUD-004 a 1025-byte body on /api/brightness is 413', r.status === 413 && /1024/.test(r.json?.error ?? ''), `${r.status} ${r.text}`);
r = await send('POST', `/api/brightness?v=${notSet}`, 'x'.repeat(5000), { 'Content-Type': 'text/plain' });
check('FW-SEC-005 FW-SEC-013 the same without a token is 401', r.status === 401, r.status);
r = await req('GET', '/api/state');
check('FW-SEC-013 a refused request changes nothing', r.json?.brightness === brightnessNow, r.text);
r = await send('POST', `/api/brightness?v=${brightnessNow}`, 'x'.repeat(1024), plain);
check('FW-SEC-013 a 1024-byte body on a query route is ignored', r.status === 200 && r.json?.brightness === brightnessNow, `${r.status} ${r.text}`);
r = await send('POST', '/update', 'x'.repeat(5000), { 'Content-Type': 'text/plain' });
check('FW-SEC-011 FW-SEC-013 a stock route refuses a large body, with no data and no token needed', r.status === 413 && !r.text.includes('pwd'), `${r.status} ${r.text}`);
r = await send('POST', '/update', 'x', { 'Content-Type': 'text/plain' });
check('FW-SEC-011 a stock route with a small body is still 404', r.status === 404, `${r.status} ${r.text}`);
r = await send('POST', '/health', 'x'.repeat(5000), { 'Content-Type': 'text/plain' });
check('FW-SEC-013 POST /health with a large body is 413', r.status === 413, `${r.status} ${r.text}`);
r = await send('POST', `/api/brightness?v=${notSet}`, '--x--\r\n', { ...auth, 'Content-Type': 'multipart/form-data; boundary=x' });
check('FW-SEC-013 a multipart body is 415', r.status === 415, `${r.status} ${r.text}`);
r = await send('POST', `/api/brightness?v=${notSet}`, '--x--\r\n', { 'Content-Type': 'multipart/form-data; boundary=x' });
check('FW-SEC-005 FW-SEC-013 a multipart body without a token is 401', r.status === 401, `${r.status} ${r.text}`);
const host = new URL(base).host;
let rawReply = await raw(`POST /nope HTTP/1.1\r\nHost: ${host}\r\nContent-Length: 200000\r\nConnection: close\r\n\r\n`);
check('FW-SEC-013 a declared 200 KB is 413 at once, not waited for', rawReply.status === 413 && rawReply.ms < 1500, `${rawReply.status} ${Math.round(rawReply.ms)} ms`);
rawReply = await raw(`POST /nope HTTP/1.1\r\nHost: ${host}\r\nX: y\rZ\nContent-Length: 200000\r\nConnection: close\r\n\r\n`);
check('FW-SEC-013 a stray CR does not hide the length', rawReply.status === 413 && rawReply.ms < 1500, `${rawReply.status} ${Math.round(rawReply.ms)} ms`);
rawReply = await raw(`POST /nope HTTP/1.1\r\nHost: ${host}\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n`);
check('FW-SEC-013 a chunked body is 411', rawReply.status === 411, `${rawReply.status}`);
rawReply = await raw(`POST /nope HTTP/1.1\r\nHost: ${host}\r\nX-Long: ${'y'.repeat(800)}\r\nConnection: close\r\n\r\n`);
check('FW-SEC-013 headers too long to check are 431', rawReply.status === 431, `${rawReply.status}`);
rawReply = await raw(`GET /health HTTP/1.1\r\nHost: ${host}\r\nX-Long: ${'y'.repeat(800)}\r\nContent-Length: 200000\r\nConnection: close\r\n\r\n`);
check('FW-API-010 FW-SEC-013 a GET reads no body, so nothing is refused', rawReply.status === 200 && rawReply.text.endsWith('ok'), `${rawReply.status}`);
rawReply = await raw(`POST /nope HTTP/1.1\r\nHost: ${host}\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial`);
check('FW-BUD-006 a stalled body is 408 and does not hold the server for its 5 s read', rawReply.status === 408 && rawReply.ms < 3000, `${rawReply.status} ${Math.round(rawReply.ms)} ms`);

// FW-FX-041, FW-FX-043..045, VER-020: presets.
const listPresets = async () => (await req('GET', '/api/presets')).json ?? [];
for (const p of await listPresets()) {
  if (!p.builtin && /^(mine|api-test)/i.test(p.name)) await req('DELETE', `/api/preset?name=${encodeURIComponent(p.name)}`);  // an earlier failed run
}
let presets = await listPresets();
const BUILTIN = ['rainbow-wave', 'nixie', 'classic-blue', 'ocean', 'sunset', 'synthwave', 'aurora', 'lava', 'candy', 'ice', 'night', 'vaporwave'];
check('FW-FX-044 FW-FX-063 the built-in presets are listed', BUILTIN.every((n) => presets.some((p) => p.name === n && p.builtin === true && typeof p.pattern === 'string')), JSON.stringify(presets));
check('FW-FX-001 no v1.1 preset yet', !presets.some((p) => ['fire', 'candle', 'police', 'matrix', 'cyberpunk', 'twinkle'].includes(p.name)), JSON.stringify(presets));
for (const name of BUILTIN) {
  r = await req('GET', `/api/preset?name=${name}`);
  check(`FW-FX-064 ${name} reads back whole`, r.status === 200 && fullEffect(r.json) && !('preset' in r.json), r.text);
}
r = await req('GET', '/api/preset?name=nope');
check('FW-FX-064 unknown preset is 404', r.status === 404, r.status);
r = await req('POST', '/api/preset?name=nope');
check('FW-FX-065 applying an unknown preset is 404', r.status === 404, r.status);
r = await req('POST', '/api/preset?name=ocean');
check('FW-FX-065 FW-FX-043 applying ocean sets preset', r.status === 200 && r.json?.preset === 'ocean' && r.json?.effect?.pattern === 'wave', r.text);
r = await req('GET', '/api/effect');
check('FW-FX-043 the effect is now ocean', r.json?.preset === 'ocean' && r.json.period === 5000 && r.json.phase === 25 && r.json.palette.length === 8, r.text);
presets = await listPresets();
check('FW-FX-044 the active preset is marked', presets.filter((p) => p.active).map((p) => p.name).join() === 'ocean', JSON.stringify(presets));
r = await putEffect({ period: 31000 });
check('FW-FX-043 a following PUT clears preset', r.status === 200 && r.json?.preset === null && r.json.period === 31000, r.text);
r = await req('GET', '/api/state');
check('FW-FX-043 FW-FX-069 state says preset null', r.json?.preset === null, r.text);
r = await req('POST', '/api/preset?name=OCEAN');
check('FW-FX-040 names compare without case', r.status === 200 && r.json?.preset === 'ocean', r.text);

r = await send('PUT', '/api/preset?name=mine', { pattern: 'gradient', palette: ['#112233', '#445566'], period: 0 });
check('FW-FX-066 PUT saves a preset and answers with it', r.status === 200 && r.json?.pattern === 'gradient' && r.json.palette[1] === '#445566' && r.json.period === 0, `${r.status} ${r.text}`);
presets = await listPresets();
check('FW-FX-044 FW-FX-063 it is listed with builtin:false', presets.some((p) => p.name === 'mine' && p.builtin === false && p.pattern === 'gradient'), JSON.stringify(presets));
r = await req('GET', '/api/preset?name=Mine');
check('FW-FX-064 FW-FX-045 FW-FX-040 it reads back as plain JSON, any case', r.status === 200 && r.json?.palette?.[0] === '#112233', r.text);
r = await send('PUT', '/api/preset?name=mine', { pattern: 'solid', palette: ['#778899'] });
check('FW-FX-042 saving over a name replaces it', r.status === 200 && (await req('GET', '/api/preset?name=mine')).json?.pattern === 'solid' && (await listPresets()).filter((p) => p.name === 'mine').length === 1, r.text);
r = await send('PUT', '/api/preset?name=mine', { palette: ['#778899'] });
check('FW-FX-001 FW-FX-066 a preset needs a pattern', r.status === 400 && /pattern/.test(r.json?.error ?? ''), r.text);
r = await send('PUT', '/api/preset?name=ocean', { pattern: 'solid' });
check('FW-FX-040 FW-FX-066 a built-in name is 403', r.status === 403, r.status);
for (const bad of ['a%2Fb', '', 'x'.repeat(25), 'a%22b']) {
  r = await send('PUT', `/api/preset?name=${bad}`, { pattern: 'solid' });
  check(`FW-FX-040 FW-FX-066 the name "${bad.slice(0, 8)}" is 400`, r.status === 400 && /name/.test(r.json?.error ?? ''), `${r.status} ${r.text}`);
}
r = await req('POST', '/api/preset?name=mine');
check('FW-FX-065 a user preset applies too', r.status === 200 && r.json?.preset === 'mine' && r.json?.effect?.pattern === 'solid', r.text);
r = await req('DELETE', '/api/preset?name=mine');
check('FW-FX-067 DELETE of a user preset', r.status === 200 && r.json?.ok === true, r.text);
r = await req('GET', '/api/state');
check('FW-FX-067 deleting the active preset leaves its effect, without the name', r.json?.preset === null && r.json?.effect?.pattern === 'solid', r.text);
r = await req('DELETE', '/api/preset?name=mine');
check('FW-FX-067 DELETE of an unknown preset is 404', r.status === 404, r.status);
r = await req('DELETE', '/api/preset?name=ocean');
check('FW-FX-067 DELETE of a built-in is 403', r.status === 403, r.status);

// FW-FX-042: 16 full-size presets fit the 64 KB file system, the 17th is 507.
const large = (i) => ({
  pattern: 'gradient',
  palette: Array.from({ length: 8 }, (_, k) => ({ at: k * 12, color: `#${String(i).padStart(2, '0')}${k}0ff` })),
  interpolate: 'rgb',
  direction: 'center',
  period: 600000,
  phase: 100,
  easing: 'ease-in-out',
  pulse: { min: 100, max: 200, period: 600000, easing: 'ease-in-out' },
});
const existing = (await listPresets()).filter((p) => !p.builtin).length;
const added = [];
let filled = true;
for (let i = existing; i < 16; i++) {
  const name = `api-test-${i} ${'x'.repeat(10)}`;
  r = await send('PUT', `/api/preset?name=${encodeURIComponent(name)}`, large(i));
  added.push(name);
  filled = filled && r.status === 200;
  if (!filled) console.log(`  preset ${i}: ${r.status} ${r.text}`);
}
check('FW-FX-042 16 presets of full size fit', filled, `${added.length} added`);
r = await send('PUT', '/api/preset?name=api-test-17th', { pattern: 'solid' });
check('FW-FX-042 FW-FX-066 saving a 17th is 507', r.status === 507, `${r.status} ${r.text}`);
r = await send('PUT', `/api/preset?name=${encodeURIComponent(added[0] ?? 'api-test-x')}`, large(99));
check('FW-FX-042 saving over one is fine when full', r.status === 200, `${r.status} ${r.text}`);
r = await req('GET', '/api/presets');
check('FW-BUD-005 the longest list fits a response', r.status === 200 && r.text.length <= 3072 && r.json.filter((p) => !p.builtin).length === 16, `${r.text.length} bytes`);
r = await req('POST', `/api/preset?name=${encodeURIComponent(added.at(-1) ?? 'api-test-x')}`);
const state = r.json;
r = await req('GET', '/api/effect');
check('FW-FX-065 a full-size preset applies', state?.effect?.pattern === 'gradient' && r.json?.palette?.length === 8 && r.json.pulse?.max === 200, r.text);
for (const name of added) await req('DELETE', `/api/preset?name=${encodeURIComponent(name)}`);
check('FW-FX-067 the test presets are gone', (await listPresets()).filter((p) => !p.builtin).length === existing, '');

// FW-FX-066: an empty body saves the active effect.
await req('POST', '/api/preset?name=lava');
r = await req('PUT', '/api/preset?name=mine');
check('FW-FX-066 an empty body saves the active effect', r.status === 200 && r.json?.pattern === 'wave' && r.json.period === 20000 && r.json.drift === 75000, `${r.status} ${r.text}`);
r = await req('GET', '/api/state');
check('FW-FX-043 saving leaves the applied preset alone', r.json?.preset === 'lava', r.text);
await req('DELETE', '/api/preset?name=mine');

// FW-FX-068: display options.
r = await req('GET', '/api/display');
check('FW-FX-068 GET /api/display', r.status === 200 && 'leadingZero' in r.json && 'seconds' in r.json && 'transition' in r.json && 'fadeMs' in r.json && [12, 24].includes(r.json.hours), r.text);
r = await req('POST', '/api/display?leadingZero=0');
check('FW-FX-031 FW-FX-068 leadingZero round trip', r.status === 200 && r.json?.leadingZero === false, r.text);
r = await req('POST', '/api/display?seconds=0');
check('FW-FX-032 FW-FX-068 seconds round trip', r.status === 200 && r.json?.seconds === false && r.json.leadingZero === false, r.text);
r = await req('POST', '/api/display?transition=crossfade&fadeMs=300');
check('FW-FX-033 FW-FX-068 transition and fadeMs round trip', r.status === 200 && r.json?.transition === 'crossfade' && r.json.fadeMs === 300, r.text);
r = await req('GET', '/api/state');
check('FW-FX-069 FW-API-041 state carries the display options', r.json?.display?.leadingZero === false && r.json.display.seconds === false && r.json.display.transition === 'crossfade' && [12, 24].includes(r.json.display.hours), r.text);
for (const bad of ['fadeMs=20', 'fadeMs=1001', 'fadeMs=x', 'transition=scramble', 'leadingZero=2', 'seconds=yes', 'night=off', 'tubeBrightness=1,2,3,4,5,6']) {
  r = await req('POST', `/api/display?${bad}`);
  check(`FW-FX-068 FW-API-002 ${bad} is 400`, r.status === 400 && typeof r.json?.error === 'string', `${r.status} ${r.text}`);
}
r = await req('POST', '/api/hours?format=24');
check('FW-FX-030 /api/hours and /api/display agree', r.json?.display?.hours === 24 && (await req('GET', '/api/display')).json?.hours === 24, r.text);
await req('POST', `/api/hours?format=${before.is24h ? 24 : 12}`);
await restoreDisplay();

// FW-FX-069, FW-FX-070, FW-FX-075, FW-DSP-025: what the tubes show, from the emulator's log.
r = await req('POST', '/api/preset?name=rainbow-wave');
check('OQ-22 rainbow-wave applies', r.json?.preset === 'rainbow-wave' && r.json?.effect?.pattern === 'wave', r.text);
if (emulatorLog()) {
  await sleep(1500);
  const mark = emulatorLog().length;
  await req('POST', '/api/attention?h=120&s=100&ms=2500');
  await sleep(1500);
  const during = ledLines(emulatorLog().slice(mark));
  check('FW-FX-070 FW-ATT-002 a flash during rainbow-wave shows on every tube', during.some((l) => l.tubes === Array(6).fill('#00ff00').join(',')), JSON.stringify(during.at(-1)));
  await sleep(3500);  // the flash ended 2500 ms in; 2.5 s of log after that
  const afterMark = emulatorLog().slice(mark);
  const after = ledLines(afterMark).filter((l) => l.tubes !== Array(6).fill('#00ff00').join(','));
  check('FW-FX-070 the effect is back after the flash, and moving', after.length >= 2 && after.at(-1).tubes !== after.at(-2).tubes && new Set(after.at(-1).tubes.split(',')).size > 1, JSON.stringify(after.slice(-2)));

  // FW-DSP-025: the digits only ever follow the clock, whatever the colors do.
  const seconds = (d) => Number(d.slice(2, 4)) * 60 + Number(d.slice(4, 6));
  const shown = ledLines(afterMark).filter((l) => l.lit).map((l) => l.digits);
  const steps = shown.slice(1).map((d, i) => (seconds(d) - seconds(shown[i]) + 3600) % 3600);
  check('FW-DSP-025 digits step with the clock through colors and a flash', shown.length >= 5 && steps.every((s) => s <= 2), JSON.stringify(shown));
  // FW-FX-075: about one line a second, not one per 50 ms frame.
  const lines = ledLines(afterMark).length;
  check('FW-FX-075 the [leds] log is rate-limited', lines >= 5 && lines <= 22, `${lines} lines in 5 s`);

  // FW-FX-032: hidden seconds stay colored.
  await req('POST', '/api/display?seconds=0');
  await sleep(1200);
  const noSeconds = ledLines(emulatorLog()).at(-1);
  check('FW-FX-032 seconds off blanks tubes 5 and 6 and the colors keep flowing', /^\d\d\d\d--$/.test(noSeconds.digits) && !noSeconds.tubes.includes('#000000'), JSON.stringify(noSeconds));
  await req('POST', '/api/attention?h=0&s=100&ms=400');
  await sleep(150);
  const flashed = ledLines(emulatorLog()).at(-1);
  check('FW-ATT-002 FW-FX-032 a flash still shows on every tube', /^\d{6}$/.test(flashed.digits) && flashed.tubes.startsWith('#ff0000'), JSON.stringify(flashed));
  await sleep(500);
  await restoreDisplay();

  // FW-FX-051: a burst of changes is one write, 2 s after the last; a flash is none.
  await sleep(2500);
  const saves = (from) => (emulatorLog().slice(from).match(/\[fx\] \/effect\.json saved/g) ?? []).length;
  let saveMark = emulatorLog().length;
  for (const period of [3000, 3100, 3200]) {
    await putEffect({ period });
    await sleep(500);
  }
  check('FW-FX-051 nothing is written while changes keep coming', saves(saveMark) === 0, `${saves(saveMark)} writes`);
  await sleep(2200);
  check('FW-FX-051 one write 2 s after the last change', saves(saveMark) === 1, `${saves(saveMark)} writes`);
  saveMark = emulatorLog().length;
  await req('POST', '/api/attention?h=0&s=100&ms=100');
  await sleep(2500);
  check('FW-FX-051 FW-SET-003 an attention flash writes nothing', saves(saveMark) === 0, `${saves(saveMark)} writes`);

  // VER-021, FW-FX-072: each pattern's slowest frame. A change of effect logs the one before.
  for (const pattern of ['solid', 'tubes', 'wave', 'gradient', 'breathe', 'solid']) {
    await putEffect({ pattern, palette: 'rainbow', pulse: { min: 10, max: 200, period: 1000 } });
    await sleep(400);
  }
  const maxes = [...emulatorLog().matchAll(/\[fx\] frame max=(\d+) pattern=(\w+)/g)].slice(-5).map((m) => ({ us: Number(m[1]), pattern: m[2] }));
  check('VER-021 FW-FX-072 no pattern takes over 5 ms a frame', maxes.length === 5 && maxes.every((m) => m.us <= 5000), JSON.stringify(maxes));
  console.log(`  frame max: ${maxes.map((m) => `${m.pattern} ${m.us} us`).join(', ')}`);
} else {
  console.log('SKIP VER-020 VER-021 log checks: no emulator log');
}

await restoreEffect();
r = await req('GET', '/api/effect');
const { preset: _p, ...restored } = r.json ?? {};
const { preset: _b, ...wanted } = beforeEffect;
check('VER-004 effect and display restored', JSON.stringify(restored) === JSON.stringify(wanted) && (await req('GET', '/api/display')).text === JSON.stringify(beforeDisplay), `${r.text} vs ${JSON.stringify(beforeEffect)}`);

// FW-SEC-014, VER-014: after a good token, five bad ones are answered at once, the sixth after 1 s,
// and a good token still works straight away (and clears the count).
await req('GET', '/api/state');
const quick = [];
for (let i = 0; i < 5; i++) quick.push((await timed('GET', '/api/state', { Authorization: 'Bearer wrong' })).ms);
check('FW-SEC-014 first five bad tokens answer at once', quick.every((ms) => ms < 200), quick.map(Math.round).join(','));
const slow = await timed('GET', '/api/state', { Authorization: 'Bearer wrong' });
check('FW-SEC-014 sixth bad token waits 1 s', slow.status === 401 && slow.ms >= 1000, `${slow.status} ${Math.round(slow.ms)} ms`);
const good = await timed('GET', '/api/state');
check('FW-SEC-014 good token still answers at once', good.status === 200 && good.ms < 200, `${good.status} ${Math.round(good.ms)} ms`);
const after = await timed('GET', '/api/state', { Authorization: 'Bearer wrong' });
check('FW-SEC-014 good token clears the count', after.status === 401 && after.ms < 200, `${after.status} ${Math.round(after.ms)} ms`);
await req('GET', '/api/state');

// FW-FX-050, VER-020: what has to be there after a reboot.
const persisted = { pattern: 'gradient', palette: ['#123456', '#654321'], period: 7777 };
await send('PUT', '/api/preset?name=api-test-kept', { pattern: 'solid', palette: ['#abcdef'] });
await req('POST', '/api/display?seconds=0&fadeMs=444');
const kept = (await putEffect(persisted)).json;

// FW-API-018 (VER-015): the clock reboots here (the emulator exits).
r = await req('POST', '/api/restart', {});
check('FW-API-018 restart without token is 401', r.status === 401, r.status);
r = await req('POST', '/api/restart');
check('FW-API-018 restart answers {ok:true}', r.status === 200 && r.json?.ok === true, r.text);

const backUp = async (seconds) => {
  for (let i = 0; i < seconds * 2; i++) {
    try {
      if ((await req('GET', '/health', {})).text === 'ok') return true;
    } catch {}
    await sleep(500);
  }
  return false;
};
await sleep(1000);
if (local) {
  // The emulator exits on a restart, so start it again, without rebuilding: its files are all that carry over.
  const [cmd, args] = inWsl('env', 'EMULATE_SKIP_BUILD=1', 'sh', 'firmware/scripts/emulate.sh');
  spawn(cmd, args, { cwd: repo, stdio: 'ignore', detached: true }).unref();
}
if (await backUp(60)) {
  r = await req('GET', '/api/effect');
  const { preset: _k, ...effectAfter } = r.json ?? {};
  const { preset: _k2, ...effectKept } = kept ?? {};
  check('FW-FX-050 FW-FX-051 the active effect survives a restart', JSON.stringify(effectAfter) === JSON.stringify(effectKept) && r.json?.preset === null, r.text);
  r = await req('GET', '/api/preset?name=api-test-kept');
  check('FW-FX-050 FW-FX-042 user presets survive a restart', r.status === 200 && r.json?.palette?.[0] === '#abcdef', r.text);
  r = await req('GET', '/api/display');
  check('FW-FX-050 display options survive a restart', r.json?.seconds === false && r.json.fadeMs === 444, r.text);
  const bootLog = emulatorLog();
  if (bootLog) {
    const at = (text) => bootLog.indexOf(text);
    check('FW-FX-053 boot order: LittleFS, first frame, Wi-Fi', at('[settings]') <= at('[fx] effect') && at('[fx] effect') >= 0 && at('[fx] effect') < at('[leds]') && at('[leds]') < at('[wifi] joining'), bootLog.slice(0, 400));
    check('FW-FX-052 valid files load without a fallback', !bootLog.includes('invalid, using defaults'), bootLog.slice(0, 400));
  }
  await req('DELETE', '/api/preset?name=api-test-kept');
  await restoreDisplay();
  await restoreEffect();
  r = await req('GET', '/api/state');
  check('VER-004 settings restored after the restart', r.json?.preset === beforeEffect.preset && r.json?.effect?.pattern === beforeEffect.pattern && JSON.stringify(r.json?.tubes) === JSON.stringify(before.tubes), r.text);
  // Saves what was just restored, and leaves the emulator stopped as this test always has.
  await req('POST', '/api/restart');
} else {
  check('FW-FX-050 the clock came back after the restart', false, 'no answer in 60 s');
}

console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
