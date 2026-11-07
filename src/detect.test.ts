import assert from 'node:assert/strict';
import { test } from 'node:test';
import { detectFirmware } from './detect.js';
import { startFakeServer, type FakeReply, type SeenRequest } from './fake-server.js';

// The stock firmware redirects every unknown path to /admin
const stock = (req: SeenRequest): FakeReply =>
  req.path === '/version' ? { body: { ver: 2110 } } : { status: 307, headers: { Location: '/admin' } };

test('HOST-LIB-001 /health answering ok is the new firmware', async () => {
  const server = await startFakeServer((req) => (req.path === '/health' ? { body: 'ok' } : { status: 404, body: { error: 'not found' } }));
  try {
    assert.equal(await detectFirmware(server.host), 'tubeclock');
  } finally {
    await server.close();
  }
});

test('HOST-LIB-001 a 307 on /health and JSON with ver on /version is stock', async () => {
  const server = await startFakeServer(stock);
  try {
    assert.equal(await detectFirmware(server.host), 'stock');
    assert.deepEqual(server.requests.map((r) => r.path), ['/health', '/version']);
  } finally {
    await server.close();
  }
});

test('HOST-LIB-001 redirects everywhere, non-JSON /version or nothing listening is unknown', async () => {
  const redirects = await startFakeServer(() => ({ status: 307, headers: { Location: '/admin' } }));
  const junk = await startFakeServer((req) => (req.path === '/version' ? { body: 'hello' } : { status: 404 }));
  try {
    assert.equal(await detectFirmware(redirects.host), 'unknown');
    assert.equal(await detectFirmware(junk.host), 'unknown');
    assert.equal(await detectFirmware('127.0.0.1:1', 2000), 'unknown');
  } finally {
    await redirects.close();
    await junk.close();
  }
});

test('HOST-LIB-002 detection sends no token and only GETs /health and /version', async () => {
  process.env.CLOCTECK_TOKEN = 'should-not-be-sent';
  process.env.FW_API_TOKEN = 'should-not-be-sent';
  const server = await startFakeServer(stock);
  try {
    await detectFirmware(server.host);
    for (const req of server.requests) {
      assert.equal(req.method, 'GET');
      assert.equal(req.authorization, undefined);
      assert.ok(['/health', '/version'].includes(req.path), req.path);
    }
  } finally {
    delete process.env.CLOCTECK_TOKEN;
    delete process.env.FW_API_TOKEN;
    await server.close();
  }
});
