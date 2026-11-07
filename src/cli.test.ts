// Runs the built CLI against a fake clock, as a user would.
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { after, before, beforeEach, test } from 'node:test';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { sampleDisplay, sampleEffect, sampleState, startFakeServer, type FakeReply, type FakeServer, type SeenRequest } from './fake-server.js';

const CLI = fileURLToPath(new URL('./cli.js', import.meta.url));
const TOKEN = 'cli-test-token-fedcba9876543210';

let mode: 'tubeclock' | 'stock' = 'tubeclock';
let server: FakeServer;

const STOCK_CONFIG = { h1: 10, h2: 20, h3: 30, h4: 40, h5: 50, h6: 60, s1: 255, s2: 255, s3: 255, s4: 255, s5: 255, s6: 255, light: 204, mode: 1, outcarry: 0, is24h: 0, tz: -5 };

function answer(req: SeenRequest): FakeReply {
  if (mode === 'stock') {
    if (req.path === '/version') return { body: { ver: 2110 } };
    if (req.path === '/config') return { body: STOCK_CONFIG };
    if (req.path === '/tubecolor' || req.path === '/mode') return { body: 'OK' };
    return { status: 307, headers: { Location: '/admin' } };
  }
  if (req.path === '/health') return { body: 'ok' };
  if (req.authorization !== `Bearer ${TOKEN}`) return { status: 401, body: { error: 'unauthorized' } };
  if (req.path === '/api/restart') return { body: { ok: true } };
  if (req.path === '/api/effect') {
    if (req.body.includes('"period":99')) return { status: 400, body: { error: 'period: need 0 or 100-600000' } };
    return { body: sampleEffect(req.body ? { ...(JSON.parse(req.body) as object), preset: null } : {}) };
  }
  if (req.path === '/api/presets') return { body: [{ name: 'rainbow-wave', builtin: true, pattern: 'wave', active: true }, { name: 'mine', builtin: false, pattern: 'solid' }] };
  if (req.path === '/api/preset') {
    if (req.method === 'POST') return { body: sampleState({ preset: req.query.get('name'), effect: { pattern: 'wave' } }) };
    if (req.method === 'DELETE') return { body: { ok: true } };
    return { body: sampleEffect({ preset: undefined, ...(req.body ? (JSON.parse(req.body) as object) : {}) }) };
  }
  if (req.path === '/api/display') return { body: sampleDisplay(req.query.get('seconds') === '0' ? { seconds: false } : {}) };
  const tz = req.query.get('tz');
  return { body: sampleState({ ...stateExtra, ...(tz ? { tz } : req.path === '/api/attention' ? { attention: req.method === 'POST' } : {}) }) };
}

let stateExtra: Record<string, unknown> = {};

async function clocteckStatus(extra: Record<string, unknown>): Promise<string> {
  stateExtra = extra;
  try {
    const { code, stdout } = await run(['status']);
    assert.equal(code, 0);
    return stdout;
  } finally {
    stateExtra = {};
  }
}

before(async () => {
  server = await startFakeServer(answer);
});
after(() => server.close());
beforeEach(() => {
  mode = 'tubeclock';
  server.requests.length = 0;
});

interface Run {
  code: number;
  stdout: string;
  stderr: string;
}

function run(args: string[], env: Record<string, string> = { CLOCTECK_TOKEN: TOKEN }): Promise<Run> {
  const base = { ...process.env };
  for (const key of ['CLOCTECK_HOST', 'CLOCTECK_TOKEN', 'FW_API_TOKEN']) delete base[key];
  return new Promise((resolve) => {
    execFile(process.execPath, [CLI, '--host', server.host, ...args], { env: { ...base, ...env } }, (err, stdout, stderr) => {
      resolve({ code: err ? Number(err.code ?? 1) : 0, stdout, stderr });
    });
  });
}

const apiCalls = () => server.requests.filter((r) => r.path.startsWith('/api/')).map((r) => `${r.method} ${r.path}?${r.query}`);

test('HOST-CLI-010 status on the new firmware: --json prints the state unchanged', async () => {
  const { code, stdout } = await run(['status', '--json']);
  assert.equal(code, 0);
  assert.deepEqual(JSON.parse(stdout), sampleState());
});

