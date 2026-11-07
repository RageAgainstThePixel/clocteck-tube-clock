import assert from 'node:assert/strict';
import { after, before, beforeEach, test } from 'node:test';
import { inspect } from 'node:util';
import { sampleDisplay, sampleEffect, sampleState, startFakeServer, type FakeReply, type FakeServer, type SeenRequest } from './fake-server.js';
import { AuthError, HttpError, TUBECLOCK_DEFAULT_HOST, TubeClockClient, ValidationError } from './tubeclock.js';

const TOKEN = 'test-token-0123456789abcdef';
let reply: (req: SeenRequest) => FakeReply = () => ({ body: sampleState() });
let server: FakeServer;

before(async () => {
  server = await startFakeServer((req) => reply(req));
});
after(() => server.close());
beforeEach(() => {
  server.requests.length = 0;
  reply = () => ({ body: sampleState() });
  delete process.env.CLOCTECK_TOKEN;
  delete process.env.FW_API_TOKEN;
});

const client = () => new TubeClockClient({ host: server.host, token: TOKEN });
const last = () => server.requests.at(-1)!;

test('HOST-LIB-030 HOST-LIB-003 token comes from the argument, then CLOCTECK_TOKEN, then FW_API_TOKEN', async () => {
  process.env.FW_API_TOKEN = 'from-fw';
  await new TubeClockClient({ host: server.host }).getState();
  assert.equal(last().authorization, 'Bearer from-fw');
  process.env.CLOCTECK_TOKEN = 'from-clocteck';
  await new TubeClockClient({ host: server.host }).getState();
  assert.equal(last().authorization, 'Bearer from-clocteck');
  await new TubeClockClient({ host: server.host, token: 'from-arg' }).getState();
  assert.equal(last().authorization, 'Bearer from-arg');
});

test('HOST-LIB-030 a missing token throws in the constructor; host defaults to tubeclock.local', () => {
  assert.throws(() => new TubeClockClient(), /CLOCTECK_TOKEN/);
  process.env.CLOCTECK_TOKEN = 'x';
  const c = new TubeClockClient();
  assert.equal(c.host, TUBECLOCK_DEFAULT_HOST);
  assert.equal(c.timeoutMs, 10_000);
});

test('HOST-LIB-031 every call sends the bearer token', async () => {
  const c = client();
  await c.getState();
  await c.setBrightness(10);
  await c.cancelAttention();
  assert.equal(server.requests.length, 3);
  for (const req of server.requests) assert.equal(req.authorization, `Bearer ${TOKEN}`);
});

test('HOST-LIB-031 401 is AuthError, 400 is ValidationError with the firmware message, others HttpError', async () => {
  reply = () => ({ status: 401, body: { error: 'unauthorized' } });
  await assert.rejects(client().getState(), (err) => err instanceof AuthError && err.status === 401);
  reply = () => ({ status: 400, body: { error: 'need v=0-255' } });
  await assert.rejects(client().setBrightness(999), (err) => err instanceof ValidationError && /need v=0-255/.test(err.message));
  reply = () => ({ status: 404, body: { error: 'not found' } });
  await assert.rejects(client().restart(), (err) => err instanceof HttpError && !(err instanceof AuthError) && err.status === 404);
});

