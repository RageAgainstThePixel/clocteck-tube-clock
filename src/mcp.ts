#!/usr/bin/env node
import http from 'node:http';
import { isIP } from 'node:net';
import os from 'node:os';
import { Readable } from 'node:stream';
import { parseArgs } from 'node:util';
import { createMcpHandler, McpServer } from '@modelcontextprotocol/server';
import { z } from 'zod';
import { AttentionController } from './attention.js';
import { ClocTeckClient, DEFAULT_HOST } from './client.js';
import { hsvToHex, NAMED_COLORS, parseColor } from './color.js';

const USAGE = `Usage: clocteck-mcp [options]

Serves an MCP endpoint (Streamable HTTP) at http://<bind>:<port>/mcp so agents on your
network can flash the tube clock to get your attention.

Options:
  --clock <ip>          Clock address (default: $CLOCTECK_HOST or ${DEFAULT_HOST})
  --port <n>            Port to listen on (default: $CLOCTECK_MCP_PORT or 3939)
  --bind <addr>         Address to bind (default: 0.0.0.0, every interface)
  --token <secret>      Require "Authorization: Bearer <secret>" (default: $CLOCTECK_MCP_TOKEN)
  --allow-host <name>   Extra Host header to accept, repeatable (IPs, localhost and this
                        computer's name are always accepted)
  -h, --help            Show this help`;

const { values } = parseArgs({
  options: {
    clock: { type: 'string' },
    port: { type: 'string' },
    bind: { type: 'string', default: '0.0.0.0' },
    token: { type: 'string' },
    'allow-host': { type: 'string', multiple: true, default: [] },
    help: { type: 'boolean', short: 'h', default: false },
  },
});

if (values.help) {
  console.log(USAGE);
  process.exit(0);
}

const port = Number(values.port ?? process.env.CLOCTECK_MCP_PORT ?? 3939);
const token = values.token ?? process.env.CLOCTECK_MCP_TOKEN;
const clock = new ClocTeckClient({ host: values.clock ?? process.env.CLOCTECK_HOST ?? DEFAULT_HOST });
const log = (message: string) => console.error(`[${new Date().toLocaleTimeString()}] ${message}`);
const attention = new AttentionController(clock, log);

const DEFAULT_DURATION_SECONDS = 5;
const MAX_DURATION_SECONDS = 30;

function createServer(caller: string): McpServer {
  const server = new McpServer({ name: 'clocteck-tube-clock', version: '0.1.0' });

  server.registerTool(
    'get_attention',
    {
      title: 'Get the user\'s attention',
      description:
        'Flashes the user\'s RGB tube clock a color to get their attention, e.g. when a long task finishes, ' +
        'something needs their input, or something failed. The clock returns to the user\'s own colors on its own ' +
        `after the duration (default ${DEFAULT_DURATION_SECONDS} seconds). Use sparingly: it is a physical light on their desk.`,
      inputSchema: z.object({
        color: z
          .string()
          .default('orange')
          .describe(`A color name (${Object.keys(NAMED_COLORS).join(', ')}), "#rrggbb", or "h,s,v" (hue 0-360, saturation/value 0-100)`),
        durationSeconds: z
          .number()
          .min(1)
          .max(MAX_DURATION_SECONDS)
          .default(DEFAULT_DURATION_SECONDS)
          .describe('How long to show the color before restoring the user\'s colors'),
        blink: z.boolean().default(false).describe('Pulse the brightness instead of holding a solid color'),
        message: z.string().max(500).optional().describe('Why you need attention; shown in the server log'),
      }),
      annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    },
    async ({ color, durationSeconds, blink, message }) => {
      let hsv;
      try {
        hsv = parseColor(color);
      } catch (err) {
        return { isError: true, content: [{ type: 'text', text: err instanceof Error ? err.message : String(err) }] };
      }
      log(`${caller}: ${hsvToHex(hsv)} for ${durationSeconds}s${blink ? ' (blink)' : ''}${message ? ` - ${message}` : ''}`);
      try {
        const result = await attention.flash({ color: hsv, durationMs: durationSeconds * 1000, blink });
        return {
          content: [
            {
              type: 'text',
              text: `Clock is showing ${hsvToHex(hsv)}${blink ? ', blinking,' : ''} and returns to the user's colors at ${result.restoreAt.toLocaleTimeString()}.`,
            },
          ],
        };
      } catch (err) {
        const text = `Could not reach the clock at ${clock.host}: ${err instanceof Error ? err.message : String(err)}`;
        log(text);
        return { isError: true, content: [{ type: 'text', text }] };
      }
    },
  );

  server.registerTool(
    'clock_status',
    {
      title: 'Tube clock status',
      description: 'Reads the tube clock\'s current settings and whether an attention flash is showing.',
      inputSchema: z.object({}),
      annotations: { readOnlyHint: true, openWorldHint: false },
    },
    async () => {
      try {
        const { raw: _, ...config } = await clock.getConfig();
        const status = { host: clock.host, flashing: attention.active, ...config };
        return { content: [{ type: 'text', text: JSON.stringify(status, null, 2) }] };
      } catch (err) {
        return { isError: true, content: [{ type: 'text', text: `Could not reach the clock at ${clock.host}: ${err instanceof Error ? err.message : String(err)}` }] };
      }
    },
  );

  return server;
}

