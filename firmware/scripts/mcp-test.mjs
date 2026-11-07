// End-to-end test of the firmware's MCP server (spec/firmware/mcp.md, VER-007), against the emulator
// (default) or a real clock:
//   node firmware/scripts/mcp-test.mjs [http://localhost:9080]
// Uses the official MCP TypeScript client over Streamable HTTP, plus raw HTTP for what a conforming
// client never sends (batches, oversized bodies, bad headers). Reads FW_API_TOKEN from the repo's
// .env without printing it (VER-005). Check names start with the requirement IDs they cover (VER-002).
// Nothing saved is changed; the last check cancels the flash it started.
import http from 'node:http';
import { readFileSync } from 'node:fs';
import { Client, ProtocolError, StreamableHTTPClientTransport } from '@modelcontextprotocol/client';
import { Ajv } from '@modelcontextprotocol/client/validators/ajv';

const base = process.argv[2] || 'http://localhost:9080';
const env = readFileSync(new URL('../../.env', import.meta.url), 'utf8');
const token = [...env.matchAll(/^FW_API_TOKEN=(.*)$/gm)].at(-1)?.[1].trim().replace(/^"(.*)"$/, '$1');
if (!token) throw new Error('FW_API_TOKEN is empty in .env');
const auth = { Authorization: `Bearer ${token}` };
const VERSIONS = ['2025-03-26', '2025-06-18', '2025-11-25'];