test('HOST-LIB-032 HOST-LIB-034 methods map to the routes and resolve with the state', async () => {
  const c = client();
  const cases: [() => Promise<unknown>, string, string, Record<string, string>][] = [
    [() => c.getState(), 'GET', '/api/state', {}],
    [() => c.setTube(3, 120.4, 50), 'POST', '/api/tube', { t: '3', h: '120', s: '50' }],
    [() => c.setTube('all', 0, 100), 'POST', '/api/tube', { t: 'all', h: '0', s: '100' }],
    [() => c.setBrightness(128), 'POST', '/api/brightness', { v: '128' }],
    [() => c.setHourFormat(24), 'POST', '/api/hours', { format: '24' }],
    [() => c.setTimeZone('EST5EDT,M3.2.0,M11.1.0'), 'POST', '/api/tz', { tz: 'EST5EDT,M3.2.0,M11.1.0' }],
    [() => c.attention({ h: 30, s: 100 }), 'POST', '/api/attention', { h: '30', s: '100' }],
    [() => c.attention({ h: 30, s: 100 }, { ms: 3000, blink: true, beep: false }), 'POST', '/api/attention', { h: '30', s: '100', ms: '3000', blink: '1', beep: '0' }],
    [() => c.cancelAttention(), 'DELETE', '/api/attention', {}],
  ];
  for (const [call, method, path, query] of cases) {
    const state = await call();
    assert.deepEqual(state, sampleState(), `${method} ${path}`);
    assert.equal(last().method, method);
    assert.equal(last().path, path);
    assert.deepEqual(Object.fromEntries(last().query), query, `${method} ${path} query`);
  }
});

test('HOST-LIB-032 restart resolves with {ok:true}; a bad tube is refused before sending', async () => {
  reply = () => ({ body: { ok: true } });
  assert.deepEqual(await client().restart(), { ok: true });
  assert.equal(last().method, 'POST');
  assert.equal(last().path, '/api/restart');
  const before = server.requests.length;
  assert.throws(() => client().setTube(7, 0, 0), RangeError);
  assert.equal(server.requests.length, before);
});

test('HOST-LIB-032 FW-API-005 the tz rule is sent URL-encoded', async () => {
  await client().setTimeZone('<-05>5');
  assert.equal(last().query.get('tz'), '<-05>5');
  await client().setTimeZone('<+0530>-5:30');
  assert.equal(last().query.get('tz'), '<+0530>-5:30');
});

test('HOST-LIB-033 unknown extra state fields are kept', async () => {
  reply = () => ({ body: sampleState({ timeSource: 'ntp', lastSync: '2026-11-03T12:00:00Z', future: { x: 1 } }) });
  const state = await client().getState();
  assert.equal(state.timeSource, 'ntp');
  assert.deepEqual(state.future, { x: 1 });
  assert.equal(state.tubes.length, 6);
});

test('HOST-LIB-036 effect and preset methods map to the routes', async () => {
  const c = client();
  reply = () => ({ body: sampleEffect() });
  assert.deepEqual(await c.getEffect(), sampleEffect());
  assert.equal(last().method, 'GET');
  assert.equal(last().path, '/api/effect');

  await c.setEffect({ period: 5000 });
  assert.equal(last().method, 'PUT');
  assert.equal(last().path, '/api/effect');
  assert.equal(last().body, '{"period":5000}');
  assert.equal(last().contentType, 'application/json');

  reply = () => ({ body: [{ name: 'ocean', builtin: true, pattern: 'wave' }] });
  assert.equal((await c.listPresets())[0]?.name, 'ocean');
  assert.equal(last().path, '/api/presets');

  reply = () => ({ body: sampleEffect() });
  await c.getPreset('my one');
  assert.equal(last().method, 'GET');
  assert.equal(last().path, '/api/preset');
  assert.equal(last().query.get('name'), 'my one');

  reply = () => ({ body: sampleState({ preset: 'ocean' }) });
  assert.equal((await c.applyPreset('ocean')).preset, 'ocean');
  assert.equal(last().method, 'POST');
  assert.equal(last().query.get('name'), 'ocean');

  reply = () => ({ body: sampleEffect() });
  await c.savePreset('mine', { pattern: 'solid' });
  assert.equal(last().method, 'PUT');
  assert.equal(last().query.get('name'), 'mine');
  assert.equal(last().body, '{"pattern":"solid"}');
  await c.savePreset('mine');  // FW-FX-066 no body: the active effect
  assert.equal(last().body, '');
  assert.equal(last().contentType, undefined);

  reply = () => ({ body: { ok: true } });
  assert.deepEqual(await c.deletePreset('mine'), { ok: true });
  assert.equal(last().method, 'DELETE');
  assert.equal(last().query.get('name'), 'mine');
});

