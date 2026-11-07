import assert from 'node:assert/strict';
import http from 'node:http';
import type { AddressInfo } from 'node:net';
import { test } from 'node:test';
import { ClocTeckClient } from './client.js';

const SECRET = 'hunter2-not-real';

async function withServer(handler: http.RequestListener, run: (host: string) => Promise<void>) {
  const server = http.createServer(handler);
  await new Promise<void>((ready) => server.listen(0, '127.0.0.1', ready));
  try {
    await run(`127.0.0.1:${(server.address() as AddressInfo).port}`);
  } finally {
    server.closeAllConnections();
    await new Promise((done) => server.close(done));
  }
}

test('an HTTP error does not repeat the Wi-Fi password', async () => {
  await withServer(
    (_req, res) => {
      res.statusCode = 500;
      res.end();
    },
    async (host) => {
      const clock = new ClocTeckClient({ host });
      await assert.rejects(clock.request('/wificonn', { s: 'net', p: SECRET }), (err: Error) => {
        assert.match(err.message, /\/wificonn returned HTTP 500/);
        assert.ok(!err.message.includes(SECRET));
        return true;
      });
    },
  );
});

test('a timeout does not repeat the Wi-Fi password', async () => {
  await withServer(
    () => {
      // never answers
    },
    async (host) => {
      const clock = new ClocTeckClient({ host, timeoutMs: 50 });
      await assert.rejects(clock.request('/wificonn', { s: 'net', p: SECRET }), (err: Error) => {
        assert.match(err.message, /\/wificonn timed out/);
        assert.ok(!err.message.includes(SECRET));
        return true;
      });
    },
  );
});
