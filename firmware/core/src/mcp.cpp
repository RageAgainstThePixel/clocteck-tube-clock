#include "clock_core/mcp.h"

#include <cstdio>
#include <cstring>

#include "clock_core/json.h"
#include "clock_core/json_arena.h"

// On the ESP8285 the tool list stays in flash (FW-MCP-034); ArduinoJson has pulled in Arduino.h.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef ARDUINO
#define memcpy_P memcpy
#endif

namespace clock_core {

const char* const kMcpVersions[kMcpVersionCount] = {"2025-03-26", "2025-06-18", "2025-11-25"};
const char kMcpAssumedVersion[] = "2025-03-26";

namespace {

// FW-MCP-021
const char kInstructions[] =
    "This server is a physical tube clock on the user's desk. get_attention lights its six tubes in a "
    "color so the user looks up: use it sparingly, only when something needs the user (a long task "
    "finished, a question is waiting), never for routine progress. cancel_attention ends a flash early; "
    "clock_status reads the clock's state.";

// FW-MCP-024, FW-MCP-030..034: the fixed tool list, kept in flash as one literal.
const char kToolsList[] PROGMEM =
    R"({"tools":[)"
    R"({"name":"get_attention","title":"Get the user's attention",)"
    R"("description":"Flash the user's desk clock in a color so the user looks up, e.g. when a long task )"
    R"(finishes or input is needed. Use sparingly. The clock goes back to its own colors when the flash ends.",)"
    R"("inputSchema":{"type":"object","properties":{)"
    R"("color":{"type":"string","default":"orange","description":"red, orange, yellow, green, cyan, blue, )"
    R"~(purple, magenta, pink, white, #rrggbb, #rgb or h,s,v (h 0-360, s and v 0-100)"},)~"
    R"("durationSeconds":{"type":"number","minimum":1,"maximum":30,"default":5,"description":"How long to flash"},)"
    R"("blink":{"type":"boolean","default":false,"description":"Blink once a second instead of a steady color"},)"
    R"("beep":{"type":"boolean","default":false,"description":"Also beep twice briefly when the flash starts"},)"
    R"("message":{"type":"string","maxLength":200,"description":"Why, for the clock's serial log; not shown on the clock"}},)"
    R"("required":[],"additionalProperties":false},)"
    R"("annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},)"
    R"({"name":"cancel_attention","title":"Cancel the attention flash",)"
    R"("description":"End an attention flash early. Harmless when none is showing.",)"
    R"("inputSchema":{"type":"object","properties":{},"required":[],"additionalProperties":false},)"
    R"("annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},)"
    R"({"name":"clock_status","title":"Clock status",)"
    R"("description":"The clock's state: firmware version, local time, time zone, hour format, brightness, )"
    R"(tube colors, whether a flash is showing, and time-sync health.",)"
    R"("inputSchema":{"type":"object","properties":{},"required":[],"additionalProperties":false},)"
    R"("annotations":{"readOnlyHint":true,"openWorldHint":false}})"
    R"(]})";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kServerError = -32000;

constexpr size_t kMaxIdChars = 64;
constexpr size_t kStateBytes = 640;  // the state grew preset, effect and display (FW-FX-069)

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The first `len` bytes of `a` equal the NUL-terminated `b`, ignoring ASCII case.
bool equalsIgnoreCase(const char* a, size_t len, const char* b) {
  for (size_t i = 0; i < len; i++) {
    if (!b[i] || lower(a[i]) != lower(b[i])) return false;
  }
  return b[len] == '\0';
}

bool hostMatches(const char* host, size_t len, const char* ip, const char* hostname) {
  // Drop a ":port" suffix.
  for (size_t i = len; i > 0; i--) {
    const char c = host[i - 1];
    if (c == ':') {
      if (i == len) return false;  // "host:" with no port
      len = i - 1;
      break;
    }
    if (c < '0' || c > '9') break;
  }
  if (len == 0) return false;
  if (ip && *ip && equalsIgnoreCase(host, len, ip)) return true;
  if (!hostname || !*hostname) return false;
  if (equalsIgnoreCase(host, len, hostname)) return true;
  const size_t n = strlen(hostname);
  return len == n + 6 && equalsIgnoreCase(host, n, hostname) && equalsIgnoreCase(host + n, 6, ".local");
}

