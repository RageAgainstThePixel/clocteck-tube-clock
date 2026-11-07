import http from 'node:http';

export type FirmwareKind = 'tubeclock' | 'stock' | 'unknown';

/** Splits `host` or `host:port` for node:http, which wants them apart. */
export function splitHost(host: string): { hostname: string; port: number } {
  const url = new URL(`http://${host}`);
  return { hostname: url.hostname.replace(/^\[|\]$/g, ''), port: url.port ? Number(url.port) : 80 };
}

/**
 * An unauthenticated GET that never follows redirects. Uses the lenient parser because the stock
 * firmware's responses trip the strict one. Resolves with undefined on any network error.
 */
function probe(host: string, path: string, timeoutMs: number): Promise<{ status: number; body: string } | undefined> {
  return new Promise((resolve) => {
    const req = http.get({ ...splitHost(host), path, timeout: timeoutMs, insecureHTTPParser: true }, (res) => {
      const chunks: Buffer[] = [];
      res.on('data', (c: Buffer) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode ?? 0, body: Buffer.concat(chunks).toString('utf8') }));
      res.on('error', () => resolve(undefined));
    });
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', () => resolve(undefined));
  });
}

/**
 * Tells which firmware answers at `host` (HOST-LIB-001): `/health` saying `ok` is the new firmware,
 * `/version` answering JSON with `ver` is the stock one, anything else is unknown. Only GETs to those
 * two paths, never the token (HOST-LIB-002).
 */
export async function detectFirmware(host: string, timeoutMs = 5000): Promise<FirmwareKind> {
  const health = await probe(host, '/health', timeoutMs);
  if (!health) return 'unknown'; // nothing answers; don't wait out a second timeout
  if (health.status === 200 && health.body.trim() === 'ok') return 'tubeclock';
  const version = await probe(host, '/version', timeoutMs);
  if (version?.status === 200) {
    try {
      const json = JSON.parse(version.body) as { ver?: unknown };
      if (json.ver !== undefined && json.ver !== null) return 'stock';
    } catch {
      // not JSON: unknown
    }
  }
  return 'unknown';
}
