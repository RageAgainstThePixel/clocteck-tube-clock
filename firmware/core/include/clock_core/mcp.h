#pragma once

#include <cstddef>
#include <cstdint>

#include "clock_core/color.h"

// The on-device MCP server (spec/firmware/mcp.md, FW-MCP): Streamable HTTP in its simplest form,
// one JSON-RPC message per POST, JSON answers, no sessions. Everything here is plain C++ so it runs
// in the host unit tests; the sketch reads the HTTP request off the socket and writes the answer.
namespace clock_core {

constexpr size_t kMcpMaxBodyBytes = 2048;      // FW-BUD-004
constexpr size_t kMcpMaxResponseBytes = 3072;  // FW-BUD-005
constexpr size_t kMcpArenaBytes = 4096;        // FW-BUD-003: the JSON documents' memory, per request
constexpr size_t kMcpMaxMessageChars = 200;    // FW-MCP-030 `message`

/// FW-MCP-020: the protocol versions served, oldest first; the last one is the newest.
constexpr int kMcpVersionCount = 3;
extern const char* const kMcpVersions[kMcpVersionCount];
/// FW-MCP-007: what a request without an MCP-Protocol-Version header is taken to speak.
extern const char kMcpAssumedVersion[];

bool isSupportedMcpVersion(const char* version);
/// FW-MCP-021: the client's version when it is supported, else the newest supported one.
const char* negotiateMcpVersion(const char* requested);

/// FW-MCP-010: `host` (a Host header, any ":port" ignored) names this device: its IP, `hostname` or
/// `hostname`.local, case-insensitively.
bool mcpHostAllowed(const char* host, const char* ip, const char* hostname);
/// FW-MCP-011: `origin` is "http://" followed by an allowed host.
bool mcpOriginAllowed(const char* origin, const char* ip, const char* hostname);

/// The request headers the MCP endpoint looks at, collected one header line at a time into fixed
/// buffers. A value that does not fit is flagged rather than cut, so it can never match by accident.
struct McpHeaders {
  char host[80];
  char origin[96];
  char authorization[160];
  char protocolVersion[24];
  size_t authorizationLen;
  long contentLength;  // -1 when absent
  bool hostSeen, originSeen, protocolSeen;
  bool hostTooLong, originTooLong, authorizationTooLong, protocolTooLong;
  bool contentLengthBad, chunked, multipart;

  McpHeaders() { reset(); }
  void reset();
  /// One header line without its CR LF, e.g. "Host: tubeclock.local".
  void addLine(const char* line, size_t len);
  /// FW-SEC-013: a block of header lines, cut up the way the web server will read them (a line ends
  /// at a CR, and whatever follows up to the next LF is dropped), so a bare CR cannot hide a
  /// Content-Length from this check that the server would then obey.
  void addLines(const char* head, size_t len);
};

/// FW-SEC-013, FW-BUD-004: what the web server may read of a request. 0 when the body fits `limit`,
/// else 411 (a length that is chunked, malformed or contradicts itself), 413 (over the limit) or 415
/// (multipart: the server reads those by boundary, whatever the length says).
int checkBodyHeaders(const McpHeaders& headers, size_t limit);

/// FW-MCP-010, FW-MCP-011, FW-SEC-012: 0 when Host and Origin are acceptable, else 403. Runs before
/// the token check.
int mcpCheckHostAndOrigin(const McpHeaders& headers, const char* ip, const char* hostname);
/// After the token check: 0 when the request may be read, else 400 (FW-MCP-007: unsupported
/// MCP-Protocol-Version, or a malformed Content-Length), 411 (no length: chunked bodies are not read)
/// or 413 (FW-MCP-012: body over kMcpMaxBodyBytes).
int mcpCheckRequest(const McpHeaders& headers);

/// get_attention's arguments once validated.
struct AttentionCall {
  Hsv color;            // v as given; the clock shows it at v=100 and the saved brightness
  uint32_t durationMs;  // 1000-30000
  bool blink;
  bool beep;
};

/// What the MCP tools act on. The sketch implements it over the real Attention object and state.
class McpDevice {
 public:
  /// FW-MCP-035: replaces a flash that is showing (FW-ATT-006).
  virtual void startAttention(const AttentionCall& call) = 0;
  virtual void cancelAttention() = 0;
  /// The FW-API-030 state object as compact JSON into `out`; the length, or 0 when it did not fit.
  virtual size_t writeState(char* out, size_t cap) = 0;
  /// Whether a buzzer is wired up (TUBECLOCK_BUZZER_PIN), so the reply can say if a beep sounded.
  virtual bool hasBuzzer() const = 0;
  /// FW-MCP-013: the sketch prints "[mcp] <client ip> <tool> <summary>".
  virtual void logToolCall(const char* tool, const char* summary) = 0;

 protected:
  ~McpDevice() = default;
};

struct McpReply {
  int status;     // 200, 202 (empty body), 400 or 413
  size_t length;  // bytes written to `out`
};

/// Handles one POST body (FW-MCP-003..005, FW-MCP-020..036). `arena` is scratch memory for the JSON
/// documents (kMcpArenaBytes, FW-MCP-040); `out` receives the response body (kMcpMaxResponseBytes).
McpReply handleMcpMessage(const char* body, size_t len, const char* firmwareVersion, McpDevice& device, uint8_t* arena,
                          size_t arenaLen, char* out, size_t outCap);

/// A JSON-RPC error with `id: null`, for answers decided before the body is read (403, 400, 413).
size_t writeMcpError(int code, const char* message, char* out, size_t outCap);

/// FW-MCP-007: the 400 body for an unsupported MCP-Protocol-Version, naming the supported ones.
size_t writeMcpVersionError(char* out, size_t outCap);

}  // namespace clock_core