// Appends to the response buffer, keeping it NUL-terminated; anything that does not fit sets
// `overflow` and is dropped.
struct Out {
  char* buf;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  Out(char* b, size_t c) : buf(b), cap(c) {
    if (cap) buf[0] = '\0';
  }

  bool fits(size_t n) {
    if (overflow || len + n >= cap) overflow = true;
    return !overflow;
  }
  void raw(const char* s, size_t n) {
    if (!fits(n)) return;
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }
  void str(const char* s) { raw(s, strlen(s)); }
  void flash(const char* s, size_t n) {
    if (!fits(n)) return;
    memcpy_P(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }
  void num(long v) {
    char t[16];
    snprintf(t, sizeof(t), "%ld", v);
    str(t);
  }
  void json(JsonVariantConst v) {
    const size_t n = measureJson(v);
    if (!fits(n)) return;
    serializeJson(v, buf + len, cap - len);
    len += n;
  }
  void jsonString(const char* s) {
    static const char kHex[] = "0123456789abcdef";
    raw("\"", 1);
    for (; *s; s++) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        const char e[2] = {'\\', static_cast<char>(c)};
        raw(e, 2);
      } else if (c == '\n') {
        raw("\\n", 2);
      } else if (c < 0x20) {
        const char e[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
        raw(e, 6);
      } else {
        raw(s, 1);
      }
    }
    raw("\"", 1);
  }
};

void envelope(Out& out, const char* id) {
  out.str("{\"jsonrpc\":\"2.0\",\"id\":");
  out.str(id);
}

void writeError(Out& out, const char* id, int code, const char* message) {
  envelope(out, id);
  out.str(",\"error\":{\"code\":");
  out.num(code);
  out.str(",\"message\":");
  out.jsonString(message);
  out.str("}}");
}

void writeText(Out& out, const char* id, const char* text, bool isError) {
  envelope(out, id);
  out.str(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
  out.jsonString(text);
  out.str(isError ? "}],\"isError\":true}}" : "}]}}");
}

// UTF-8 code points, for the `message` length rule.
size_t codePoints(const char* s) {
  size_t n = 0;
  for (; *s; s++) n += (static_cast<unsigned char>(*s) & 0xC0) != 0x80;
  return n;
}

// The message as one printable-ASCII log line: anything else becomes '?', one per code point.
void appendLoggable(char* out, size_t cap, const char* s) {
  size_t len = strlen(out);
  for (; *s && len + 1 < cap; s++) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if ((c & 0xC0) == 0x80) continue;
    out[len++] = c >= 0x20 && c < 0x7F && c != '"' ? static_cast<char>(c) : '?';
  }
  out[len] = '\0';
}

// "5", "2.5", "1.25"
void formatSeconds(uint32_t ms, char (&out)[16]) {
  const unsigned long whole = ms / 1000, frac = ms % 1000;
  if (frac == 0) {
    snprintf(out, sizeof(out), "%lu", whole);
    return;
  }
  snprintf(out, sizeof(out), "%lu.%03lu", whole, frac);
  size_t len = strlen(out);
  while (out[len - 1] == '0') out[--len] = '\0';
}

McpReply reply(int status, const Out& out) { return {status, out.overflow ? 0 : out.len}; }