let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${ok ? '' : ` ${typeof detail === 'string' ? detail : JSON.stringify(detail)}`}`);
  if (!ok) failed++;
};

// fetch() cannot set Host, so requests go through node:http. Against the emulator (reached as
// localhost) the Host header names the clock instead, as a real client's would (FW-MCP-010).
const baseUrl = new URL(base);
let hostHeader;
function hostFetch(url, init = {}, host = hostHeader) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const headers = {};
    new Headers(init.headers).forEach((v, k) => (headers[k] = v));
    if (host) headers.host = host;
    const body = init.body == null ? undefined : typeof init.body === 'string' ? init.body : Buffer.from(init.body);
    if (body !== undefined) headers['content-length'] = String(Buffer.byteLength(body));
    const req = http.request(
      { hostname: u.hostname, port: u.port, path: u.pathname + u.search, method: init.method || 'GET', headers, signal: init.signal },
      (res) => {
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('error', reject);
        res.on('end', () => {
          const h = new Headers();
          for (const [k, v] of Object.entries(res.headers)) h.set(k, Array.isArray(v) ? v.join(', ') : v);
          const empty = [204, 205, 304].includes(res.statusCode);
          resolve(new Response(empty ? null : Buffer.concat(chunks), { status: res.statusCode, statusText: res.statusMessage, headers: h }));
        });
      },
    );
    req.on('error', reject);
    if (body !== undefined) req.write(body);
    req.end();
  });
}

const mcpUrl = new URL('/mcp', base);
// A raw POST to /mcp: status, headers, body text and parsed JSON.
async function post(body, headers = {}, { host = hostHeader, method = 'POST', path = '/mcp' } = {}) {
  const r = await hostFetch(
    new URL(path, base),
    {
      method,
      headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream', ...auth, ...headers },
      body: method === 'POST' ? (typeof body === 'string' ? body : JSON.stringify(body)) : undefined,
    },
    host,
  );
  const text = await r.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {}
  return { status: r.status, headers: r.headers, text, json };
}
const ping = (id = 1) => ({ jsonrpc: '2.0', id, method: 'ping' });

async function connect({ versions, headers = auth, negotiation } = {}) {
  const client = new Client(
    { name: 'tubeclock-mcp-test', version: '1.0.0' },
    { ...(versions ? { supportedProtocolVersions: versions } : {}), ...(negotiation ? { versionNegotiation: negotiation } : {}) },
  );
  const transport = new StreamableHTTPClientTransport(mcpUrl, { fetch: hostFetch, requestInit: { headers } });
  await client.connect(transport);
  return client;
}
const textOf = (result) => result?.content?.[0]?.text ?? '';
const state = async () => (await hostFetch(new URL('/api/state', base), { headers: auth })).json();

// ---- Preflight ----
try {
  await hostFetch(new URL('/health', base), {}, undefined);
} catch {
  console.log(`FAIL nothing is listening at ${base}: start it with npm run fw:emulate`);
  process.exit(1);
}
const before = await state();
check('FW-API-039 state names the hostname', typeof before.hostname === 'string' && before.hostname.length > 0, before.hostname);
if (/^(localhost|127\.0\.0\.1)$/.test(baseUrl.hostname)) hostHeader = `${before.hostname}.local${baseUrl.port ? `:${baseUrl.port}` : ''}`;

// ---- Protocol, through the official client ----
for (const v of VERSIONS) {
  const client = await connect({ versions: [v] });
  check(`FW-MCP-020 FW-MCP-021 initialize with ${v}`, client.getNegotiatedProtocolVersion() === v, client.getNegotiatedProtocolVersion());
  await client.ping();
  check(`FW-MCP-023 ping on ${v}`, true);
  await client.close();
}

const client = await connect();
const info = client.getServerVersion();
check('FW-MCP-021 FW-API-031 serverInfo is tubeclock at the firmware version', info?.name === 'tubeclock' && info?.version === before.version, info);
check('FW-MCP-021 capabilities are tools only, no listChanged', JSON.stringify(client.getServerCapabilities()) === '{"tools":{}}', client.getServerCapabilities());
check('FW-MCP-021 instructions ask for get_attention to be used sparingly', /sparingly/.test(client.getInstructions() ?? ''), client.getInstructions());

const { tools, nextCursor } = await client.listTools();
check('FW-MCP-024 FW-MCP-033 tools/list is get_attention, cancel_attention, clock_status', tools.map((t) => t.name).join() === 'get_attention,cancel_attention,clock_status', tools.map((t) => t.name));
check('FW-MCP-024 no nextCursor', nextCursor === undefined, nextCursor);
const ajv = new Ajv({ strict: false });
for (const t of tools) {
  const s = t.inputSchema;
  let compiles = false;
  try {
    ajv.compile(s);
    compiles = true;
  } catch {}
  check(
    `FW-MCP-034 ${t.name} inputSchema is a closed object schema that compiles`,
    compiles && s.type === 'object' && typeof s.properties === 'object' && Array.isArray(s.required) && s.additionalProperties === false,
    s,
  );
}
const att = tools[0]?.annotations ?? {};
check('FW-MCP-030 get_attention annotations', att.readOnlyHint === false && att.destructiveHint === false && att.idempotentHint === true && att.openWorldHint === false, att);
check('FW-MCP-031 cancel_attention is idempotent', tools[1]?.annotations?.idempotentHint === true, tools[1]?.annotations);
check('FW-MCP-032 clock_status is read-only', tools[2]?.annotations?.readOnlyHint === true, tools[2]?.annotations);

// ---- Tools ----
let r = await client.callTool({ name: 'get_attention', arguments: { color: 'red', durationSeconds: 3 } });
check('FW-MCP-030 FW-MCP-025 get_attention red for 3 s', textOf(r) === 'Clock is showing #ff0000 for 3 s.' && !r.isError, r);
let s = await state();
check('FW-ATT-008 the MCP flash shows in /api/state', s.attention === true, s.attention);
check('FW-API-004 FW-MCP-033 the flash leaves saved colors alone', JSON.stringify(s.tubes) === JSON.stringify(before.tubes), s.tubes);

r = await client.callTool({ name: 'clock_status', arguments: {} });
let status;
try {
  status = JSON.parse(textOf(r));
} catch {}
check('FW-MCP-032 clock_status is the state object as pretty JSON', status?.version === before.version && status?.attention === true && textOf(r).includes('\n'), textOf(r));

r = await client.callTool({ name: 'get_attention', arguments: {} });
check('FW-MCP-030 get_attention defaults to orange for 5 s', textOf(r) === 'Clock is showing #ff6a00 for 5 s.', textOf(r));
r = await client.callTool({ name: 'get_attention', arguments: { color: '#00f', durationSeconds: 2.5, blink: true, message: 'mcp-test' } });
check('FW-MCP-030 FW-MCP-035 blinking flash replaces the last one', textOf(r) === 'Clock is showing #0000ff, blinking, for 2.5 s.', textOf(r));
r = await client.callTool({ name: 'get_attention', arguments: { color: '120,100,100', beep: true } });
check('FW-MCP-030 FW-ATT-010 beep is accepted and the reply says no buzzer sounded it', /^Clock is showing #00ff00 for 5 s\. No beep: /.test(textOf(r)) && !r.isError, textOf(r));
r = await client.callTool({ name: 'get_attention', arguments: { color: 'chartreuse' } });
check('FW-MCP-025 FW-MCP-036 bad color is a tool error', r.isError === true && /Invalid color/.test(textOf(r)), r);
r = await client.callTool({ name: 'get_attention', arguments: { durationSeconds: 60 } });
check('FW-MCP-025 durationSeconds over 30 is a tool error', r.isError === true, r);
r = await client.callTool({ name: 'get_attention', arguments: { message: 'x'.repeat(201) } });
check('FW-MCP-030 message over 200 characters is a tool error', r.isError === true, r);

// FW-MCP-036, HOST-LIB-020: the clock's table is a C++ copy, so check it against src/color.ts. A
// channel may differ by 1: JavaScript and the clock round .5 differently on the way back to RGB.
const hostColors = [...readFileSync(new URL('../../src/color.ts', import.meta.url), 'utf8').matchAll(/^\s+(\w+): '(#[0-9a-f]{6})',$/gm)];
check('FW-MCP-036 src/color.ts lists ten named colors', hostColors.length === 10, hostColors.length);
const channels = (hex) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
for (const [, name, hex] of hostColors) {
  r = await client.callTool({ name: 'get_attention', arguments: { color: name, durationSeconds: 1 } });
  const shown = /#[0-9a-f]{6}/.exec(textOf(r))?.[0] ?? '#000000';
  const close = channels(shown).every((c, i) => Math.abs(c - channels(hex)[i]) <= 1);
  check(`FW-MCP-036 ${name} matches the host table`, close && !r.isError, `${shown} vs ${hex}`);
}

r = await client.callTool({ name: 'cancel_attention', arguments: {} });
s = await state();
check('FW-MCP-031 cancel_attention', textOf(r) === 'Attention cancelled.' && s.attention === false, { text: textOf(r), attention: s.attention });
r = await client.callTool({ name: 'cancel_attention', arguments: {} });
check('FW-MCP-031 cancel_attention with nothing showing', textOf(r) === 'Attention cancelled.', textOf(r));

const errorCode = async (fn) => {
  try {
    await fn();
    return 'no error';
  } catch (e) {
    return e instanceof ProtocolError ? e.code : `${e?.name}: ${e?.message}`;
  }
};
const fresh = await connect();  // no tools/list yet, so the client cannot refuse the name itself
check('FW-MCP-025 FW-MCP-033 unknown tool is -32602', (await errorCode(() => fresh.callTool({ name: 'set_brightness', arguments: { v: 1 } }))) === -32602);
check('FW-MCP-025 wrong argument type is -32602', (await errorCode(() => fresh.callTool({ name: 'get_attention', arguments: { durationSeconds: '5' } }))) === -32602);
await fresh.close();
await client.close();

// FW-MCP-007: a client that tries the 2026-07-28 era first gets a 400 without a modern error body
// and falls back to initialize.
try {
  const auto = await connect({ negotiation: { mode: 'auto' } });
  check('FW-MCP-007 FW-MCP-020 a dual-era client falls back to 2025-11-25', auto.getNegotiatedProtocolVersion() === '2025-11-25', auto.getNegotiatedProtocolVersion());
  await auto.close();
} catch (e) {
  check('FW-MCP-007 FW-MCP-020 a dual-era client falls back to 2025-11-25', false, `${e?.name}: ${e?.message}`);
}

// ---- Transport rules, raw HTTP ----
let p = await post(ping('abc'));
check('FW-MCP-004 FW-MCP-006 a request is 200 application/json even when SSE is accepted', p.status === 200 && p.headers.get('content-type') === 'application/json', `${p.status} ${p.headers.get('content-type')}`);
check('FW-MCP-027 a string id is echoed', p.json?.id === 'abc' && JSON.stringify(p.json?.result) === '{}', p.text);
p = await post(ping(), { 'MCP-Protocol-Version': '2025-06-18', 'Mcp-Session-Id': 'abc123' });
check('FW-MCP-008 a session id is ignored and none is issued', p.status === 200 && !p.headers.has('mcp-session-id'), p.status);
p = await post(ping());
check('FW-MCP-007 no MCP-Protocol-Version header is accepted', p.status === 200, p.status);
p = await post(ping(), { 'MCP-Protocol-Version': '2026-07-28' });
check('FW-MCP-007 an unsupported MCP-Protocol-Version is 400', p.status === 400 && p.json?.error?.code === -32600, `${p.status} ${p.text}`);
p = await post({ jsonrpc: '2.0', method: 'notifications/initialized' });
check('FW-MCP-005 FW-MCP-022 notifications/initialized is 202 with no body', p.status === 202 && p.text === '', `${p.status} ${p.text}`);
p = await post([ping(1), ping(2)]);
check('FW-MCP-003 a batch is 400 -32600', p.status === 400 && p.json?.error?.code === -32600, `${p.status} ${p.text}`);
p = await post('{"jsonrpc":"2.0","id":1,');
check('FW-MCP-012 unparseable JSON is 400 -32700 with id null', p.status === 400 && p.json?.error?.code === -32700 && p.json?.id === null, `${p.status} ${p.text}`);
// The client's own capability check never lets these reach the clock, hence raw requests.
for (const method of ['resources/list', 'prompts/list', 'logging/setLevel', 'completion/complete', 'no/such/method']) {
  p = await post({ jsonrpc: '2.0', id: 4, method });
  check(`FW-MCP-026 FW-MCP-027 ${method} is 200 with -32601`, p.status === 200 && p.json?.error?.code === -32601 && p.json?.id === 4, `${p.status} ${p.text}`);
}
p = await post({ jsonrpc: '2.0', id: 4, method: 'tools/call', params: { name: 'set_brightness', arguments: {} } });
check('FW-MCP-025 FW-MCP-033 the clock itself answers an unknown tool with -32602', p.status === 200 && p.json?.error?.code === -32602, `${p.status} ${p.text}`);
// A body of exactly `size` bytes.
const sized = (size) => {
  const shell = JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'ping', params: { pad: '' } });
  return JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'ping', params: { pad: 'x'.repeat(size - shell.length) } });
};
p = await post(sized(2048));
check('FW-BUD-004 a body of exactly 2048 bytes is served', p.status === 200, `${p.status} ${p.text}`);
p = await post(sized(2049));
check('FW-MCP-012 FW-BUD-004 FW-SEC-013 2049 bytes is 413', p.status === 413, p.status);
p = await post(sized(200000));
check('FW-MCP-012 FW-SEC-013 200 KB is 413 without being read', p.status === 413, p.status);
p = await post(`{"jsonrpc":"2.0","id":1,"method":"ping","params":[${'1,'.repeat(1000)}1]}`);
check('FW-BUD-003 a small body that would overflow the JSON arena is 413, not a crash', p.status === 413, `${p.status} ${p.text}`);
p = await post(`{"jsonrpc":"2.0","id":1,"method":"ping","params":${'['.repeat(200)}${']'.repeat(200)}}`);
check('FW-MCP-012 JSON nested too deep is 400', p.status === 400, `${p.status} ${p.text}`);
p = await post(ping());
check('FW-BUD-003 the clock still answers after those', p.status === 200, p.status);
p = await post({ jsonrpc: '2.0', id: 1, method: 'tools/list' });
check('FW-BUD-005 tools/list is the largest answer and stays under 3072 bytes', p.status === 200 && p.text.length < 3072, p.text.length);
console.log(`tools/list: ${p.text.length} bytes`);
p = await post(null, {}, { method: 'GET' });
check('FW-MCP-002 GET /mcp is 405', p.status === 405 && p.headers.get('allow') === 'POST', `${p.status} ${p.headers.get('allow')}`);
p = await post(null, {}, { method: 'DELETE' });
check('FW-MCP-002 DELETE /mcp is 405', p.status === 405, p.status);

// FW-MCP-009, FW-SEC-005..008: the token, as for /api. Fewer than 5 failures in a row, so the
// backoff (FW-SEC-014) never kicks in; a good request in between clears the count.
p = await post(ping(), { Authorization: '' });
check('FW-MCP-009 FW-SEC-005 FW-SEC-007 no token is 401', p.status === 401 && p.headers.get('www-authenticate') === 'Bearer' && p.json?.error === 'unauthorized', `${p.status} ${p.text}`);
p = await post(ping(), { Authorization: 'Bearer wrong' });
check('FW-MCP-009 FW-SEC-007 wrong token is 401', p.status === 401 && p.json?.error === 'unauthorized', p.status);
p = await post(ping(), { Authorization: '' }, { path: `/mcp?token=${encodeURIComponent(token)}` });
check('FW-SEC-008 a token in the query string is not accepted', p.status === 401, p.status);
p = await post(ping());
check('FW-SEC-006 a good token right after bad ones still works', p.status === 200, p.status);
try {
  await (await connect({ headers: {} })).close();
  check('FW-MCP-009 the official client without a token is refused', false, 'connected');
} catch (e) {
  check('FW-MCP-009 the official client without a token is refused', /401|nauthorized/.test(`${e?.name} ${e?.message} ${e?.code}`), `${e?.name}: ${e?.message}`);
}
p = await post(ping());

// FW-MCP-010, FW-MCP-011, FW-SEC-012: Host and Origin, checked before the token.
p = await post(ping(), {}, { host: 'evil.example' });
check('FW-MCP-010 a foreign Host is 403', p.status === 403, p.status);
p = await post(ping(), { Authorization: '' }, { host: 'evil.example' });
check('FW-SEC-012 the Host check comes before the token check', p.status === 403, p.status);
p = await post(ping(), { Origin: 'http://evil.example' });
check('FW-MCP-011 a foreign Origin is 403', p.status === 403, p.status);
p = await post(ping(), { Origin: `https://${before.hostname}.local` });
check('FW-MCP-011 an https Origin is 403', p.status === 403, p.status);
p = await post(ping(), { Origin: `http://${before.hostname}.local` });
check('FW-MCP-011 the clock’s own Origin is allowed', p.status === 200, p.status);
try {
  await (await connect({ headers: { ...auth, Origin: 'http://evil.example' } })).close();
  check('FW-MCP-011 the official client with a foreign Origin is refused', false, 'connected');
} catch (e) {
  check('FW-MCP-011 the official client with a foreign Origin is refused', /403|orbidden/.test(`${e?.name} ${e?.message} ${e?.code}`), `${e?.name}: ${e?.message}`);
}

p = await post({ jsonrpc: '2.0', id: 9, method: 'tools/call', params: { name: 'cancel_attention', arguments: {} } });
s = await state();
check('VER-004 nothing left showing and nothing saved changed', s.attention === false && JSON.stringify(s.tubes) === JSON.stringify(before.tubes) && s.brightness === before.brightness, s);

console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