test('HOST-LIB-036 the firmware\'s field-level 400 comes through; other errors keep their status and message', async () => {
  reply = () => ({ status: 400, body: { error: 'period: need 0 or 100-600000' } });
  await assert.rejects(client().setEffect({ period: 99 }), (err) => err instanceof ValidationError && /period: need 0 or 100-600000/.test(err.message));
  reply = () => ({ status: 507, body: { error: 'preset storage full (16 presets)' } });
  await assert.rejects(client().savePreset('x'), (err) => err instanceof HttpError && err.status === 507 && /full/.test(err.message));
  reply = () => ({ status: 403, body: { error: 'built-in presets are read-only' } });
  await assert.rejects(client().deletePreset('ocean'), (err) => err instanceof HttpError && err.status === 403);
  reply = () => ({ status: 413, body: { error: 'body too large (limit 1024 bytes)' } });
  await assert.rejects(client().setEffect({}), (err) => err instanceof HttpError && err.status === 413);
});

test('HOST-LIB-036 display options', async () => {
  const c = client();
  reply = () => ({ body: sampleDisplay() });
  assert.deepEqual(await c.getDisplay(), sampleDisplay());
  assert.equal(last().path, '/api/display');

  await c.setDisplay({ leadingZero: false, seconds: true, transition: 'crossfade', fadeMs: 300 });
  assert.equal(last().method, 'POST');
  assert.deepEqual(Object.fromEntries(last().query), { leadingZero: '0', seconds: '1', transition: 'crossfade', fadeMs: '300' });

  await c.setDisplay({ tubeBrightness: [1, 2, 3, 4, 5, 6], night: 'off' });
  assert.deepEqual(Object.fromEntries(last().query), { tubeBrightness: '1,2,3,4,5,6', night: 'off' });

  // FW-FX-030: the hour format has its own route.
  server.requests.length = 0;
  await c.setDisplay({ hours: 24 });
  assert.deepEqual(server.requests.map((r) => `${r.method} ${r.path}`), ['POST /api/hours', 'GET /api/display']);
  server.requests.length = 0;
  await c.setDisplay({ hours: 12, seconds: false });
  assert.deepEqual(server.requests.map((r) => `${r.method} ${r.path}`), ['POST /api/hours', 'POST /api/display']);
});

test('HOST-LIB-033 the state\'s effect fields come through', async () => {
  reply = () => ({ body: sampleState({ preset: 'nixie', effect: { pattern: 'solid' }, display: sampleDisplay() }) });
  const state = await client().getState();
  assert.equal(state.preset, 'nixie');
  assert.equal(state.effect?.pattern, 'solid');
  assert.equal(state.display?.transition, 'cut');
});

test('HOST-LIB-035 the token never appears in errors or in the inspected client', async () => {
  const c = client();
  assert.ok(!inspect(c, { showHidden: true }).includes(TOKEN));
  assert.ok(!JSON.stringify(c).includes(TOKEN));
  const errors: unknown[] = [];
  for (const status of [400, 401, 500]) {
    reply = () => ({ status, body: { error: 'nope' } });
    await c.getState().catch((e: unknown) => errors.push(e));
  }
  reply = () => ({ body: 'not json' });
  await c.getState().catch((e: unknown) => errors.push(e));
  await new TubeClockClient({ host: '127.0.0.1:1', token: TOKEN, timeoutMs: 2000 }).getState().catch((e: unknown) => errors.push(e));
  assert.equal(errors.length, 5);
  for (const err of errors) {
    assert.ok(err instanceof Error);
    assert.ok(!`${err.message}\n${err.stack}\n${inspect(err)}`.includes(TOKEN), err.message);
  }
});