// tools/call get_attention (FW-MCP-030, FW-MCP-035)
void callGetAttention(JsonObjectConst args, const char* id, McpDevice& device, Out& out) {
  JsonVariantConst color = args["color"];
  JsonVariantConst duration = args["durationSeconds"];
  JsonVariantConst blink = args["blink"];
  JsonVariantConst beep = args["beep"];
  JsonVariantConst message = args["message"];
  // FW-MCP-025: a wrong type is a protocol error; a bad value is the tool's error, so the agent can
  // correct itself. FW-MCP-013: refused calls are logged too.
  auto badType = [&](const char* text) {
    device.logToolCall("get_attention", "refused: wrong argument type");
    writeError(out, id, kInvalidParams, text);
  };
  auto badValue = [&](const char* text) {
    device.logToolCall("get_attention", "refused: bad argument value");
    writeText(out, id, text, true);
  };
  if (!color.isNull() && !color.is<const char*>()) return badType("color must be a string");
  if (!duration.isNull() && !duration.is<double>()) return badType("durationSeconds must be a number");
  if (!blink.isNull() && !blink.is<bool>()) return badType("blink must be a boolean");
  if (!beep.isNull() && !beep.is<bool>()) return badType("beep must be a boolean");
  if (!message.isNull() && !message.is<const char*>()) return badType("message must be a string");

  AttentionCall call{};
  if (!parseColor(color.isNull() ? "orange" : color.as<const char*>(), call.color)) {
    return badValue(
        "Invalid color. Use a name (red, orange, yellow, green, cyan, blue, purple, magenta, pink, white), #rrggbb, "
        "#rgb or h,s,v.");
  }
  const double seconds = duration.isNull() ? 5.0 : duration.as<double>();
  if (!(seconds >= 1.0 && seconds <= 30.0)) return badValue("durationSeconds must be between 1 and 30.");
  const char* note = message.isNull() ? nullptr : message.as<const char*>();
  if (note && codePoints(note) > kMcpMaxMessageChars) return badValue("message must be at most 200 characters.");
  call.durationMs = static_cast<uint32_t>(seconds * 1000.0 + 0.5);
  call.blink = blink.as<bool>();
  call.beep = beep.as<bool>();
  device.startAttention(call);

  char hex[8];
  formatHex(hsvToRgb({call.color.h, call.color.s, 100}), hex);
  char secs[16];
  formatSeconds(call.durationMs, secs);

  char summary[280];
  snprintf(summary, sizeof(summary), "%s %lums blink=%d beep=%d", hex, static_cast<unsigned long>(call.durationMs),
           call.blink ? 1 : 0, call.beep ? 1 : 0);
  if (note) {
    strcat(summary, " \"");
    appendLoggable(summary, sizeof(summary) - 1, note);
    strcat(summary, "\"");
  }
  device.logToolCall("get_attention", summary);

  // FW-ATT-010: without a buzzer the beep is accepted but nothing sounds, and the reply says so.
  const bool beeped = call.beep && device.hasBuzzer();
  char text[200];
  snprintf(text, sizeof(text), "Clock is showing %s%s%s for %s s.%s", hex, call.blink ? ", blinking," : "",
           beeped ? " with a beep," : "", secs,
           call.beep && !beeped ? " No beep: this clock has no buzzer configured." : "");
  writeText(out, id, text, false);
}

// Well-formed JSON laid out as serializeJsonPretty does, without a document: the state has grown too
// big to parse beside the request within the 4 KB arena (FW-BUD-003). Returns the length; with `out`
// nullptr it only measures.
size_t prettyJson(const char* json, size_t len, char* out) {
  size_t n = 0;
  int depth = 0;
  bool inString = false;
  auto put = [&](char c) {
    if (out) out[n] = c;
    n++;
  };
  auto newLine = [&] {
    put('\r');
    put('\n');
    for (int i = 0; i < depth * 2; i++) put(' ');
  };
  for (size_t i = 0; i < len; i++) {
    const char c = json[i];
    if (inString) {
      put(c);
      if (c == '\\' && i + 1 < len) {
        put(json[++i]);
      } else if (c == '"') {
        inString = false;
      }
    } else if (c == '{' || c == '[') {
      put(c);
      if (i + 1 < len && (json[i + 1] == '}' || json[i + 1] == ']')) {
        put(json[++i]);
      } else {
        depth++;
        newLine();
      }
    } else if (c == '}' || c == ']') {
      depth--;
      newLine();
      put(c);
    } else {
      put(c);
      if (c == '"') inString = true;
      if (c == ',') newLine();
      if (c == ':') put(' ');
    }
  }
  if (out) out[n] = '\0';
  return n;
}

