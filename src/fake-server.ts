// Test helper: a local HTTP server standing in for a clock, so host tests need no hardware.
import http from 'node:http';
import type { AddressInfo } from 'node:net';

export interface SeenRequest {
  method: string;
  path: string;
  query: URLSearchParams;
  authorization: string | undefined;
  contentType: string | undefined;
  body: string;
}

export interface FakeReply {
  status?: number;
  body?: unknown;
  headers?: Record<string, string>;
}

export interface FakeServer {
  /** `127.0.0.1:<port>` */
  host: string;
  requests: SeenRequest[];
  close(): Promise<void>;
}

/** Starts a server on a free port. `handle` answers each request; strings are sent as-is, anything else as JSON. */
export async function startFakeServer(handle: (req: SeenRequest) => FakeReply): Promise<FakeServer> {
  const requests: SeenRequest[] = [];
  const server = http.createServer((req, res) => {
    const chunks: Buffer[] = [];
    req.on('data', (chunk: Buffer) => chunks.push(chunk));
    req.on('end', () => {
      const url = new URL(req.url ?? '/', 'http://fake');
      const seen = {
        method: req.method ?? 'GET',
        path: url.pathname,
        query: url.searchParams,
        authorization: req.headers.authorization,
        contentType: req.headers['content-type'],
        body: Buffer.concat(chunks).toString(),
      };
      requests.push(seen);
      const { status = 200, body = '', headers = {} } = handle(seen);
      const text = typeof body === 'string' ? body : JSON.stringify(body);
      res.writeHead(status, { 'Content-Type': typeof body === 'string' ? 'text/plain' : 'application/json', ...headers });
      res.end(text);
    });
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address() as AddressInfo;
  return {
    host: `127.0.0.1:${port}`,
    requests,
    close: () => new Promise((resolve) => server.close(() => resolve())),
  };
}

/** An effect as `GET /api/effect` returns it (FW-FX-060). */
export function sampleEffect(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    pattern: 'wave',
    palette: 'rainbow',
    interpolate: 'hsv',
    direction: 'ltr',
    period: 18000,
    phase: 8,
    drift: 0,
    easing: 'linear',
    pulse: null,
    preset: 'rainbow-wave',
    ...overrides,
  };
}

/** Display options as `GET /api/display` returns them (FW-FX-068). */
export function sampleDisplay(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return { hours: 12, leadingZero: true, seconds: true, transition: 'cut', fadeMs: 200, ...overrides };
}

/** A state object as the new firmware returns it (FW-API-030). */
export function sampleState(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    version: '0.2.0-3-gabc1234',
    time: '13:05:09',
    tz: 'EST5EDT,M3.2.0,M11.1.0',
    is24h: false,
    brightness: 99,
    attention: false,
    tubes: Array.from({ length: 6 }, () => ({ h: 241, s: 100 })),
    ...overrides,
  };
}