test('HOST-CLI-010 status lists the saved tube colors only under the tubes pattern', async () => {
  const wave = await clocteckStatus({ effect: { pattern: 'wave' }, preset: 'rainbow-wave' });
  assert.match(wave, /Effect +wave \(preset rainbow-wave\)/);
  assert.doesNotMatch(wave, /Tubes|#0400ff/);
  const tubes = await clocteckStatus({ effect: { pattern: 'tubes' }, preset: null });
  assert.match(tubes, /Tubes\n  1  #0400ff/);
  assert.match(await clocteckStatus({}), /Tubes\n  1  #0400ff/);  // firmware from before effects
});

test('HOST-CLI-011 color all is one /api/tube request; value is ignored', async () => {
  const { code } = await run(['color', 'all', '200,50,10']);
  assert.equal(code, 0);
  assert.deepEqual(apiCalls(), ['POST /api/tube?t=all&h=200&s=50']);
});

test('HOST-CLI-012 brightness 50 sends v=128', async () => {
  await run(['brightness', '50']);
  assert.deepEqual(apiCalls(), ['POST /api/brightness?v=128']);
});

test('HOST-CLI-015 HOST-CLI-017 hours and tz', async () => {
  await run(['hours', '24']);
  await run(['tz', 'EST5EDT,M3.2.0,M11.1.0']);
  assert.deepEqual(apiCalls(), ['POST /api/hours?format=24', 'POST /api/tz?tz=EST5EDT%2CM3.2.0%2CM11.1.0']);
});

test('HOST-CLI-016 HOST-CLI-004 timezone -5 sends <-05>5 and hints at tz', async () => {
  const { code, stdout } = await run(['timezone', '-5']);
  assert.equal(code, 0);
  assert.equal(server.requests.at(-1)?.query.get('tz'), '<-05>5');
  assert.match(stdout, /clocteck tz /);
});

test('HOST-CLI-022 HOST-CLI-023 attention and attention cancel on the new firmware return at once', async () => {
  const flash = await run(['attention', 'orange', '--seconds', '3', '--blink', '--beep']);
  assert.equal(flash.code, 0);
  const cancel = await run(['attention', 'cancel']);
  assert.equal(cancel.code, 0);
  assert.deepEqual(apiCalls(), ['POST /api/attention?h=25&s=100&ms=3000&blink=1&beep=1', 'DELETE /api/attention?']);
});

test('HOST-CLI-024 HOST-CLI-025 detect and restart on the new firmware', async () => {
  const detect = await run(['detect']);
  assert.equal(detect.stdout.trim(), 'tubeclock 0.2.0-3-gabc1234');
  const restart = await run(['restart', '--json']);
  assert.deepEqual(JSON.parse(restart.stdout), { ok: true });
});

test('HOST-CLI-026 preset list, use, show, export and delete', async () => {
  const list = await run(['preset', 'list']);
  assert.equal(list.code, 0);
  assert.match(list.stdout, /\* rainbow-wave .* wave .* built-in/);
  assert.match(list.stdout, /mine .* solid .* saved/);
  const listJson = await run(['preset', 'list', '--json']);
  assert.equal(JSON.parse(listJson.stdout)[1].name, 'mine');

  const use = await run(['preset', 'use', 'ocean']);
  assert.match(use.stdout, /Showing ocean/);
  const useJson = await run(['preset', 'use', 'ocean', '--json']);
  assert.equal(JSON.parse(useJson.stdout).preset, 'ocean');  // the state, as for any write

  for (const sub of ['show', 'export']) {
    const out = await run(['preset', sub, 'my one']);
    assert.equal(JSON.parse(out.stdout).pattern, 'wave', sub);  // FW-FX-045 plain JSON without --json
  }
  assert.ok(server.requests.some((r) => r.query.get('name') === 'my one'));

  const del = await run(['preset', 'delete', 'mine']);
  assert.equal(del.code, 0);
  assert.equal(apiCalls().at(-1), 'DELETE /api/preset?name=mine');

  const bad = await run(['preset', 'use']);
  assert.equal(bad.code, 1);
  assert.match(bad.stderr, /usage: preset/);
});

test('HOST-CLI-026 preset save: the active effect, or --from a file', async () => {
  const active = await run(['preset', 'save', 'mine']);
  assert.equal(active.code, 0);
  assert.equal(server.requests.at(-1)?.method, 'PUT');
  assert.equal(server.requests.at(-1)?.body, '');

  const dir = mkdtempSync(join(tmpdir(), 'clocteck-cli-'));
  try {
    const file = join(dir, 'p.json');
    writeFileSync(file, '{"pattern":"solid","palette":["#010203"]}');
    const fromFile = await run(['preset', 'save', 'mine', '--from', file]);
    assert.equal(fromFile.code, 0);
    assert.equal(server.requests.at(-1)?.body, '{"pattern":"solid","palette":["#010203"]}');

    writeFileSync(file, 'nope');
    const notJson = await run(['preset', 'save', 'mine', '--from', file]);
    assert.equal(notJson.code, 1);
    assert.match(notJson.stderr, /is not JSON/);
    const missing = await run(['preset', 'save', 'mine', '--from', join(dir, 'absent.json')]);
    assert.equal(missing.code, 1);
    assert.match(missing.stderr, /could not read/);
  } finally {
    rmSync(dir, { recursive: true });
  }
});

test('HOST-CLI-027 effect, effect set and effect json', async () => {
  const shown = await run(['effect']);
  assert.match(shown.stdout, /Pattern {4}wave \(preset rainbow-wave\)/);
  const shownJson = await run(['effect', '--json']);
  assert.deepEqual(JSON.parse(shownJson.stdout), sampleEffect());

  const set = await run(['effect', 'set', 'wave', '--palette', 'red,241,100,100', '--direction', 'rtl', '--period', '5000', '--phase', '20', '--easing', 'sine', '--pulse', '1,2,3000']);
  assert.equal(set.code, 0);
  assert.deepEqual(JSON.parse(server.requests.at(-1)?.body ?? ''), {
    pattern: 'wave',
    palette: ['red', '241,100,100'],
    direction: 'rtl',
    period: 5000,
    phase: 20,
    easing: 'sine',
    pulse: { min: 1, max: 2, period: 3000 },
  });
  await run(['effect', 'set', 'wave', '--speed', '100']);
  assert.equal(JSON.parse(server.requests.at(-1)?.body ?? '').period, 600);

  const both = await run(['effect', 'set', 'wave', '--speed', '100', '--period', '5000']);
  assert.equal(both.code, 1);
  assert.match(both.stderr, /use one/);
  const noPattern = await run(['effect', 'set']);
  assert.equal(noPattern.code, 1);
  assert.match(noPattern.stderr, /usage: effect/);

  // The firmware's 400 is shown verbatim.
  const refused = await run(['effect', 'set', 'wave', '--period', '99']);
  assert.equal(refused.code, 1);
  assert.match(refused.stderr, /period: need 0 or 100-600000/);

  const dir = mkdtempSync(join(tmpdir(), 'clocteck-cli-'));
  try {
    const file = join(dir, 'e.json');
    writeFileSync(file, '{"pattern":"breathe"}');
    const json = await run(['effect', 'json', file]);
    assert.equal(json.code, 0);
    assert.equal(server.requests.at(-1)?.body, '{"pattern":"breathe"}');
  } finally {
    rmSync(dir, { recursive: true });
  }
});

test('HOST-CLI-028 display prints with no flags and changes with them', async () => {
  const shown = await run(['display']);
  assert.match(shown.stdout, /Seconds {6}on/);
  assert.deepEqual(apiCalls(), ['GET /api/display?']);

  server.requests.length = 0;
  const set = await run(['display', '--leading-zero', 'off', '--seconds', 'off', '--transition', 'crossfade', '--fade', '300', '--json']);
  assert.equal(set.code, 0);
  assert.equal(JSON.parse(set.stdout).seconds, false);
  assert.deepEqual(apiCalls(), ['POST /api/display?leadingZero=0&seconds=0&transition=crossfade&fadeMs=300']);

  server.requests.length = 0;
  await run(['display', '--hours', '24']);
  assert.deepEqual(apiCalls(), ['POST /api/hours?format=24', 'GET /api/display?']);

  const bad = await run(['display', '--seconds', 'maybe']);
  assert.equal(bad.code, 1);
  assert.match(bad.stderr, /--seconds must be on or off/);
});

test('HOST-CLI-007 HOST-CLI-013 HOST-CLI-026..028 the effects commands refuse the stock firmware; mode points at them', async () => {
  const onNew = await run(['mode', 'rainbow']);
  assert.equal(onNew.code, 2);
  assert.match(onNew.stderr, /preset use.*effect set/);
  mode = 'stock';
  for (const cmd of [['preset', 'list'], ['effect'], ['display']]) {
    const r = await run(cmd);
    assert.equal(r.code, 2, cmd[0]);
    assert.match(r.stderr, new RegExp(`^clocteck: "${cmd[0]}" needs the new firmware`));
  }
});

test('HOST-CLI-024 detect on stock prints the version', async () => {
  mode = 'stock';
  const { code, stdout } = await run(['detect']);
  assert.equal(code, 0);
  assert.equal(stdout.trim(), 'stock 2110');
});

test('HOST-CLI-022 attention on stock flashes, waits, then restores the snapshot', async () => {
  mode = 'stock';
  const { code } = await run(['attention', 'red', '--seconds', '1']);
  assert.equal(code, 0);
  const tubecolor = server.requests.filter((r) => r.path === '/tubecolor').map((r) => `${r.query.get('t')}:${r.query.get('h')},${r.query.get('s')},${r.query.get('v')}`);
  // Six tubes red at full value, then each tube back to its own hue at the saved brightness (204/2.55 = 80)
  assert.deepEqual(tubecolor, [
    ...[1, 2, 3, 4, 5, 6].map((t) => `${t}:0,100,100`),
    ...[1, 2, 3, 4, 5, 6].map((t) => `${t}:${t * 10},100,80`),
  ]);
});

test('HOST-CLI-023 attention cancel on stock points at Ctrl+C', async () => {
  mode = 'stock';
  const { code, stderr } = await run(['attention', 'cancel']);
  assert.equal(code, 2);
  assert.match(stderr, /^clocteck: "attention cancel" needs the new firmware\n.*Ctrl\+C/);
});

test('HOST-CLI-007 a command the firmware lacks exits 2 with the spec message', async () => {
  const onNew = await run(['mode', 'rainbow']);
  assert.equal(onNew.code, 2);
  assert.match(onNew.stderr, /^clocteck: "mode" needs the stock firmware/);
  const update = await run(['firmware', '--update']);
  assert.equal(update.code, 2);
  assert.match(update.stderr, /^clocteck: "firmware --update" needs the stock firmware/);
  mode = 'stock';
  const onStock = await run(['tz', 'UTC0']);
  assert.equal(onStock.code, 2);
  assert.match(onStock.stderr, /^clocteck: "tz" needs the new firmware/);
  assert.equal(apiCalls().length, 0);
});

test('HOST-CLI-003 --firmware skips detection', async () => {
  const { code } = await run(['--firmware', 'tubeclock', 'status']);
  assert.equal(code, 0);
  assert.ok(!server.requests.some((r) => r.path === '/health' || r.path === '/version'));
});

test('HOST-CLI-002 --token works and is never echoed; FW_API_TOKEN is the fallback', async () => {
  const withFlag = await run(['--token', TOKEN, 'status'], {});
  assert.equal(withFlag.code, 0);
  assert.ok(!withFlag.stdout.includes(TOKEN) && !withFlag.stderr.includes(TOKEN));
  const fallback = await run(['status'], { FW_API_TOKEN: TOKEN });
  assert.equal(fallback.code, 0);
  const missing = await run(['status'], {});
  assert.equal(missing.code, 1);
  assert.match(missing.stderr, /CLOCTECK_TOKEN/);
  const wrong = await run(['--token', 'wrong-token-value', 'status'], {});
  assert.equal(wrong.code, 1);
  assert.ok(!wrong.stderr.includes('wrong-token-value'));
});

test('HOST-CLI-005 an unreachable clock is a one-line error with exit code 1', async () => {
  const base = { ...process.env, CLOCTECK_TOKEN: TOKEN };
  const result = await new Promise<Run>((resolve) =>
    execFile(process.execPath, [CLI, '--host', '127.0.0.1:1', '--timeout', '2000', 'status'], { env: base }, (err, stdout, stderr) =>
      resolve({ code: err ? Number(err.code ?? 1) : 0, stdout, stderr }),
    ),
  );
  assert.equal(result.code, 1);
  assert.match(result.stderr, /^clocteck: could not tell which firmware/);
});