// tools/call clock_status (FW-MCP-032): the state object, pretty-printed.
void callClockStatus(const char* id, McpDevice& device, FixedArena& arena, Out& out) {
  device.logToolCall("clock_status", "");
  const size_t mark = arena.mark();
  char* state = static_cast<char*>(arena.allocate(kStateBytes));
  const size_t n = state ? device.writeState(state, kStateBytes) : 0;
  char* pretty = n ? static_cast<char*>(arena.allocate(prettyJson(state, n, nullptr) + 1)) : nullptr;
  if (pretty) {
    prettyJson(state, n, pretty);
    writeText(out, id, pretty, false);
  } else {
    writeText(out, id, "Clock state is unavailable.", true);
  }
  arena.release(mark);
}

}  // namespace

bool isSupportedMcpVersion(const char* version) {
  if (!version) return false;
  for (const char* v : kMcpVersions) {
    if (strcmp(v, version) == 0) return true;
  }
  return false;
}

const char* negotiateMcpVersion(const char* requested) {
  for (const char* v : kMcpVersions) {
    if (requested && strcmp(v, requested) == 0) return v;
  }
  return kMcpVersions[kMcpVersionCount - 1];
}

bool mcpHostAllowed(const char* host, const char* ip, const char* hostname) {
  return host && hostMatches(host, strlen(host), ip, hostname);
}

bool mcpOriginAllowed(const char* origin, const char* ip, const char* hostname) {
  if (!origin) return false;
  const size_t len = strlen(origin);
  if (len <= 7 || !equalsIgnoreCase(origin, 7, "http://")) return false;
  for (size_t i = 7; i < len; i++) {
    if (origin[i] == '/') return false;  // an origin has no path
  }
  return hostMatches(origin + 7, len - 7, ip, hostname);
}

void McpHeaders::reset() {
  host[0] = origin[0] = authorization[0] = protocolVersion[0] = '\0';
  authorizationLen = 0;
  contentLength = -1;
  hostSeen = originSeen = protocolSeen = false;
  hostTooLong = originTooLong = authorizationTooLong = protocolTooLong = false;
  contentLengthBad = chunked = multipart = false;
}

void McpHeaders::addLine(const char* line, size_t len) {
  size_t colon = 0;
  while (colon < len && line[colon] != ':') colon++;
  if (colon == len) return;
  size_t nameEnd = colon;
  while (nameEnd > 0 && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) nameEnd--;
  const char* value = line + colon + 1;
  size_t valueLen = len - colon - 1;
  while (valueLen && (*value == ' ' || *value == '\t')) {
    value++;
    valueLen--;
  }
  while (valueLen && (value[valueLen - 1] == ' ' || value[valueLen - 1] == '\t')) valueLen--;

  // Copies the value, or reports that it does not fit.
  auto store = [&](char* dst, size_t cap) {
    if (valueLen >= cap) {
      dst[0] = '\0';
      return false;
    }
    memcpy(dst, value, valueLen);
    dst[valueLen] = '\0';
    return true;
  };

  if (equalsIgnoreCase(line, nameEnd, "host")) {
    // A second Host header makes the request ambiguous: refuse it.
    hostTooLong = hostSeen || !store(host, sizeof(host));
    hostSeen = true;
  } else if (equalsIgnoreCase(line, nameEnd, "origin")) {
    originTooLong = originSeen || !store(origin, sizeof(origin));
    originSeen = true;
  } else if (equalsIgnoreCase(line, nameEnd, "authorization")) {
    authorizationTooLong = authorizationLen || !store(authorization, sizeof(authorization));
    authorizationLen = authorizationTooLong ? 0 : valueLen;
  } else if (equalsIgnoreCase(line, nameEnd, "mcp-protocol-version")) {
    protocolTooLong = protocolSeen || !store(protocolVersion, sizeof(protocolVersion));
    protocolSeen = true;
  } else if (equalsIgnoreCase(line, nameEnd, "content-length")) {
    long v = 0;
    bool ok = valueLen > 0 && valueLen <= 9;
    for (size_t i = 0; ok && i < valueLen; i++) {
      ok = value[i] >= '0' && value[i] <= '9';
      v = v * 10 + (value[i] - '0');
    }
    if (!ok || (contentLength >= 0 && contentLength != v)) {
      contentLengthBad = true;
    } else {
      contentLength = v;
    }
  } else if (equalsIgnoreCase(line, nameEnd, "transfer-encoding")) {
    chunked = true;
  } else if (equalsIgnoreCase(line, nameEnd, "content-type")) {
    multipart = multipart || (valueLen >= 10 && equalsIgnoreCase(value, 10, "multipart/"));
  }
}