/** Blocks DNS rebinding: a web page can't make a browser send requests here under a hostname it controls. */
function isAllowedHost(hostHeader: string | undefined): boolean {
  if (!hostHeader) return false;
  const host = hostHeader.replace(/:\d+$/, '').replace(/^\[|\]$/g, '').toLowerCase();
  if (isIP(host) || host === 'localhost') return true;
  const machine = os.hostname().toLowerCase();
  const allowed = [machine, `${machine}.local`, `${machine}.lan`, ...values['allow-host'].map((h) => h.toLowerCase())];
  return allowed.includes(host);
}

async function toWebRequest(req: http.IncomingMessage): Promise<Request> {
  const headers = new Headers();
  for (const [key, value] of Object.entries(req.headers)) {
    if (Array.isArray(value)) value.forEach((v) => headers.append(key, v));
    else if (value !== undefined) headers.set(key, value);
  }
  const hasBody = req.method !== 'GET' && req.method !== 'HEAD';
  return new Request(`http://${req.headers.host ?? 'localhost'}${req.url ?? '/'}`, {
    method: req.method,
    headers,
    body: hasBody ? (Readable.toWeb(req) as ReadableStream) : undefined,
    duplex: hasBody ? 'half' : undefined,
  } as RequestInit);
}

async function sendWebResponse(res: http.ServerResponse, response: Response) {
  res.writeHead(response.status, Object.fromEntries(response.headers));
  if (!response.body) {
    res.end();
    return;
  }
  const body = Readable.fromWeb(response.body as import('node:stream/web').ReadableStream);
  res.on('close', () => body.destroy());
  body.pipe(res);
}

function plain(res: http.ServerResponse, status: number, text: string) {
  res.writeHead(status, { 'content-type': 'text/plain' }).end(text);
}

const httpServer = http.createServer(async (req, res) => {
  const caller = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? 'unknown';
  try {
    if (!isAllowedHost(req.headers.host)) return plain(res, 403, 'Host not allowed');
    const path = new URL(req.url ?? '/', 'http://localhost').pathname;
    if (path === '/health') return plain(res, 200, 'ok');
    if (path !== '/mcp') return plain(res, 404, 'Not found. The MCP endpoint is /mcp');
    if (token && req.headers.authorization !== `Bearer ${token}`) {
      log(`${caller}: rejected (bad or missing token)`);
      res.writeHead(401, { 'www-authenticate': 'Bearer' }).end('Unauthorized');
      return;
    }
    const handler = createMcpHandler(() => createServer(caller));
    await sendWebResponse(res, await handler.fetch(await toWebRequest(req)));
  } catch (err) {
    log(`${caller}: request failed: ${err instanceof Error ? err.stack : String(err)}`);
    if (!res.headersSent) plain(res, 500, 'Internal error');
    else res.end();
  }
});

httpServer.listen(port, values.bind, () => {
  const lan = Object.values(os.networkInterfaces())
    .flat()
    .filter((i) => i && i.family === 'IPv4' && !i.internal)
    .map((i) => `http://${i!.address}:${port}/mcp`);
  log(`Tube clock MCP server for ${clock.host} listening on ${values.bind}:${port}`);
  for (const url of lan) log(`  ${url}`);
  log(token ? 'Bearer token required' : 'No token set: anyone on your network can flash the clock (set --token to require one)');
});

async function shutdown() {
  log('Shutting down; restoring the clock');
  httpServer.close();
  await attention.restore();
  process.exit(0);
}
process.on('SIGINT', () => void shutdown());
process.on('SIGTERM', () => void shutdown());