void McpHeaders::addLines(const char* head, size_t len) {
  for (size_t start = 0; start < len;) {
    size_t stop = start;
    while (stop < len && head[stop] != '\r') stop++;
    addLine(head + start, stop - start);
    start = stop;
    while (start < len && head[start] != '\n') start++;
    start++;
  }
}

int checkBodyHeaders(const McpHeaders& h, size_t limit) {
  if (h.chunked || h.contentLengthBad) return 411;
  if (h.multipart) return 415;
  if (h.contentLength > 0 && static_cast<size_t>(h.contentLength) > limit) return 413;
  return 0;
}

int mcpCheckHostAndOrigin(const McpHeaders& h, const char* ip, const char* hostname) {
  if (!h.hostSeen || h.hostTooLong || !mcpHostAllowed(h.host, ip, hostname)) return 403;
  if (h.originSeen && (h.originTooLong || !mcpOriginAllowed(h.origin, ip, hostname))) return 403;
  return 0;
}

int mcpCheckRequest(const McpHeaders& h) {
  if (h.protocolSeen && (h.protocolTooLong || !isSupportedMcpVersion(h.protocolVersion))) return 400;
  if (h.contentLengthBad) return 400;
  if (h.chunked || h.contentLength < 0) return 411;
  if (static_cast<size_t>(h.contentLength) > kMcpMaxBodyBytes) return 413;
  return 0;
}

size_t writeMcpError(int code, const char* message, char* out, size_t outCap) {
  Out o(out, outCap);
  writeError(o, "null", code, message);
  return o.overflow ? 0 : o.len;
}

size_t writeMcpVersionError(char* out, size_t outCap) {
  // Not -32022: this server speaks the initialize-based revisions, and a dual-era client must read
  // this 400 as "fall back to initialize", not as a modern server's version error (FW-MCP-020).
  char message[96] = "Unsupported MCP-Protocol-Version (supported:";
  for (const char* v : kMcpVersions) {
    strcat(message, " ");
    strcat(message, v);
  }
  strcat(message, ")");
  return writeMcpError(kInvalidRequest, message, out, outCap);
}

McpReply handleMcpMessage(const char* body, size_t len, const char* firmwareVersion, McpDevice& device, uint8_t* arenaBuf,
                          size_t arenaLen, char* outBuf, size_t outCap) {
  Out out(outBuf, outCap);

  // FW-MCP-003: one message per POST; batches are refused before parsing.
  size_t first = 0;
  while (first < len && isSpace(body[first])) first++;
  if (first < len && body[first] == '[') {
    writeError(out, "null", kInvalidRequest, "Batch requests are not supported");
    return reply(400, out);
  }

  FixedArena arena(arenaBuf, arenaLen);
  JsonDocument doc(&arena);
  const DeserializationError err = deserializeJson(doc, body, len);
  if (err == DeserializationError::NoMemory) {
    writeError(out, "null", kServerError, "Request too large to process");
    return reply(413, out);
  }
  if (err) {  // FW-MCP-012
    writeError(out, "null", kParseError, "Parse error");
    return reply(400, out);
  }

  // Not a JSON-RPC 2.0 message at all: the transport answers 400 (like a batch, FW-MCP-003).
  JsonObjectConst msg = doc.as<JsonObjectConst>();
  JsonVariantConst method = msg["method"];
  if (msg.isNull() || msg["jsonrpc"] != "2.0") {
    writeError(out, "null", kInvalidRequest, "Invalid Request");
    return reply(400, out);
  }
  bool hasId = false;
  for (JsonPairConst kv : msg) hasId = hasId || kv.key() == "id";

  // FW-MCP-005: client responses and notifications are acknowledged and otherwise ignored. That
  // includes notifications/initialized, which is never required first (FW-MCP-022).
  if (method.isNull() && (!msg["result"].isNull() || !msg["error"].isNull())) return {202, 0};
  if (!method.is<const char*>()) {
    writeError(out, "null", kInvalidRequest, "Invalid Request");
    return reply(400, out);
  }
  if (!hasId) return {202, 0};

  // FW-MCP-027: string and integer ids are echoed as they came; nothing else is an id.
  JsonVariantConst idValue = msg["id"];
  char id[kMaxIdChars + 3];  // quotes and NUL
  if (!(idValue.is<long long>() || idValue.is<const char*>()) || measureJson(idValue) >= sizeof(id)) {
    writeError(out, "null", kInvalidRequest, "Invalid Request: id must be a string of up to 64 characters or an integer");
    return reply(400, out);
  }
  serializeJson(idValue, id, sizeof(id));

  const char* name = method.as<const char*>();
  JsonVariantConst params = msg["params"];

  if (strcmp(name, "initialize") == 0) {  // FW-MCP-021
    JsonVariantConst requested = params["protocolVersion"];
    if (!requested.is<const char*>()) {
      writeError(out, id, kInvalidParams, "Invalid params: protocolVersion must be a string");
      return reply(200, out);
    }
    const size_t mark = arena.mark();
    {
      JsonDocument result(&arena);
      result["protocolVersion"] = negotiateMcpVersion(requested.as<const char*>());
      result["capabilities"]["tools"].to<JsonObject>();
      result["serverInfo"]["name"] = "tubeclock";
      result["serverInfo"]["version"] = firmwareVersion;
      result["instructions"] = kInstructions;
      if (result.overflowed()) {
        writeError(out, id, kInternalError, "Internal error");
      } else {
        envelope(out, id);
        out.str(",\"result\":");
        out.json(result);
        out.str("}");
      }
    }
    arena.release(mark);
  } else if (strcmp(name, "ping") == 0) {  // FW-MCP-023
    envelope(out, id);
    out.str(",\"result\":{}}");
  } else if (strcmp(name, "tools/list") == 0) {  // FW-MCP-024: cursor ignored, no nextCursor
    envelope(out, id);
    out.str(",\"result\":");
    out.flash(kToolsList, sizeof(kToolsList) - 1);
    out.str("}");
  } else if (strcmp(name, "tools/call") == 0) {  // FW-MCP-025
    JsonVariantConst tool = params["name"];
    JsonVariantConst args = params["arguments"];
    if (!tool.is<const char*>()) {
      writeError(out, id, kInvalidParams, "Invalid params: name must be a string");
    } else if (!args.isNull() && !args.is<JsonObjectConst>()) {
      writeError(out, id, kInvalidParams, "Invalid params: arguments must be an object");
    } else if (strcmp(tool.as<const char*>(), "get_attention") == 0) {
      callGetAttention(args.as<JsonObjectConst>(), id, device, out);
    } else if (strcmp(tool.as<const char*>(), "cancel_attention") == 0) {  // FW-MCP-031
      device.cancelAttention();
      device.logToolCall("cancel_attention", "");
      writeText(out, id, "Attention cancelled.", false);
    } else if (strcmp(tool.as<const char*>(), "clock_status") == 0) {
      callClockStatus(id, device, arena, out);
    } else {  // FW-MCP-033: nothing else exists
      char loggable[48] = "";
      appendLoggable(loggable, sizeof(loggable), tool.as<const char*>());
      device.logToolCall(loggable, "refused: unknown tool");  // FW-MCP-013
      writeError(out, id, kInvalidParams, "Unknown tool");
    }
  } else {  // FW-MCP-026: resources/*, prompts/*, logging/*, completion/* and anything else
    writeError(out, id, kMethodNotFound, "Method not found");
  }

  if (out.overflow) {
    Out retry(outBuf, outCap);
    writeError(retry, id, kInternalError, "Response too large");
    return reply(200, retry);
  }
  return reply(200, out);
}

}  // namespace clock_core
