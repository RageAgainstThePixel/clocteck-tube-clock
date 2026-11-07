// Dependency-free tests for the firmware core; run on the host with CTest.
// Each check cites the requirement it covers (spec/verification.md, VER-002).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>

#include "clock_core/attention.h"
#include "clock_core/auth.h"
#include "clock_core/backoff.h"
#include "clock_core/beep.h"
#include "clock_core/button.h"
#include "clock_core/color.h"
#include "clock_core/display.h"
#include "clock_core/effect.h"
#include "clock_core/json.h"
#include "clock_core/mcp.h"
#include "clock_core/pins.h"
#include "clock_core/rtc.h"
#include "clock_core/settings.h"
#include "clock_core/time_sync.h"
#include "clock_core/timefmt.h"

using namespace clock_core;

static int failures = 0;

// VER-019: counts the heap use of the code under test, so a frame can be shown not to allocate.
static int allocations = 0;
void* operator new(std::size_t size) {
  allocations++;
  void* p = std::malloc(size ? size : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                  \
    }                                                              \
  } while (0)

// FW-DSP-008
static void testHsvToRgb() {
  CHECK((hsvToRgb({0, 100, 100}) == Rgb{255, 0, 0}));
  CHECK((hsvToRgb({120, 100, 100}) == Rgb{0, 255, 0}));
  CHECK((hsvToRgb({240, 100, 100}) == Rgb{0, 0, 255}));
  CHECK((hsvToRgb({0, 0, 100}) == Rgb{255, 255, 255}));
  CHECK((hsvToRgb({360, 100, 100}) == Rgb{255, 0, 0}));  // FW-DSP-008 h=360 equals h=0
  CHECK((hsvToRgb({200, 50, 0}) == Rgb{0, 0, 0}));
}

// FW-DSP-004
static void testScale() {
  CHECK((scale({255, 128, 0}, 255) == Rgb{255, 128, 0}));
  CHECK((scale({255, 128, 0}, 0) == Rgb{0, 0, 0}));  // FW-DSP-009
  CHECK((scale({200, 100, 50}, 127) == Rgb{100, 50, 25}));
}

// FW-DSP-003
static void testTimeDigits() {
  auto d = timeDigits(13, 5, 9, true);
  CHECK(d[0] == 1 && d[1] == 3 && d[2] == 0 && d[3] == 5 && d[4] == 0 && d[5] == 9);
  d = timeDigits(13, 5, 9, false);
  CHECK(d[0] == 0 && d[1] == 1);  // FW-DSP-003 no leading-zero suppression
  d = timeDigits(0, 0, 0, false);
  CHECK(d[0] == 1 && d[1] == 2);  // FW-DSP-003 hour 0 shows as 12
  d = timeDigits(0, 0, 0, true);
  CHECK(d[0] == 0 && d[1] == 0);
  d = timeDigits(23, 59, 59, true);
  CHECK(d[0] == 2 && d[1] == 3 && d[2] == 5 && d[3] == 9 && d[4] == 5 && d[5] == 9);
}

// FW-TIME-004, FW-DSP-007
static void testTubeDigits() {
  auto d = tubeDigits(true, false, 13, 5, 9, true);
  CHECK(d[0] == 1 && d[1] == 3 && d[5] == 9);
  d = tubeDigits(false, false, 13, 5, 9, true);  // FW-DSP-007 blank while the time is unknown
  for (int8_t digit : d) CHECK(digit == kBlank);
  d = tubeDigits(false, true, 13, 5, 9, true);  // FW-DSP-007 attention lights digit 0
  for (int8_t digit : d) CHECK(digit == 0);
  d = tubeDigits(true, true, 13, 5, 9, true);  // FW-ATT-002 digits keep showing the time
  CHECK(d[0] == 1 && d[1] == 3);

  CHECK(!isTimeKnown(0, 1762000000));  // FW-TIME-004, FW-TIME-013 1970 is unknown
  CHECK(!isTimeKnown(1761999999, 1762000000));
  CHECK(isTimeKnown(1762000000, 1762000000));
}

// FW-DSP-001, FW-DSP-002
static void testLayoutAndRender() {
  LedLayout layout = LedLayout::ct001();
  CHECK(layout.isValid());  // FW-DSP-002
  CHECK(layout.index[0][0] == 40);  // FW-HW-005 leftmost digit 0 is silkscreen LED51
  CHECK(layout.index[0][9] == 49);
  CHECK(layout.index[1][0] == 50);  // FW-HW-005 the chain ends in the second tube
  CHECK(layout.index[1][9] == 59);
  CHECK(layout.index[2][7] == 32);  // FW-HW-005 tubes 2..5 run backward
  CHECK(layout.index[5][0] == 9);   // FW-HW-005 rightmost digit 0 is silkscreen LED1
  CHECK(layout.index[5][9] == 0);   // FW-HW-005 the chain starts at silkscreen LED10

  LedLayout broken = layout;
  broken.index[0][0] = broken.index[0][1];
  CHECK(!broken.isValid());  // FW-DSP-002 must be a permutation

  std::array<TubeState, kTubes> tubes{};
  for (int t = 0; t < kTubes; t++) tubes[t] = {static_cast<int8_t>(t), {255, 0, 0}};
  tubes[5].digit = kBlank;
  Frame frame = render(layout, tubes, 255);
  int lit = 0;
  for (const Rgb& px : frame) lit += (px.r || px.g || px.b) ? 1 : 0;
  CHECK(lit == 5);  // FW-DSP-001 one LED per tube, none for a blank tube
  CHECK((frame[layout.index[3][3]] == Rgb{255, 0, 0}));
  CHECK((frame[layout.index[3][4]] == Rgb{0, 0, 0}));
}

// FW-OTA-005
static void testUpdateProgress() {
  const LedLayout layout = LedLayout::ct001();
  const Rgb green{0, 255, 0};
  const auto digitOf = [&](const Frame& frame, int tube) {
    int found = -1;
    for (int d = 0; d < kDigitsPerTube; d++) {
      const Rgb& px = frame[layout.index[tube][d]];
      if (px.r || px.g || px.b) {
        CHECK(found < 0);  // FW-DSP-001 one LED per tube
        found = d;
      }
    }
    return found;
  };

  const auto colorOf = [&](const Frame& frame, int tube) { return frame[layout.index[tube][digitOf(frame, tube)]]; };

  // Scrambled tubes change digit and color with the seed, and always show one digit.
  std::array<bool, kDigitsPerTube> seen{};
  bool changed = false;
  int colors = 0;
  int last = -1;
  Rgb lastColor;
  for (uint32_t seed = 0; seed < 200; seed++) {
    const Frame frame = renderUpdateProgress(layout, 0, 600, seed, 255);
    for (int t = 0; t < kTubes; t++) CHECK(digitOf(frame, t) >= 0);
    const int digit = digitOf(frame, 3);
    seen[digit] = true;
    if (last >= 0 && digit != last) changed = true;
    last = digit;
    const Rgb color = colorOf(frame, 3);
    if (!(color == lastColor)) colors++;
    lastColor = color;
  }
  CHECK(changed);
  CHECK(colors > 100);           // FW-OTA-005 the colors keep changing
  for (bool s : seen) CHECK(s);  // FW-OTA-005 reaches every digit

  // Tubes lock on a green 0 from the left, one per sixth of the image, whatever the seed.
  for (uint32_t seed = 0; seed < 50; seed++) {
    const Frame half = renderUpdateProgress(layout, 300, 600, seed, 255);
    for (int t = 0; t < 3; t++) CHECK(digitOf(half, t) == 0 && colorOf(half, t) == green);
    const Frame almost = renderUpdateProgress(layout, 599, 600, seed, 255);
    for (int t = 0; t < 5; t++) CHECK(digitOf(almost, t) == 0 && colorOf(almost, t) == green);
    const Frame full = renderUpdateProgress(layout, 600, 600, seed, 255);
    for (int t = 0; t < kTubes; t++) CHECK(digitOf(full, t) == 0 && colorOf(full, t) == green);
  }
  CHECK(digitOf(renderUpdateProgress(layout, 0, 0, 1, 255), 0) >= 0);  // no size yet: nothing locked, no crash
  CHECK(digitOf(renderUpdateProgress(layout, 0xFFFFFFFFu, 0xFFFFFFFFu, 1, 255), 5) == 0);  // no overflow
}

// FW-SEC-005, FW-SEC-006, FW-SEC-009
static void testBearer() {
  CHECK(bearerMatches("Bearer s3cret", "s3cret"));
  CHECK(!bearerMatches("Bearer s3cre", "s3cret"));
  CHECK(!bearerMatches("Bearer s3cretX", "s3cret"));
  CHECK(!bearerMatches("bearer s3cret", "s3cret"));
  CHECK(!bearerMatches("s3cret", "s3cret"));
  CHECK(!bearerMatches("Bearer ", ""));  // FW-SEC-009 empty token rejects everything
  CHECK(!bearerMatches(nullptr, "s3cret"));
  // FW-SEC-006: a cleared ESP8266 String keeps its old bytes; only the length says it is empty.
  const char stale[] = "Bearer s3cret";
  CHECK(!bearerMatches(stale, 0, "s3cret"));  // FW-SEC-006
  CHECK(!bearerMatches(stale, 10, "s3cret"));  // FW-SEC-006
  CHECK(bearerMatches(stale, sizeof(stale) - 1, "s3cret"));
  CHECK(constantTimeEquals("abc", 3, "abc", 3));  // FW-SEC-006
  CHECK(!constantTimeEquals("abc", 3, "abd", 3));
}

// FW-SEC-014
static void testBackoff() {
  AuthBackoff b;
  const uint32_t ip = 0x0a00000a;
  for (int i = 0; i < 5; i++) CHECK(b.onFailure(ip, 1000 + i) == 0);  // FW-SEC-014 first 5 are free
  CHECK(b.onFailure(ip, 2000) == AuthBackoff::kDelayMs);              // FW-SEC-014 6th waits 1 s
  CHECK(b.onFailure(ip, 3000) == AuthBackoff::kDelayMs);
  CHECK(b.onFailure(0x0a00000b, 3000) == 0);  // FW-SEC-014 per IP

  b.onSuccess(ip);  // FW-SEC-014 a good token clears the count
  CHECK(b.onFailure(ip, 4000) == 0);

  for (int i = 0; i < 5; i++) b.onFailure(ip, 5000);
  CHECK(b.onFailure(ip, 5000 + 59999) == AuthBackoff::kDelayMs);    // still within 60 s of the last
  CHECK(b.onFailure(ip, 5000 + 59999 + 60000) == 0);                // FW-SEC-014 resets after 60 s

  // FW-SEC-014 at most 4 IPs; the oldest is evicted and starts over.
  AuthBackoff t;
  for (int i = 0; i < 6; i++) t.onFailure(1, 100);
  for (uint32_t other = 2; other <= 4; other++) t.onFailure(other, 200);
  t.onFailure(5, 300);                       // evicts IP 1
  CHECK(t.onFailure(1, 400) == 0);           // IP 1 starts over
  CHECK(t.onFailure(5, 500) == 0);           // IP 5 kept its slot (2 failures)

  // FW-SEC-020 millis() wrap does not break the window
  AuthBackoff w;
  for (int i = 0; i < 5; i++) w.onFailure(ip, 0xFFFFFF00u);
  CHECK(w.onFailure(ip, 0x00000100u) == AuthBackoff::kDelayMs);
}

// FW-ATT-001..006, FW-ATT-009
static void testAttention() {
  std::array<Hsv, kTubes> base{};
  base.fill(Hsv{241, 100, 100});
  Attention a;

  auto colors = base;
  CHECK(a.apply(colors, 0));
  CHECK(colors[0].h == 241);

  a.start({30, 100, 100}, 1000, 5000, false);  // FW-ATT-001
  colors = base;
  CHECK(a.apply(colors, 1000));
  CHECK(colors[0].h == 30 && colors[5].h == 30);  // FW-ATT-002 every tube
  CHECK(a.isActive(5999));                        // FW-ATT-009
  CHECK(!a.isActive(6000));                       // FW-ATT-004 ends exactly at durationMs
  colors = base;
  CHECK(a.apply(colors, 6000));
  CHECK(colors[0].h == 241);  // FW-ATT-002 saved colors untouched

  a.start({0, 100, 100}, 0, 3000, true);  // FW-ATT-003 blink, lit first
  colors = base;
  CHECK(a.apply(colors, 100));
  CHECK(!a.apply(colors, 600));
  CHECK(a.apply(colors, 1100));

  a.start({0, 100, 100}, 0, 999999, false);  // FW-ATT-001 clamped to 30 s
  CHECK(a.isActive(Attention::kMaxDurationMs - 1));
  CHECK(!a.isActive(Attention::kMaxDurationMs));

  a.start({0, 100, 100}, 0xFFFFF000u, 5000, false);  // FW-ATT-004 millis() rollover
  CHECK(a.isActive(0x00000100u));
  CHECK(!a.isActive(0x00001000u));

  // FW-ATT-004: once the loop has seen a flash end, millis() coming round again does not replay it.
  a.start({0, 100, 100}, 1000, 5000, false);
  a.expire(3000);
  CHECK(a.isActive(3000));  // FW-ATT-004 expire leaves a running flash alone
  a.expire(6000);
  CHECK(!a.isActive(1000));  // FW-ATT-004 the same millis() value, one rollover later
  CHECK(!a.isActive(3000));

  BeepPattern replay;
  replay.start(1000);
  replay.expire(1000 + BeepPattern::kLengthMs - 1);
  CHECK(replay.isActive(1000 + BeepPattern::kLengthMs - 1));
  replay.expire(1000 + BeepPattern::kLengthMs);
  CHECK(!replay.isOn(1000));  // FW-ATT-010 no pulse one rollover later

  a.start({0, 100, 100}, 0, 5000, false);
  a.cancel();  // FW-ATT-005
  CHECK(!a.isActive(10));
  a.cancel();  // FW-ATT-005 no-op when idle
  CHECK(!a.isActive(10));

  a.start({0, 100, 100}, 0, 0, false);  // FW-ATT-001 durationMs = 0 starts nothing
  CHECK(!a.isActive(0));

  a.start({30, 100, 100}, 0, 5000, false);
  a.start({90, 100, 100}, 100, 5000, false);  // FW-ATT-006 replaces, new timer
  colors = base;
  a.apply(colors, 5050);
  CHECK(colors[0].h == 90);
}

// FW-SET-001, FW-SET-002, FW-SET-004, FW-SET-005
static void testSettingsRoundTrip() {
  Settings s = Settings::defaults();
  CHECK(s.tubes[0].h == 241 && s.tubes[0].s == 100 && s.brightness == 99 && !s.is24Hour);  // FW-SET-002
  CHECK(std::strcmp(s.tz, "EST5EDT,M3.2.0,M11.1.0") == 0);                                 // FW-SET-002
  s.tubes[2] = {360, 50, 100};
  s.brightness = 200;
  s.is24Hour = true;
  std::strcpy(s.tz, "UTC0");

  uint8_t blob[kSettingsBlobSize];
  encodeSettings(s, blob);
  CHECK(blob[0] == 'T' && blob[1] == 'C' && blob[2] == 1);  // FW-SET-004
  Settings out;
  CHECK(decodeSettings(blob, out));
  CHECK(out.tubes[2].h == 360 && out.tubes[2].s == 50);
  CHECK(out.tubes[0].h == 241);
  CHECK(out.brightness == 200 && out.is24Hour);
  CHECK(std::strcmp(out.tz, "UTC0") == 0);

  uint8_t corrupt[kSettingsBlobSize];
  std::memcpy(corrupt, blob, sizeof(blob));
  corrupt[10] ^= 1;
  CHECK(!decodeSettings(corrupt, out));  // FW-SET-005 checksum mismatch

  uint8_t blank[kSettingsBlobSize];
  std::memset(blank, 0xff, sizeof(blank));
  CHECK(!decodeSettings(blank, out));  // FW-SET-005 wrong magic
}

// FW-SET-007
static void testSameSettings() {
  const Settings a = Settings::defaults();
  Settings b = a;
  CHECK(sameSettings(a, b));  // FW-SET-007 identical write: no commit
  b.brightness = 100;
  CHECK(!sameSettings(a, b));
  b = a;
  b.tubes[5].s = 99;
  CHECK(!sameSettings(a, b));
  b = a;
  std::strcpy(b.tz, "UTC0");
  CHECK(!sameSettings(a, b));
  b = a;
  b.tubes[0].v = 50;  // not stored, so not a change
  CHECK(sameSettings(a, b));
}

// FW-SET-011, FW-SET-012, FW-API-001
static void testValidation() {
  CHECK(isValidTz("EST5EDT,M3.2.0,M11.1.0"));
  CHECK(isValidTz("<+0530>-5:30"));
  CHECK(!isValidTz(""));
  CHECK(!isValidTz("EST 5"));
  CHECK(!isValidTz("EST\"5"));
  CHECK(!isValidTz("012345678901234567890123456789012345678901234567"));  // FW-SET-011 48 chars

  long v = 0;
  CHECK(parseInt("241", 0, 360, v) && v == 241);
  CHECK(parseInt("-5", -12, 14, v) && v == -5);
  CHECK(!parseInt("361", 0, 360, v));
  CHECK(!parseInt("", 0, 360, v));    // FW-API-001 empty
  CHECK(!parseInt("-", 0, 360, v));   // FW-API-001 sign only
  CHECK(!parseInt("12a", 0, 360, v)); // FW-API-001 junk
  CHECK(!parseInt("99999999999999999999", 0, 360, v));  // FW-API-001 overflow
  CHECK(!parseInt(nullptr, 0, 360, v));
}

// FW-TIME-010
static void testNtpServers() {
  static_assert(ntpServerCount("time.cloudflare.com,time.google.com,time.nist.gov") == 3, "FW-TIME-010");
  static_assert(ntpServerCount("ntp.lan") == 1, "FW-TIME-010");
  char names[kMaxNtpServers][kMaxNtpNameLen + 1];
  CHECK(splitNtpServers("time.cloudflare.com,time.google.com,time.nist.gov", names) == 3);
  CHECK(std::strcmp(names[0], "time.cloudflare.com") == 0 && std::strcmp(names[2], "time.nist.gov") == 0);
  CHECK(splitNtpServers("192.168.1.1", names) == 1 && std::strcmp(names[0], "192.168.1.1") == 0);
  CHECK(splitNtpServers("a,b,c,d", names) == 0);  // FW-TIME-010 at most 3
  CHECK(splitNtpServers("a,,c", names) == 0);
  CHECK(splitNtpServers("a,", names) == 0);
  CHECK(splitNtpServers("", names) == 0);
  CHECK(splitNtpServers(nullptr, names) == 0);
  char longName[kMaxNtpNameLen + 2];
  std::memset(longName, 'x', sizeof(longName) - 1);
  longName[sizeof(longName) - 1] = '\0';
  CHECK(splitNtpServers(longName, names) == 0);
}

// FW-TIME-013, FW-TIME-014 (VER-016)
static void testJudgeSync() {
  const int64_t build = 1762000000LL * kUsPerSec;
  const int64_t hour = 3600LL * kUsPerSec;
  const int64_t now = build + 240 * hour;
  auto is = [](SyncDecision d, SyncVerdict v, SyncReason r) { return d.verdict == v && d.reason == r; };

  // FW-TIME-013 below the build epoch: rejected, RTC or not
  CHECK(is(judgeSync(build - 1, now, build, false, nullptr), SyncVerdict::kReject, SyncReason::kBeforeBuild));
  CHECK(is(judgeSync(build - 1, now, build, true, nullptr), SyncVerdict::kReject, SyncReason::kBeforeBuild));
  // FW-TIME-014 no RTC: anything after the build is accepted
  CHECK(is(judgeSync(now + 1000 * hour, 0, build, false, nullptr), SyncVerdict::kAccept, SyncReason::kNone));
  // FW-TIME-014 RTC valid and a jump of at most 24 h: accepted as drift
  CHECK(is(judgeSync(now + 24 * hour, now, build, true, nullptr), SyncVerdict::kAccept, SyncReason::kNone));
  CHECK(is(judgeSync(now - 24 * hour + kUsPerSec, now, build, true, nullptr), SyncVerdict::kAccept, SyncReason::kNone));
  // FW-TIME-014 over 24 h once: needs a second answer
  const int64_t jump = now + 72 * hour;
  CHECK(is(judgeSync(jump, now, build, true, nullptr), SyncVerdict::kNeedConfirmation, SyncReason::kJump));
  // FW-TIME-014 second answer within 10 s of the first: accepted
  const int64_t pending = jump;
  CHECK(is(judgeSync(jump + 10 * kUsPerSec, now, build, true, &pending), SyncVerdict::kAccept, SyncReason::kConfirmed));
  CHECK(is(judgeSync(jump - 9 * kUsPerSec, now, build, true, &pending), SyncVerdict::kAccept, SyncReason::kConfirmed));
  // FW-TIME-014 second answer elsewhere: rejected (the caller replaces the pending value)
  CHECK(is(judgeSync(jump + 11 * kUsPerSec, now, build, true, &pending), SyncVerdict::kReject, SyncReason::kDisagree));
  CHECK(std::strcmp(syncReasonName(SyncReason::kBeforeBuild), "before-build") == 0);  // FW-TIME-015 log reason
}

// FW-TIME-006, FW-TIME-014, FW-TIME-015, FW-TIME-016
static void testTimeSync() {
  const int64_t buildSec = 1762000000;
  const int64_t build = buildSec * kUsPerSec;
  const int64_t day = 24LL * 3600 * kUsPerSec;

  // No RTC: the clock starts in 1970; the first answer is accepted and recorded.
  TimeSync a(buildSec);
  CHECK(!a.begin(0, 5 * kUsPerSec, nullptr));
  CHECK(!a.hasSynced() && a.rejectedSyncs() == 0);  // FW-TIME-016 lastSync null, 0 rejected
  CHECK(a.expectedUs(7 * kUsPerSec) == 2 * kUsPerSec);
  CHECK(a.onAnswer(build + day, 8 * kUsPerSec).verdict == SyncVerdict::kAccept);
  CHECK(a.hasSynced() && a.lastSyncUs() == build + day && a.retryDelayMs() == 0);  // FW-TIME-016
  CHECK(a.expectedUs(9 * kUsPerSec) == build + day + kUsPerSec);
  // FW-TIME-015 a rejected answer leaves the expected time (what the clock is put back to) alone
  CHECK(a.onAnswer(build - 1, 10 * kUsPerSec).verdict == SyncVerdict::kReject);
  CHECK(a.expectedUs(10 * kUsPerSec) == build + day + 2 * kUsPerSec);
  CHECK(a.rejectedSyncs() == 1 && a.lastSyncUs() == build + day);  // FW-TIME-016
  CHECK(a.retryDelayMs() == TimeSync::kRetryMinMs);

  // FW-TIME-006 an RTC time from before the build is not used
  const int64_t stale = build - day;
  TimeSync old(buildSec);
  CHECK(!old.begin(0, 0, &stale));
  CHECK(!old.rtcValid());

  // FW-TIME-014 RTC valid at boot, then a 3-day jump: first rejected, second agreeing one accepted.
  const int64_t rtc = build + 10 * day;
  TimeSync b(buildSec);
  CHECK(b.begin(0, 0, &rtc) && b.rtcValid());
  CHECK(b.expectedUs(kUsPerSec) == rtc + kUsPerSec);
  const int64_t real = rtc + 3 * day;  // the RTC had stopped for 3 days
  CHECK(b.onAnswer(real + 2 * kUsPerSec, 2 * kUsPerSec).verdict == SyncVerdict::kNeedConfirmation);
  CHECK(b.expectedUs(2 * kUsPerSec) == rtc + 2 * kUsPerSec);  // FW-TIME-015 undo target
  CHECK(b.rejectedSyncs() == 1 && !b.hasSynced());
  // 15 s later the next server answers 15 s further on: the pending value ages with the clock.
  CHECK(b.onAnswer(real + 17 * kUsPerSec + 3 * kUsPerSec, 17 * kUsPerSec).verdict == SyncVerdict::kAccept);
  CHECK(b.hasSynced() && b.rejectedSyncs() == 1);

  // FW-TIME-014 two disagreeing jumps: both rejected, the second replaces the pending value.
  TimeSync c(buildSec);
  c.begin(0, 0, &rtc);
  CHECK(c.onAnswer(rtc + 3 * day, kUsPerSec).verdict == SyncVerdict::kNeedConfirmation);
  CHECK(c.onAnswer(rtc + 5 * day, 2 * kUsPerSec).reason == SyncReason::kDisagree);
  CHECK(c.retryDelayMs() == 2 * TimeSync::kRetryMinMs);
  CHECK(c.onAnswer(rtc + 5 * day + kUsPerSec, 3 * kUsPerSec).reason == SyncReason::kConfirmed);
  CHECK(c.rejectedSyncs() == 2);

  // Retry back-off doubles and stops at the hourly resync (FW-TIME-011).
  TimeSync d(buildSec);
  d.begin(0, 0, nullptr);
  for (int i = 0; i < 20; i++) d.onAnswer(0, 0);
  CHECK(d.retryDelayMs() == TimeSync::kResyncMs);

  // FW-TIME-006, FW-API-037 where the time came from; a rejected answer changes nothing
  TimeSync e(buildSec);
  e.begin(0, 0, nullptr);
  CHECK(e.source() == TimeSource::kNone && strcmp(timeSourceName(e.source()), "none") == 0);
  e.onAnswer(build - 1, kUsPerSec);
  CHECK(e.source() == TimeSource::kNone);
  e.onAnswer(build + day, 2 * kUsPerSec);
  CHECK(e.source() == TimeSource::kNtp && strcmp(timeSourceName(e.source()), "ntp") == 0);
  TimeSync f(buildSec);
  f.begin(0, 0, &rtc);
  CHECK(f.source() == TimeSource::kRtc && strcmp(timeSourceName(f.source()), "rtc") == 0);
  f.onAnswer(rtc + 3 * day, kUsPerSec);  // FW-TIME-015 needs confirming, so still the RTC
  CHECK(f.source() == TimeSource::kRtc);
  f.onAnswer(rtc + kUsPerSec, 2 * kUsPerSec);
  CHECK(f.source() == TimeSource::kNtp);
  TimeSync g(buildSec);
  g.begin(0, 0, &stale);  // FW-TIME-013 too old to believe: no source
  CHECK(g.source() == TimeSource::kNone);
}

// FW-TIME-006: the GC1307's registers, both ways
static void testRtc() {
  // 2026-11-03 17:04:12 UTC, a Tuesday
  const int64_t sample = 1793725452LL * kUsPerSec;
  const uint8_t good[kRtcRegisterCount] = {0x12, 0x04, 0x17, 0x03, 0x03, 0x11, 0x26};
  int64_t us = 0;
  CHECK(decodeRtc(good, us) == RtcStatus::kOk && us == sample);

  uint8_t regs[kRtcRegisterCount];
  CHECK(encodeRtc(sample, regs));
  CHECK(memcmp(regs, good, sizeof(regs)) == 0);
  CHECK(!(regs[0] & 0x80) && !(regs[2] & 0x40));  // running, 24-hour mode

  // Rounds to the nearest second.
  CHECK(encodeRtc(sample + 499999, regs) && regs[0] == 0x12);
  CHECK(encodeRtc(sample + 500000, regs) && regs[0] == 0x13);
  CHECK(encodeRtc(1793725499LL * kUsPerSec + 900000, regs) && regs[0] == 0x00 && regs[1] == 0x05);  // carries

  // Round trips over the range: the ends, leap days, month and year ends, a new year's eve.
  for (int64_t sec : {946684800LL, 951782400LL, 951868799LL, 1709164800LL, 1709251199LL, 1709251200LL,
                      1735689599LL, 1735689600LL, 4102444799LL}) {
    CHECK(encodeRtc(sec * kUsPerSec, regs));
    CHECK(decodeRtc(regs, us) == RtcStatus::kOk && us == sec * kUsPerSec);
  }
  CHECK(encodeRtc(951782400LL * kUsPerSec, regs) && regs[4] == 0x29 && regs[5] == 0x02 && regs[6] == 0x00);  // 2000-02-29
  CHECK(encodeRtc(946684800LL * kUsPerSec, regs) && regs[3] == 7);  // 2000-01-01, a Saturday; 1 = Sunday

  // Outside what the chip can hold.
  CHECK(!encodeRtc(-1, regs));
  CHECK(!encodeRtc(946684799LL * kUsPerSec, regs));
  CHECK(!encodeRtc(4102444800LL * kUsPerSec, regs));

  // The clock-halt bit, whatever else the registers say. Power-up is 2000-01-01 with it set.
  uint8_t bad[kRtcRegisterCount];
  auto with = [&](size_t reg, uint8_t value) {
    memcpy(bad, good, sizeof(bad));
    bad[reg] = value;
    return decodeRtc(bad, us);
  };
  CHECK(with(0, 0x80 | 0x12) == RtcStatus::kHalted);
  const uint8_t fresh[kRtcRegisterCount] = {0x80, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00};
  CHECK(decodeRtc(fresh, us) == RtcStatus::kHalted);

  // Out of range or not BCD. A floating bus reads all ones.
  const uint8_t floating[kRtcRegisterCount] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(decodeRtc(floating, us) == RtcStatus::kHalted);
  CHECK(with(0, 0x60) == RtcStatus::kBadValue);  // second 60
  CHECK(with(0, 0x1A) == RtcStatus::kBadValue);  // not BCD
  CHECK(with(1, 0x60) == RtcStatus::kBadValue);
  CHECK(with(1, 0x80) == RtcStatus::kBadValue);  // a reserved bit
  CHECK(with(2, 0x24) == RtcStatus::kBadValue);  // 24:00
  CHECK(with(2, 0x80) == RtcStatus::kBadValue);
  CHECK(with(2, 0x40) == RtcStatus::kBadValue);  // hour 0 in 12-hour mode
  CHECK(with(2, 0x53) == RtcStatus::kBadValue);  // hour 13 in 12-hour mode
  CHECK(with(4, 0x00) == RtcStatus::kBadValue);
  CHECK(with(4, 0x32) == RtcStatus::kBadValue);
  CHECK(with(5, 0x00) == RtcStatus::kBadValue);
  CHECK(with(5, 0x13) == RtcStatus::kBadValue);
  CHECK(with(6, 0xA0) == RtcStatus::kBadValue);
  const uint8_t feb29[kRtcRegisterCount] = {0x00, 0x00, 0x00, 0x01, 0x29, 0x02, 0x25};  // not a leap year
  CHECK(decodeRtc(feb29, us) == RtcStatus::kBadValue);
  const uint8_t leap[kRtcRegisterCount] = {0x00, 0x00, 0x00, 0x01, 0x29, 0x02, 0x24};
  CHECK(decodeRtc(leap, us) == RtcStatus::kOk && us == 1709164800LL * kUsPerSec);

  // The stock firmware may have left 12-hour mode: 12 AM is 0, 12 PM is 12, 5 PM is 17.
  CHECK(with(2, 0x40 | 0x12) == RtcStatus::kOk && us == sample - 17 * 3600 * kUsPerSec);
  CHECK(with(2, 0x60 | 0x12) == RtcStatus::kOk && us == sample - 5 * 3600 * kUsPerSec);
  CHECK(with(2, 0x60 | 0x05) == RtcStatus::kOk && us == sample);
  // The day of week is not looked at: it costs no good time at boot.
  CHECK(with(3, 0x00) == RtcStatus::kOk && us == sample);
  CHECK(with(3, 0xFF) == RtcStatus::kOk && us == sample);

  // A chip left halted, or with a time from before the build, gives the clock no time (FW-TIME-013).
  CHECK(with(6, 0x20) == RtcStatus::kOk);  // 2020 decodes fine...
  TimeSync sync(1762000000);
  CHECK(!sync.begin(0, 0, &us) && !sync.rtcValid());  // ...and is too old to believe
}

// FW-HW-006: debounce and press edges, FW-ATT-011: a press ends a flash
static void testButtons() {
  ButtonDebouncer b;
  CHECK(!b.update(false, 0) && !b.isDown());

  // Has to hold for 30 ms, whatever the loop's pace.
  CHECK(!b.update(true, 100));
  CHECK(!b.update(true, 129) && !b.isDown());
  CHECK(b.update(true, 130) && b.isDown());
  CHECK(!b.update(true, 131));  // once per press
  CHECK(!b.update(true, 10000));
  CHECK(!b.update(false, 10001) && b.isDown());  // a release debounces too, and is no press
  CHECK(!b.update(false, 10031) && !b.isDown());

  // Bounce restarts the wait; a glitch shorter than 30 ms is nothing.
  ButtonDebouncer bouncy;
  CHECK(!bouncy.update(true, 0));
  CHECK(!bouncy.update(false, 5));
  CHECK(!bouncy.update(true, 10));
  CHECK(!bouncy.update(false, 25));
  CHECK(!bouncy.update(false, 100) && !bouncy.isDown());
  CHECK(!bouncy.update(true, 110));
  CHECK(!bouncy.update(true, 139));
  CHECK(bouncy.update(true, 140));

  // Slow polling (a long handler): the first reading that shows the change starts the wait.
  ButtonDebouncer slow;
  CHECK(!slow.update(true, 1000));
  CHECK(slow.update(true, 1200));

  // Across the millis() rollover.
  ButtonDebouncer wrap;
  const uint32_t t0 = 0xFFFFFFF0u;
  CHECK(!wrap.update(true, t0));
  CHECK(!wrap.update(true, t0 + 29));
  CHECK(wrap.update(true, t0 + 30));

  // FW-ATT-011: the sketch's rule. A press ends a running flash, and does nothing without one.
  Attention flash;
  ButtonDebouncer button;
  auto poll = [&](bool down, uint32_t nowMs) {
    if (button.update(down, nowMs) && flash.isActive(nowMs)) {
      flash.cancel();
      return true;
    }
    return false;
  };
  CHECK(!poll(true, 0));
  CHECK(!poll(true, 30));  // a press with no flash: nothing to cancel
  flash.start({0, 100, 100}, 40, 5000, false);
  CHECK(!poll(true, 50) && flash.isActive(50));  // held since before the flash: not a new press
  CHECK(!poll(false, 60));
  CHECK(!poll(false, 90));
  CHECK(!poll(true, 100));
  CHECK(!poll(false, 110) && flash.isActive(110));  // a glitch does not end it
  CHECK(!poll(false, 150));
  CHECK(!poll(true, 200));
  CHECK(poll(true, 230) && !flash.isActive(230));
}

// FW-HW-002, FW-HW-004: which GPIOs a build may name, and what the pin-finder walks
static void testPins() {
  for (int gpio = -1; gpio <= 17; gpio++) {
    const bool flash = (gpio >= 6 && gpio <= 8) || gpio == 11;
    CHECK(isUsableGpio(gpio) == (gpio >= 0 && gpio <= 16 && !flash));  // FW-HW-002 DOUT leaves GPIO 9 and 10 free
    if (gpio == 0 || gpio == 2 || gpio == 9 || gpio == 10 || gpio == 15 || flash) CHECK(!pinFinderMayDrive(gpio));  // FW-HW-004
    if (gpio == 1 || gpio == 3 || gpio == 16 || gpio < 0 || gpio > 16) CHECK(!pinFinderMayDrive(gpio));
  }
  CHECK(pinFinderMayDrive(4) && pinFinderMayDrive(5) && pinFinderMayDrive(12) && pinFinderMayDrive(13) && pinFinderMayDrive(14));
  for (int gpio : kPinFinderPins) CHECK(pinFinderMayDrive(gpio));
  static_assert(pinFinderStep(0, kPinFinderPinCount).led == 0, "constexpr, so the sketch can use it at compile time");

  // Every step: a pin of the list, each pin's walk covering every LED once, and then it starts over.
  bool seen[kPinFinderPinCount][kLedCount] = {};
  int colorSteps[kPinFinderPinCount][kPinFinderColorSteps] = {};
  const uint32_t total = static_cast<uint32_t>(kPinFinderPinCount) * kPinFinderStepsPerPin;
  for (uint32_t step = 0; step < total; step++) {
    const PinFinderStep s = pinFinderStep(step, kPinFinderPinCount);
    CHECK(s.pinSlot >= 0 && s.pinSlot < kPinFinderPinCount && s.led >= 0 && s.led < kLedCount);
    CHECK(pinFinderMayDrive(kPinFinderPins[s.pinSlot]));
    if (s.onlyByte < 0) {
      CHECK(!seen[s.pinSlot][s.led]);
      seen[s.pinSlot][s.led] = true;
    } else {
      CHECK(s.led == 0 && s.onlyByte < kPinFinderColorSteps);  // the color order is read off LED 0
      colorSteps[s.pinSlot][s.onlyByte]++;
    }
  }
  for (int p = 0; p < kPinFinderPinCount; p++) {
    for (int i = 0; i < kLedCount; i++) CHECK(seen[p][i]);
    for (int i = 0; i < kPinFinderColorSteps; i++) CHECK(colorSteps[p][i] == 1);
  }
  const PinFinderStep again = pinFinderStep(total + 3, kPinFinderPinCount);
  CHECK(again.pinSlot == 0 && again.led == 0 && again.onlyByte == -1);
  // Stepping far past the end stays inside the list, and one named pin stays that pin.
  CHECK(pinFinderStep(0xFFFFFFFFu, kPinFinderPinCount).pinSlot < kPinFinderPinCount);
  CHECK(pinFinderStep(70, 1).pinSlot == 0 && pinFinderStep(70, 1).led == 4);
}

// FW-TIME-015 which server answered, for the log line
static void testAnsweringServer() {
  const uint8_t before[3] = {0x00, 0x00, 0x00};
  const uint8_t one[3] = {0x00, 0x01, 0x00};
  CHECK(answeringServer(before, one, 3, -1) == 1);
  const uint8_t steady[3] = {0xff, 0x00, 0x00};
  CHECK(answeringServer(steady, steady, 3, 0) == 0);  // a server that always answers stays 0xff
  const uint8_t was[3] = {0xff, 0x00, 0x07};
  const uint8_t now[3] = {0xff, 0x00, 0x0f};
  CHECK(answeringServer(was, now, 3, 0) == 2);  // the register that changed wins
  CHECK(answeringServer(steady, steady, 3, 2) == 0);
  const uint8_t none[3] = {0x00, 0x02, 0x00};
  CHECK(answeringServer(before, none, 3, -1) == -1);
}

// FW-TIME-017 (VER-022): no RTC, so the last accepted answer is the only reference.
static void testLastSyncReference() {
  const int64_t buildSec = 1762000000;
  const int64_t build = buildSec * kUsPerSec;
  const int64_t hour = 3600LL * kUsPerSec;
  const int64_t day = 24 * hour;

  TimeSync a(buildSec);
  a.begin(0, 0, nullptr);
  const int64_t first = build + 10 * day;
  CHECK(a.onAnswer(first, 0).verdict == SyncVerdict::kAccept);  // FW-TIME-017 first answer: only the floor
  // 1 h of uptime later, an answer 3 days off (first + elapsed): needs a second one.
  const uint64_t t1 = hour;
  const SyncDecision jump = a.onAnswer(first + hour + 3 * day, t1);
  CHECK(jump.verdict == SyncVerdict::kNeedConfirmation && jump.reason == SyncReason::kJump);  // FW-TIME-017
  CHECK(a.rejectedSyncs() == 1 && a.lastSyncUs() == first);
  CHECK(a.expectedUs(t1) == first + hour);  // FW-TIME-015 undo target is unmoved
  // A third that agrees with the second within 10 s: accepted.
  const uint64_t t2 = t1 + 15 * kUsPerSec;
  const SyncDecision agreed = a.onAnswer(first + hour + 3 * day + 15 * kUsPerSec + 9 * kUsPerSec, t2);
  CHECK(agreed.verdict == SyncVerdict::kAccept && agreed.reason == SyncReason::kConfirmed);  // FW-TIME-017
  CHECK(a.rejectedSyncs() == 1 && a.retryDelayMs() == 0);

  // A third that agrees with the expected time instead: accepted, and the pending value is dropped.
  TimeSync b(buildSec);
  b.begin(0, 0, nullptr);
  CHECK(b.onAnswer(first, 0).verdict == SyncVerdict::kAccept);
  CHECK(b.onAnswer(first + hour + 3 * day, t1).verdict == SyncVerdict::kNeedConfirmation);
  const SyncDecision back = b.onAnswer(first + hour + 15 * kUsPerSec, t2);
  CHECK(back.verdict == SyncVerdict::kAccept && back.reason == SyncReason::kNone);  // FW-TIME-017
  // With the pending value gone, the same jump again starts over rather than confirming.
  CHECK(b.onAnswer(first + hour + 3 * day + 30 * kUsPerSec, t2 + 15 * kUsPerSec).verdict == SyncVerdict::kNeedConfirmation);

  // A second answer 1 h away is ordinary drift.
  TimeSync c(buildSec);
  c.begin(0, 0, nullptr);
  CHECK(c.onAnswer(first, 0).verdict == SyncVerdict::kAccept);
  CHECK(c.onAnswer(first + 2 * hour, t1).verdict == SyncVerdict::kAccept);  // FW-TIME-017
  CHECK(c.rejectedSyncs() == 0);

  // FW-TIME-017 RTC and last sync both present: the last sync is the reference. The RTC was 5 days
  // slow and its correction was confirmed; an answer near the RTC's old idea of the time is now a jump.
  const int64_t rtc = first - 5 * day;
  TimeSync d(buildSec);
  CHECK(d.begin(0, 0, &rtc));
  CHECK(d.onAnswer(first, 0).verdict == SyncVerdict::kNeedConfirmation);
  CHECK(d.onAnswer(first + kUsPerSec, kUsPerSec).verdict == SyncVerdict::kAccept);
  CHECK(d.onAnswer(rtc + hour, t1).verdict == SyncVerdict::kNeedConfirmation);
  CHECK(d.onAnswer(first + hour + kUsPerSec, t1 + kUsPerSec).verdict == SyncVerdict::kAccept);

  // FW-TIME-013 the floor still comes first once synced.
  CHECK(c.onAnswer(build - 1, 2 * t1).reason == SyncReason::kBeforeBuild);
}

// FW-FX-002: exact bytes, unlike the HSV route.
static void testParseColorRgb() {
  Rgb c;
  CHECK(parseColorRgb("#ffa030", c) && (c == Rgb{0xff, 0xa0, 0x30}));
  CHECK(parseColorRgb("#f00", c) && (c == Rgb{255, 0, 0}));
  CHECK(parseColorRgb(" ORANGE ", c) && (c == Rgb{0xff, 0x6a, 0x00}));
  CHECK(parseColorRgb("0,100,50", c) && (c == Rgb{128, 0, 0}));
  CHECK(!parseColorRgb("chartreuse", c));
  CHECK(!parseColorRgb("361,0,0", c));
  CHECK(!parseColorRgb("#12345", c));
  CHECK(!parseColorRgb(nullptr, c));
}

static FxError parseFx(const std::string& json, const Effect* base, Effect& out) {
  return parseEffect(json.data(), json.size(), base, out);
}

static std::string errorOf(FxError e) {
  char text[80];
  fxErrorText(e, text, sizeof(text));
  return text;
}

// FW-FX-001..008, FW-FX-013, FW-FX-014
static void testEffectParse() {
  Effect e;
  CHECK(parseFx("{\"pattern\":\"wave\"}", nullptr, e) == FxError::kNone);  // FW-FX-001 every other field has a default
  CHECK(e.pattern == FxPattern::kWave && e.stopCount == 1 && !e.rainbow);
  CHECK((e.stops[0].color == Rgb{0xff, 0x18, 0x00}));                               // FW-FX-002 nixie neon orange
  CHECK(e.interpolate == FxInterpolate::kHsv && e.direction == FxDirection::kLtr);  // FW-FX-003, FW-FX-004
  CHECK(e.period == 18000 && e.phase == 8 && e.easing == FxEasing::kLinear);        // FW-FX-005..007
  CHECK(!e.hasPulse);                                                               // FW-FX-008

  CHECK(parseFx("{\"palette\":\"rainbow\"}", nullptr, e) == FxError::kPatternMissing);  // FW-FX-001
  CHECK(parseFx("{\"pattern\":\"breathe\"}", nullptr, e) == FxError::kNone);
  CHECK(e.easing == FxEasing::kSine && e.hasPulse && e.pulse.min == 32 && e.pulse.max == 255);  // FW-FX-024

  CHECK(parseFx("{\"pattern\":\"wave\",\"palette\":[\"red\",\"#00f\",\"120,100,50\",{\"at\":90,\"color\":\"white\"}],"
                "\"interpolate\":\"rgb\",\"direction\":\"edges\",\"period\":100,\"phase\":100,\"easing\":\"step\","
                "\"pulse\":{\"min\":1,\"max\":2,\"period\":0,\"easing\":\"ease-in-out\"}}",
                nullptr, e) == FxError::kNone);
  CHECK(e.stopCount == 4 && (e.stops[0].color == Rgb{255, 0, 0}) && e.stops[3].at == 90);  // FW-FX-002
  CHECK(e.interpolate == FxInterpolate::kRgb && e.direction == FxDirection::kEdges);
  CHECK(e.period == 100 && e.phase == 100 && e.easing == FxEasing::kStep);
  CHECK(e.hasPulse && e.pulse.min == 1 && e.pulse.max == 2 && e.pulse.period == 0 && e.pulse.easing == FxEasing::kEaseInOut);

  // FW-FX-013: each bad value is an error naming its field.
  struct Bad {
    const char* json;
    FxError error;
    const char* field;
  };
  const Bad bad[] = {
      {"not json", FxError::kBadJson, "JSON object"},
      {"[1]", FxError::kBadJson, "JSON object"},
      {"{\"pattern\":\"wave\",\"bogus\":1}", FxError::kUnknownField, "unknown field"},
      {"{\"pattern\":\"plasma\"}", FxError::kPattern, "pattern"},
      {"{\"pattern\":\"fire\",\"density\":0}", FxError::kDensity, "density"},  // FW-FX-009
      {"{\"pattern\":\"fire\",\"density\":101}", FxError::kDensity, "density"},
      {"{\"pattern\":\"chase\",\"width\":7}", FxError::kWidth, "width"},  // FW-FX-010
      {"{\"pattern\":\"chase\",\"width\":0}", FxError::kWidth, "width"},
      {"{\"pattern\":\"chase\",\"count\":0}", FxError::kCount, "count"},
      {"{\"pattern\":\"chase\",\"count\":\"2\"}", FxError::kCount, "count"},
      {"{\"pattern\":\"strobe\",\"duty\":101}", FxError::kDuty, "duty"},  // FW-FX-011
      {"{\"pattern\":\"sparkle\",\"seed\":65536}", FxError::kSeed, "seed"},  // FW-FX-012
      {"{\"pattern\":\"sparkle\",\"seed\":-1}", FxError::kSeed, "seed"},
      {"{\"pattern\":\"matrix\",\"glyphs\":1}", FxError::kGlyphs, "glyphs"},  // FW-FX-017
      {"{\"pattern\":\"wave\",\"accent\":5}", FxError::kAccent, "accent"},    // FW-FX-016
      {"{\"pattern\":\"wave\",\"accent\":{\"style\":\"wobble\"}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"ms\":49}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"ms\":3001}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"color\":\"nope\"}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"tubes\":\"11111\"}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"tubes\":\"11x111\"}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"spread\":1001}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"strength\":101}}", FxError::kAccent, "accent"},
      {"{\"pattern\":\"wave\",\"accent\":{\"bogus\":1}}", FxError::kAccent, "accent"},
      {"{\"pattern\":1}", FxError::kPattern, "pattern"},
      {"{\"pattern\":\"wave\",\"palette\":\"#ff0000\"}", FxError::kPalette, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":5}", FxError::kPalette, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[]}", FxError::kPaletteSize, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[\"red\",\"red\",\"red\",\"red\",\"red\",\"red\",\"red\",\"red\",\"red\"]}", FxError::kPaletteSize, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[\"nope\"]}", FxError::kColor, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[7]}", FxError::kColor, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[{\"at\":101,\"color\":\"red\"}]}", FxError::kStop, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[{\"at\":5}]}", FxError::kStop, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[{\"color\":\"red\",\"x\":1}]}", FxError::kStop, "palette"},
      {"{\"pattern\":\"wave\",\"palette\":[{\"at\":50,\"color\":\"red\"},{\"at\":10,\"color\":\"red\"}]}", FxError::kStopOrder, "palette"},
      {"{\"pattern\":\"wave\",\"interpolate\":\"lab\"}", FxError::kInterpolate, "interpolate"},
      {"{\"pattern\":\"wave\",\"direction\":\"up\"}", FxError::kDirection, "direction"},
      {"{\"pattern\":\"wave\",\"period\":99}", FxError::kPeriod, "period"},
      {"{\"pattern\":\"wave\",\"period\":600001}", FxError::kPeriod, "period"},
      {"{\"pattern\":\"wave\",\"period\":-1}", FxError::kPeriod, "period"},
      {"{\"pattern\":\"wave\",\"period\":1000.5}", FxError::kPeriod, "period"},
      {"{\"pattern\":\"wave\",\"period\":\"1000\"}", FxError::kPeriod, "period"},
      {"{\"pattern\":\"wave\",\"phase\":101}", FxError::kPhase, "phase"},
      {"{\"pattern\":\"wave\",\"drift\":99}", FxError::kDrift, "drift"},
      {"{\"pattern\":\"wave\",\"drift\":600001}", FxError::kDrift, "drift"},
      {"{\"pattern\":\"wave\",\"drift\":\"1000\"}", FxError::kDrift, "drift"},
      {"{\"pattern\":\"wave\",\"easing\":\"bounce\"}", FxError::kEasing, "easing"},
      {"{\"pattern\":\"wave\",\"pulse\":3}", FxError::kPulse, "pulse"},
      {"{\"pattern\":\"wave\",\"pulse\":{\"min\":256}}", FxError::kPulse, "pulse"},
      {"{\"pattern\":\"wave\",\"pulse\":{\"period\":50}}", FxError::kPulse, "pulse"},
      {"{\"pattern\":\"wave\",\"pulse\":{\"easing\":\"nope\"}}", FxError::kPulse, "pulse"},
      {"{\"pattern\":\"wave\",\"pulse\":{\"depth\":1}}", FxError::kPulse, "pulse"},
      {"{\"pattern\":\"wave\",\"pulse\":{\"min\":9,\"max\":8}}", FxError::kPulseMinMax, "pulse"},
  };
  for (const Bad& b : bad) {
    Effect untouched;
    untouched.phase = 42;
    CHECK(parseFx(b.json, nullptr, untouched) == b.error);
    CHECK(untouched.phase == 42);                                // on failure nothing changes
    CHECK(errorOf(b.error).find(b.field) != std::string::npos);  // FW-FX-013
    CHECK(errorOf(b.error).size() < 80);
    CHECK(errorOf(b.error).find('"') == std::string::npos);  // the message goes into JSON as it is
  }
  CHECK(errorOf(FxError::kPeriod) == "period: need 0 or 100-600000");  // FW-API-002
  CHECK(errorOf(FxError::kPulseMinMax) == "pulse: min must not exceed max");

  // FW-FX-014: over the limit, and the largest legal effect fits the 2 KB document and the buffers.
  CHECK(parseFx("{\"pattern\":\"wave\"}" + std::string(1024, ' '), nullptr, e) == FxError::kTooLarge);
  std::string big = "{\"pattern\":\"gradient\",\"palette\":[";
  for (int i = 0; i < kFxMaxStops; i++) big += std::string(i ? "," : "") + "{\"at\":100,\"color\":\"#ffff0" + std::to_string(i) + "\"}";
  big += "],\"interpolate\":\"rgb\",\"direction\":\"center\",\"period\":600000,\"phase\":100,\"drift\":600000,\"easing\":\"ease-in-out\","
         "\"pulse\":{\"min\":255,\"max\":255,\"period\":600000,\"easing\":\"ease-in-out\"}}";
  CHECK(big.size() <= kFxMaxBodyBytes);
  CHECK(parseFx(big, nullptr, e) == FxError::kNone);
  char json[kFxJsonBytes];
  const size_t n = writeEffectJson(e, json, sizeof(json), true, "a preset name of 24 char");
  CHECK(n > 0 && n <= kFxMaxBodyBytes);  // FW-FX-014
  char tiny[8];
  CHECK(writeEffectJson(e, tiny, sizeof(tiny), false, nullptr) == 0);

  // FW-FX-060: the round trip, including the preset of the /effect.json form (FW-FX-050).
  Effect again;
  char preset[kPresetNameMax + 1] = "stale";
  CHECK(parseActiveEffect(json, n, again, preset) == FxError::kNone);
  CHECK(again == e && std::strcmp(preset, "a preset name of 24 char") == 0);
  Effect rainbow = Effect::defaults(FxPattern::kWave);
  CHECK(parseFx("{\"pattern\":\"wave\",\"palette\":\"rainbow\"}", nullptr, rainbow) == FxError::kNone);
  const size_t m = writeEffectJson(rainbow, json, sizeof(json), true, nullptr);
  CHECK(std::string(json, m).find("\"palette\":\"rainbow\"") != std::string::npos);
  CHECK(std::string(json, m).find("\"pulse\":null,\"accent\":null,\"preset\":null}") != std::string::npos);  // FW-FX-060 every field
  CHECK(parseActiveEffect(json, m, again, preset) == FxError::kNone);
  CHECK(again == rainbow && preset[0] == '\0');
  CHECK(parseFx(std::string(json, m), nullptr, again) == FxError::kUnknownField);  // `preset` is not an effect field
  CHECK(std::string(json, m).find("density") == std::string::npos);                // only the patterns that read them

  // FW-FX-009..012, FW-FX-060: each v1.1 pattern writes its own fields, and they round-trip.
  const char* const own[] = {
      "{\"pattern\":\"sparkle\",\"density\":77,\"seed\":65535}",
      "{\"pattern\":\"fire\",\"density\":1,\"seed\":9}",
      "{\"pattern\":\"chase\",\"width\":6,\"count\":6}",
      "{\"pattern\":\"strobe\",\"duty\":0}",
  };
  const char* const fields[] = {"\"density\":77,\"seed\":65535", "\"density\":1,\"seed\":9", "\"width\":6,\"count\":6", "\"duty\":0"};
  for (int i = 0; i < 4; i++) {
    Effect a;
    CHECK(parseFx(own[i], nullptr, a) == FxError::kNone);
    const size_t len = writeEffectJson(a, json, sizeof(json), false, nullptr);
    CHECK(len > 0 && std::string(json, len).find(fields[i]) != std::string::npos);
    Effect b;
    CHECK(parseFx(std::string(json, len), nullptr, b) == FxError::kNone && b == a);
  }
  // The largest v1.1 effect still fits the buffers, accent and all.
  std::string bigChase = big;
  bigChase.replace(bigChase.find("gradient"), 8, "matrix");
  bigChase.insert(bigChase.size() - 1,
                  ",\"density\":100,\"seed\":65535,\"glyphs\":false,"
                  "\"accent\":{\"style\":\"flicker\",\"ms\":3000,\"color\":\"#ffffff\",\"tubes\":\"110110\",\"spread\":1000,\"strength\":100}");
  CHECK(bigChase.size() <= kFxMaxBodyBytes && parseFx(bigChase, nullptr, e) == FxError::kNone);
  const size_t bigLen = writeEffectJson(e, json, sizeof(json), true, "a preset name of 24 char");
  CHECK(bigLen > 0 && bigLen + 1 < kFxJsonBytes);
  Effect bigBack;
  CHECK(parseActiveEffect(json, bigLen, bigBack, preset) == FxError::kNone && bigBack == e);

  // FW-FX-016: the accent round-trips, keeps what a partial body leaves out, survives a pattern
  // switch, and null removes it.
  Effect acc;
  CHECK(parseFx("{\"pattern\":\"wave\",\"accent\":{\"style\":\"dip\",\"tubes\":\"hm\"}}", nullptr, acc) == FxError::kNone);
  CHECK(acc.hasAccent && acc.accent.style == FxAccentStyle::kDip && acc.accent.ms == 400 && acc.accent.tubes == kAccentHmTubes);
  CHECK(!acc.accent.hasColor && acc.accent.strength == 100 && acc.accent.spread == 0);
  size_t accLen = writeEffectJson(acc, json, sizeof(json), false, nullptr);
  CHECK(std::string(json, accLen).find("\"accent\":{\"style\":\"dip\",\"ms\":400,\"color\":null,\"tubes\":\"hm\",\"spread\":0,\"strength\":100}") != std::string::npos);
  Effect accNext;
  CHECK(parseFx("{\"accent\":{\"ms\":900,\"color\":\"red\",\"tubes\":\"100001\"}}", &acc, accNext) == FxError::kNone);
  CHECK(accNext.accent.style == FxAccentStyle::kDip && accNext.accent.ms == 900 && accNext.accent.hasColor && accNext.accent.tubes == 0x21);
  accLen = writeEffectJson(accNext, json, sizeof(json), false, nullptr);
  CHECK(std::string(json, accLen).find("\"color\":\"#ff0000\",\"tubes\":\"100001\"") != std::string::npos);
  CHECK(parseFx(std::string(json, accLen), nullptr, acc) == FxError::kNone && acc == accNext);
  CHECK(parseFx("{\"pattern\":\"fire\"}", &accNext, acc) == FxError::kNone && acc.hasAccent && acc.accent.ms == 900);
  CHECK(parseFx("{\"accent\":{\"color\":null}}", &accNext, acc) == FxError::kNone && !acc.accent.hasColor);
  CHECK(parseFx("{\"accent\":null}", &accNext, acc) == FxError::kNone && !acc.hasAccent);
  // FW-FX-017: glyphs belong to matrix; a switch resets them.
  CHECK(parseFx("{\"pattern\":\"matrix\",\"glyphs\":true}", nullptr, acc) == FxError::kNone && acc.glyphs);
  accLen = writeEffectJson(acc, json, sizeof(json), false, nullptr);
  CHECK(std::string(json, accLen).find("\"density\":20,\"seed\":0,\"glyphs\":true") != std::string::npos);
  CHECK(parseFx("{\"pattern\":\"fire\"}", &acc, accNext) == FxError::kNone && !accNext.glyphs);

  // FW-FX-061: a partial body keeps the rest; a pattern switch resets the pattern's own fields.
  Effect base;
  CHECK(parseFx("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"direction\":\"rtl\",\"period\":5000,\"phase\":20,\"easing\":\"sine\"}", nullptr, base) == FxError::kNone);
  Effect next;
  CHECK(parseFx("{\"period\":6000}", &base, next) == FxError::kNone);
  CHECK(next.period == 6000 && next.rainbow && next.direction == FxDirection::kRtl && next.phase == 20 && next.easing == FxEasing::kSine);
  CHECK(parseFx("{\"pattern\":\"gradient\"}", &base, next) == FxError::kNone);
  CHECK(next.pattern == FxPattern::kGradient && next.rainbow && next.period == 5000 && next.easing == FxEasing::kSine);
  CHECK(parseFx("{\"pattern\":\"breathe\"}", &base, next) == FxError::kNone);
  CHECK(next.easing == FxEasing::kSine && next.hasPulse && next.pulse.min == 32);  // FW-FX-061 into breathe
  Effect breathing = next;
  CHECK(parseFx("{\"pattern\":\"solid\"}", &breathing, next) == FxError::kNone);
  CHECK(next.easing == FxEasing::kLinear && !next.hasPulse);  // FW-FX-061 out of breathe
  CHECK(parseFx("{\"pulse\":{\"max\":40}}", &base, next) == FxError::kNone && next.hasPulse && next.pulse.max == 40 && next.pulse.min == 0);
  Effect pulsing = next;
  CHECK(parseFx("{\"pulse\":{\"min\":5}}", &pulsing, next) == FxError::kNone && next.pulse.min == 5 && next.pulse.max == 40);  // keeps max
  CHECK(parseFx("{\"pulse\":null}", &pulsing, next) == FxError::kNone && !next.hasPulse);
  CHECK(parseFx("{\"pulse\":null}", &breathing, next) == FxError::kNone && next.hasPulse && next.pulse.min == 32);
  CHECK(!(next == base));

  // FW-FX-061: a switch resets the v1.1 fields; a partial body on the same pattern keeps them.
  Effect chasing;
  CHECK(parseFx("{\"pattern\":\"chase\",\"width\":3,\"count\":2}", nullptr, chasing) == FxError::kNone);
  CHECK(parseFx("{\"period\":900}", &chasing, next) == FxError::kNone && next.width == 3 && next.count == 2);
  CHECK(parseFx("{\"pattern\":\"wave\"}", &chasing, next) == FxError::kNone && next.width == 1 && next.count == 1);
  CHECK(parseFx("{\"pattern\":\"strobe\",\"duty\":10}", &chasing, next) == FxError::kNone && next.duty == 10 && next.width == 1);
}

static EffectEngine engineFor(const std::string& json) {
  Effect e;
  CHECK(parseFx(json, nullptr, e) == FxError::kNone);
  EffectEngine engine;
  engine.apply(e);
  return engine;
}

static bool near(const Rgb& a, const Rgb& b, int tolerance = 2) {
  auto ch = [tolerance](int x, int y) { return x - y <= tolerance && y - x <= tolerance; };
  return ch(a.r, b.r) && ch(a.g, b.g) && ch(a.b, b.b);
}

static int hueOf(const Rgb& c) {
  char hex[8];
  formatHex(c, hex);
  Hsv hsv;
  hexToHsv(hex, hsv);
  return hsv.h;
}

// FW-FX-003, FW-FX-004, FW-FX-006, FW-FX-007, FW-FX-022, FW-FX-071 (VER-019)
static void testEffectMath() {
  // FW-FX-004: each direction's d(i), in halves.
  const int expected[4][6] = {{0, 2, 4, 6, 8, 10}, {10, 8, 6, 4, 2, 0}, {5, 3, 1, 1, 3, 5}, {1, 3, 5, 5, 3, 1}};
  for (int d = 0; d < 4; d++) {
    for (int t = 0; t < kTubes; t++) CHECK(fxDistanceHalves(static_cast<FxDirection>(d), t) == expected[d][t]);
  }

  // FW-FX-007: every easing at 0, 0.5 and 1.
  const FxEasing all[] = {FxEasing::kLinear, FxEasing::kSine, FxEasing::kEaseIn, FxEasing::kEaseOut, FxEasing::kEaseInOut, FxEasing::kStep};
  for (FxEasing e : all) {
    CHECK(fxEase(e, 0) == 0);
    CHECK(fxEase(e, 65535) >= 65533);
    for (uint32_t x = 1024; x < 65536; x += 1024) {
      CHECK(fxEase(e, static_cast<uint16_t>(x)) >= fxEase(e, static_cast<uint16_t>(x - 1024)));  // never runs backwards
    }
  }
  CHECK(fxEase(FxEasing::kLinear, 32768) == 32768);
  CHECK(fxEase(FxEasing::kSine, 32768) > 32000 && fxEase(FxEasing::kSine, 32768) < 33500);
  CHECK(fxEase(FxEasing::kEaseIn, 32768) == 16384);
  CHECK(fxEase(FxEasing::kEaseOut, 32768) > 49000 && fxEase(FxEasing::kEaseOut, 32768) < 49200);
  CHECK(fxEase(FxEasing::kEaseInOut, 32768) > 32000 && fxEase(FxEasing::kEaseInOut, 32768) < 33500);
  CHECK(fxEase(FxEasing::kStep, 32767) == 0 && fxEase(FxEasing::kStep, 32768) == 65535);

  // FW-FX-022, VER-019: a rainbow wave at t=0: tube 0 hue 0, tube 1 about 330 (exactly 360 - 8% = 331.2).
  EffectEngine rainbow = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"direction\":\"ltr\",\"period\":18000,\"phase\":8}");
  CHECK((rainbow.tubeColor(0, 0) == Rgb{255, 0, 0}));
  CHECK(hueOf(rainbow.tubeColor(1, 0)) >= 329 && hueOf(rainbow.tubeColor(1, 0)) <= 333);
  CHECK(hueOf(rainbow.tubeColor(5, 0)) >= 214 && hueOf(rainbow.tubeColor(5, 0)) <= 218);  // 360 - 5 * 28.8

  // FW-FX-006: tube i+1 shows tube i's color phase% of a period later, for ltr.
  for (uint64_t t = 0; t < 40000; t += 777) {
    for (int i = 0; i + 1 < kTubes; i++) CHECK(near(rainbow.tubeColor(i + 1, t + 1440), rainbow.tubeColor(i, t)));
  }
  // FW-FX-004: rtl is the mirror of ltr.
  EffectEngine rtl = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"direction\":\"rtl\"}");
  for (int i = 0; i < kTubes; i++) CHECK(near(rtl.tubeColor(i, 5000), rainbow.tubeColor(5 - i, 5000)));
  // FW-FX-004: center starts in the middle: tubes 3 and 4 see a color before tubes 1 and 6.
  EffectEngine center = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"direction\":\"center\"}");
  CHECK(near(center.tubeColor(2, 5000), center.tubeColor(3, 5000)));
  CHECK(near(center.tubeColor(0, 5000 + 2880), center.tubeColor(2, 5000)));

  // FW-FX-015: drift starts as rtl, is ltr halfway through the sway, and passes through center.
  // Its offsets are the plain ones less five half units, the same as 20% of a period later at phase 8.
  EffectEngine drifting = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"period\":10000,\"drift\":60000}");
  EffectEngine rtl10 = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"direction\":\"rtl\",\"period\":10000}");
  EffectEngine ltr10 = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"period\":10000}");
  for (int i = 0; i < kTubes; i++) {
    CHECK(near(drifting.tubeColor(i, 0), rtl10.tubeColor(i, 2000), 8));
    CHECK(near(drifting.tubeColor(i, 30000), ltr10.tubeColor(i, 32000), 8));
  }
  bool crossedCenter = false;
  for (uint64_t t = 0; t < 30000; t += 50) {
    if (near(drifting.tubeColor(0, t), drifting.tubeColor(5, t), 8) && near(drifting.tubeColor(1, t), drifting.tubeColor(4, t), 8) &&
        near(drifting.tubeColor(2, t), drifting.tubeColor(3, t), 8) && !near(drifting.tubeColor(0, t), drifting.tubeColor(2, t), 8)) {
      crossedCenter = true;
    }
    for (int i = 0; i < kTubes; i++) CHECK(near(drifting.tubeColor(i, t), drifting.tubeColor(i, t + 50), 12));  // never jumps
  }
  CHECK(crossedCenter);
  // FW-FX-015: 0 is off, and other patterns ignore it.
  EffectEngine driftOff = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"period\":10000,\"drift\":0}");
  CHECK((driftOff.tubeColor(2, 1234) == ltr10.tubeColor(2, 1234)));
  EffectEngine solidDrift = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":1000,\"drift\":5000}");
  CHECK((solidDrift.tubeColor(0, 250) == solidDrift.tubeColor(5, 250)));

  // FW-FX-022: nothing steps. Colors move by small amounts between 50 ms frames, for every tube.
  for (uint64_t t = 0; t < 36000; t += 50) {
    for (int i = 0; i < kTubes; i++) CHECK(near(rainbow.tubeColor(i, t), rainbow.tubeColor(i, t + 50), 6));
  }

  // FW-FX-002, FW-FX-003: stops and midpoints, in both color spaces, with the wrap to the first stop.
  EffectEngine rgb = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"period\":1000}");
  CHECK((rgb.tubeColor(0, 0) == Rgb{255, 0, 0}));        // stop 0
  CHECK(near(rgb.tubeColor(0, 250), Rgb{128, 0, 128}));  // midpoint
  CHECK((rgb.tubeColor(0, 500) == Rgb{0, 0, 255}));      // stop 1
  CHECK(near(rgb.tubeColor(0, 750), Rgb{128, 0, 128}));  // back towards the first stop
  EffectEngine hsv = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":1000}");
  CHECK((hsv.tubeColor(0, 0) == Rgb{255, 0, 0}));
  CHECK(near(hsv.tubeColor(0, 250), Rgb{255, 0, 255}));  // FW-FX-003 shortest hue path, through magenta
  CHECK((hsv.tubeColor(0, 500) == Rgb{0, 0, 255}));
  EffectEngine toWhite = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#00ff00\",\"#ffffff\"],\"period\":1000}");
  CHECK(near(toWhite.tubeColor(0, 250), Rgb{128, 255, 128}, 3));  // white has no hue of its own to detour through
  // FW-FX-002: `at` moves a stop.
  EffectEngine placed = engineFor("{\"pattern\":\"solid\",\"palette\":[{\"at\":0,\"color\":\"#ff0000\"},{\"at\":25,\"color\":\"#0000ff\"}],\"interpolate\":\"rgb\",\"period\":1000}");
  CHECK((placed.tubeColor(0, 250) == Rgb{0, 0, 255}));
  CHECK(near(placed.tubeColor(0, 625), Rgb{128, 0, 128}, 3));
  // FW-FX-007: `step` holds each stop to the next boundary, with no fade.
  EffectEngine stepped = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#00ff00\",\"#0000ff\",\"#ffffff\"],\"easing\":\"step\",\"period\":4000}");
  CHECK((stepped.tubeColor(0, 0) == Rgb{255, 0, 0}) && (stepped.tubeColor(0, 999) == Rgb{255, 0, 0}));
  CHECK((stepped.tubeColor(0, 1000) == Rgb{0, 255, 0}) && (stepped.tubeColor(0, 3999) == Rgb{255, 255, 255}));
  EffectEngine steppedRainbow = engineFor("{\"pattern\":\"solid\",\"palette\":\"rainbow\",\"easing\":\"step\",\"period\":6000}");
  CHECK((steppedRainbow.tubeColor(0, 900) == Rgb{255, 0, 0}) && (steppedRainbow.tubeColor(0, 1100) == Rgb{255, 255, 0}));  // to 1/64 of a cycle
  // FW-FX-007: an easing changes where in the palette a position lands.
  EffectEngine easedIn = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"easing\":\"ease-in\",\"period\":1000}");
  CHECK(near(easedIn.tubeColor(0, 500), rgb.tubeColor(0, 250)));  // 0.5^2 = 0.25
}

// FW-FX-008, FW-FX-020, FW-FX-021, FW-FX-023, FW-FX-024, FW-FX-029 (VER-019)
static void testEffectPatterns() {
  EffectEngine solid = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#102030\"],\"period\":0}");
  for (int t = 0; t < kTubes; t++) CHECK((solid.tubeColor(t, 12345) == Rgb{0x10, 0x20, 0x30}));  // FW-FX-020
  EffectEngine cycling = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":1000}");
  CHECK(cycling.tubeColor(0, 500) == cycling.tubeColor(5, 500));  // FW-FX-020 all tubes together
  CHECK(!(cycling.tubeColor(0, 0) == cycling.tubeColor(0, 500)));

  EffectEngine tubes = engineFor("{\"pattern\":\"tubes\",\"palette\":[\"#010101\",\"#020202\"]}");
  CHECK((tubes.tubeColor(0, 0) == Rgb{1, 1, 1}) && (tubes.tubeColor(1, 0) == Rgb{2, 2, 2}));      // FW-FX-021
  CHECK((tubes.tubeColor(2, 999) == Rgb{1, 1, 1}) && (tubes.tubeColor(5, 999) == Rgb{2, 2, 2}));  // a short palette repeats

  // FW-FX-023: one palette length across the tubes, phase ignored; period 0 is static.
  EffectEngine still = engineFor("{\"pattern\":\"gradient\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"period\":0,\"phase\":77}");
  CHECK((still.tubeColor(0, 0) == Rgb{255, 0, 0}));
  CHECK((still.tubeColor(3, 0) == Rgb{0, 0, 255}));           // half way across = the second of two stops
  CHECK(still.tubeColor(2, 0) == still.tubeColor(2, 99999));  // static
  EffectEngine drift = engineFor("{\"pattern\":\"gradient\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"period\":6000}");
  CHECK(near(drift.tubeColor(1, 1000), drift.tubeColor(0, 0)));  // FW-FX-023 drifts in `direction` order
  EffectEngine mirrored = engineFor("{\"pattern\":\"gradient\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"direction\":\"center\",\"period\":0}");
  CHECK((mirrored.tubeColor(2, 0) == Rgb{255, 0, 0}) && (mirrored.tubeColor(3, 0) == Rgb{255, 0, 0}));
  CHECK(mirrored.tubeColor(0, 0) == mirrored.tubeColor(5, 0));

  // FW-FX-024: the swing, and one stop per breath.
  EffectEngine breathe = engineFor("{\"pattern\":\"breathe\",\"palette\":[\"#ff0000\",\"#00ff00\"],\"period\":1000}");
  CHECK(breathe.level(0) == 32 && breathe.level(500) == 255 && breathe.level(1000) == 32);
  for (uint64_t t = 0; t < 3000; t += 10) CHECK(breathe.level(t) >= 32);
  CHECK((breathe.tubeColor(0, 400) == Rgb{255, 0, 0}) && (breathe.tubeColor(0, 1400) == Rgb{0, 255, 0}));
  CHECK((breathe.tubeColor(0, 2400) == Rgb{255, 0, 0}));
  EffectEngine held = engineFor("{\"pattern\":\"breathe\",\"period\":0,\"pulse\":{\"min\":10,\"max\":20}}");
  CHECK(held.level(777) == 20);
  EffectEngine rainbowBreath = engineFor("{\"pattern\":\"breathe\",\"palette\":\"rainbow\",\"period\":1000}");
  CHECK((rainbowBreath.tubeColor(0, 1000) == Rgb{255, 255, 0}));

  // FW-FX-008, FW-FX-036: pulse bounds on any pattern; no pulse means no dimming.
  EffectEngine pulse = engineFor("{\"pattern\":\"solid\",\"pulse\":{\"min\":64,\"max\":200,\"period\":4000}}");
  CHECK(pulse.level(0) == 64 && pulse.level(2000) == 200 && pulse.level(4000) == 64);
  int top = 0;
  for (uint64_t t = 0; t < 8000; t += 25) {
    CHECK(pulse.level(t) >= 64 && pulse.level(t) <= 200);
    top = pulse.level(t) > top ? pulse.level(t) : top;
  }
  CHECK(top == 200);
  CHECK(engineFor("{\"pattern\":\"wave\"}").level(1234) == 255);
  CHECK(engineFor("{\"pattern\":\"solid\",\"pulse\":{\"max\":40,\"period\":0}}").level(4321) == 40);  // `night`

  // FW-FX-025: sparkle rests on the base and flashes the other colors, more often with more density.
  const Rgb navy{0x10, 0x10, 0x30};
  auto flashFrames = [&](const std::string& json) {
    EffectEngine fx = engineFor(json);
    int flashes = 0;
    for (uint64_t t = 0; t < 60000; t += 50) {
      for (int i = 0; i < kTubes; i++) {
        const Rgb c = fx.tubeColor(i, t);
        if (!(c == navy)) {
          flashes++;
          CHECK(c.b >= navy.b);  // only ever toward the white flash
        }
      }
    }
    return flashes;
  };
  const int sparse = flashFrames("{\"pattern\":\"sparkle\",\"palette\":[\"#101030\",\"#ffffff\"],\"density\":10}");
  const int dense = flashFrames("{\"pattern\":\"sparkle\",\"palette\":[\"#101030\",\"#ffffff\"],\"density\":90}");
  CHECK(sparse > 0 && dense > sparse * 4);
  CHECK(flashFrames("{\"pattern\":\"sparkle\",\"palette\":[\"#101030\",\"#ffffff\"],\"density\":90,\"seed\":1}") > sparse * 4);
  EffectEngine seedA = engineFor("{\"pattern\":\"sparkle\",\"density\":50,\"seed\":1}");
  EffectEngine seedB = engineFor("{\"pattern\":\"sparkle\",\"density\":50,\"seed\":2}");
  bool differs = false;
  for (uint64_t t = 0; t < 20000 && !differs; t += 50) differs = !(seedA.tubeColor(0, t) == seedB.tubeColor(0, t));
  CHECK(differs);  // FW-FX-012 the seed picks the sequence
  // A flash is bright at once and fades: no frame is brighter than the one before it within a flash.
  EffectEngine lone = engineFor("{\"pattern\":\"sparkle\",\"palette\":[\"#ffffff\"],\"density\":100}");
  for (uint64_t t = 0; t < 4000; t += 10) {
    const Rgb a = lone.tubeColor(2, t), b = lone.tubeColor(2, t + 10);
    CHECK(b.r <= a.r || a.r == 0);  // FW-FX-025 one color: off between flashes
  }

  // FW-FX-026: chase. One runner, ltr, a 6 s lap: tube i is fully lit about i s in, and the tube ahead
  // is still on the base.
  EffectEngine chase = engineFor("{\"pattern\":\"chase\",\"palette\":[\"#000000\",\"#ffffff\"],\"interpolate\":\"rgb\",\"period\":6000}");
  for (int i = 0; i + 1 < kTubes; i++) {
    const uint64_t t = static_cast<uint64_t>(i) * 1000 + 1000;
    CHECK((chase.tubeColor(i, t + 20) == Rgb{255, 255, 255}));
    CHECK((chase.tubeColor(i + 1, t) == Rgb{0, 0, 0}));
  }
  CHECK(chase.tubeColor(0, 2000).r > 0 && chase.tubeColor(0, 2000).r < 255);  // the tail fades
  for (uint64_t t = 0; t < 12000; t += 50) {
    for (int i = 0; i < kTubes; i++) CHECK(near(chase.tubeColor(i, t), chase.tubeColor(i, t + 50), 40));  // glides
  }
  EffectEngine chaseRtl = engineFor("{\"pattern\":\"chase\",\"palette\":[\"#000000\",\"#ffffff\"],\"interpolate\":\"rgb\",\"direction\":\"rtl\",\"period\":6000}");
  for (int i = 0; i < kTubes; i++) CHECK(chaseRtl.tubeColor(i, 3210) == chase.tubeColor(5 - i, 3210));
  EffectEngine two = engineFor("{\"pattern\":\"chase\",\"palette\":[\"#000000\",\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"period\":6000,\"count\":2}");
  CHECK((two.tubeColor(0, 1020) == Rgb{255, 0, 0}) && (two.tubeColor(3, 1020) == Rgb{0, 0, 255}));  // spaced, each its color
  EffectEngine wide = engineFor("{\"pattern\":\"chase\",\"palette\":[\"#000000\",\"#ffffff\"],\"interpolate\":\"rgb\",\"period\":6000,\"width\":3}");
  CHECK((wide.tubeColor(0, 3000) == Rgb{255, 255, 255}) && (wide.tubeColor(2, 3000) == Rgb{255, 255, 255}));
  EffectEngine centerChase = engineFor("{\"pattern\":\"chase\",\"palette\":[\"#000000\",\"#ffffff\"],\"direction\":\"center\",\"period\":3000}");
  for (uint64_t t = 0; t < 3000; t += 100) CHECK(centerChase.tubeColor(1, t) == centerChase.tubeColor(4, t));  // mirrored

  // FW-FX-027: fire stays between its first and last stops, wanders on each tube, and is smooth enough
  // to read as flicker rather than noise at low density.
  EffectEngine fire = engineFor("{\"pattern\":\"fire\",\"palette\":[\"#100000\",\"#ff0000\"],\"interpolate\":\"rgb\",\"density\":10,\"period\":0}");
  int lo = 255, hi = 0;
  bool independent = false;
  for (uint64_t t = 0; t < 60000; t += 50) {
    const Rgb c = fire.tubeColor(0, t);
    CHECK(c.r >= 0x10 && c.g == 0 && c.b == 0);
    lo = c.r < lo ? c.r : lo;
    hi = c.r > hi ? c.r : hi;
    independent = independent || !(fire.tubeColor(1, t) == c);
    CHECK(near(c, fire.tubeColor(0, t + 50), 80));
  }
  CHECK(lo < 0x50 && hi > 0xd0 && independent);
  EffectEngine ember = engineFor("{\"pattern\":\"fire\",\"palette\":[\"#ff8000\"],\"interpolate\":\"rgb\",\"density\":30}");
  for (uint64_t t = 0; t < 5000; t += 50) CHECK(ember.tubeColor(3, t).b == 0 && ember.tubeColor(3, t).r >= ember.tubeColor(3, t).g);

  // FW-FX-028: strobe: the first color for `duty` of each period, then the second; phase offsets tubes.
  EffectEngine strobe = engineFor("{\"pattern\":\"strobe\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":1000,\"duty\":30,\"phase\":0}");
  CHECK((strobe.tubeColor(0, 0) == Rgb{255, 0, 0}) && (strobe.tubeColor(0, 299) == Rgb{255, 0, 0}));
  CHECK((strobe.tubeColor(0, 301) == Rgb{0, 0, 255}) && (strobe.tubeColor(5, 999) == Rgb{0, 0, 255}));
  EffectEngine police = engineFor("{\"pattern\":\"strobe\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":400,\"phase\":50}");
  for (uint64_t t = 0; t < 2000; t += 25) {
    for (int i = 0; i + 1 < kTubes; i++) CHECK(!(police.tubeColor(i, t) == police.tubeColor(i + 1, t)));  // neighbours alternate
  }
  EffectEngine blink = engineFor("{\"pattern\":\"strobe\",\"palette\":[\"#ffffff\"],\"period\":1000,\"duty\":0}");
  CHECK((blink.tubeColor(0, 10) == Rgb{0, 0, 0}));  // one color: off for the rest
  CHECK((engineFor("{\"pattern\":\"strobe\",\"palette\":[\"#ffffff\"],\"period\":1000,\"duty\":100}").tubeColor(0, 999) == Rgb{255, 255, 255}));

  // FW-FX-029, VER-019: pure: the same inputs give the same output, on a second engine too, with no
  // allocation (FW-FX-071), for every pattern.
  const char* const json[] = {
      "{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"pulse\":{\"min\":1,\"max\":99}}",
      "{\"pattern\":\"tubes\",\"palette\":[\"#ff0000\",\"#0000ff\",\"#00ff00\"]}",
      "{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"easing\":\"sine\",\"direction\":\"edges\"}",
      "{\"pattern\":\"gradient\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"easing\":\"ease-out\"}",
      "{\"pattern\":\"breathe\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":6000}",
      "{\"pattern\":\"sparkle\",\"palette\":[\"#101030\",\"#ffffff\",\"#a0c8ff\"],\"density\":60}",
      "{\"pattern\":\"chase\",\"palette\":[\"#021a02\",\"#00ff41\"],\"count\":3,\"width\":2,\"direction\":\"edges\"}",
      "{\"pattern\":\"fire\",\"palette\":[\"#4a0000\",\"#ff2a00\",\"#ffd27f\"],\"density\":40,\"period\":4000}",
      "{\"pattern\":\"strobe\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"period\":400,\"phase\":50}",
      "{\"pattern\":\"matrix\",\"palette\":[\"#001000\",\"#00c000\",\"#c0ffc0\"],\"density\":50,\"glyphs\":true}",
  };
  for (const char* j : json) {
    EffectEngine a = engineFor(j);
    EffectEngine b = engineFor(j);
    const int before = allocations;
    bool same = true;
    for (uint64_t t = 0; t < 100000; t += 137) {
      for (int i = 0; i < kTubes; i++) same = same && a.tubeColor(i, t) == b.tubeColor(i, t) && a.level(t) == b.level(t);
    }
    CHECK(same);                   // FW-FX-029
    CHECK(allocations == before);  // FW-FX-071
  }

  // VER-019: across the millis() wrap the output is what continuous time gives: a period does not
  // divide 2^32, so a plain 32-bit modulo would jump here.
  EffectEngine wave = engineFor("{\"pattern\":\"wave\",\"palette\":\"rainbow\",\"period\":18000,\"pulse\":{\"min\":9,\"period\":7000}}");
  EffectClock clock;
  uint64_t t = clock.advance(0xFFFFFF00u);
  CHECK(t == 0xFFFFFF00u);
  uint32_t ms = 0xFFFFFF00u;
  for (int frame = 0; frame < 100; frame++) {
    ms += 50;  // wraps in here
    const uint64_t next = clock.advance(ms);
    CHECK(next == t + 50);
    t = next;
    for (int i = 0; i < kTubes; i++) CHECK(wave.tubeColor(i, t) == wave.tubeColor(i, 0xFFFFFF00ull + (frame + 1) * 50ull));
  }
}

// FW-FX-040, FW-FX-041, FW-FX-043, FW-FX-044, built-ins table (VER-019)
static void testPresets() {
  const char* const names[kBuiltinPresetCount] = {"rainbow-wave", "nixie", "classic-blue", "ocean", "sunset", "synthwave",
                                                  "aurora", "lava", "candy", "ice", "night", "vaporwave",
                                                  "fire", "candle", "christmas", "matrix", "cyberpunk", "twinkle"};
  size_t total = 0;
  for (int i = 0; i < kBuiltinPresetCount; i++) {
    char name[kPresetNameMax + 1];
    builtinPresetName(i, name);
    CHECK(std::strcmp(name, names[i]) == 0);
    CHECK(isValidPresetName(name));
    CHECK(findBuiltinPreset(names[i]) == i);
    char json[512];
    const size_t n = builtinPresetJson(i, json, sizeof(json));
    CHECK(n > 0 && n < 512);  // FW-FX-041 at most 512 B each
    total += n + 1;
    Effect e;
    CHECK(loadBuiltinPreset(i, e));
    CHECK(parseEffect(json, n, nullptr, e) == FxError::kNone);
    char pattern[12];
    fxPatternOfJson(json, pattern);
    CHECK(std::strcmp(pattern, fxPatternName(e.pattern)) == 0);  // FW-FX-044 listing does not parse
  }
  CHECK(total <= 8192);                           // FW-FX-074
  CHECK(findBuiltinPreset("RAINBOW-Wave") == 0);  // FW-FX-040 without case
  CHECK(findBuiltinPreset("FIRE") == 12);
  CHECK(findBuiltinPreset("mine") == -1);

  Effect e;
  CHECK(loadBuiltinPreset(findBuiltinPreset("rainbow-wave"), e));  // OQ-22, FW-FX built-ins table
  CHECK(e.pattern == FxPattern::kWave && !e.rainbow && e.stopCount == 8 && e.interpolate == FxInterpolate::kRgb && e.period == 18000 && e.phase == 8 && e.direction == FxDirection::kLtr);
  CHECK(loadBuiltinPreset(findBuiltinPreset("classic-blue"), e));
  CHECK(e.pattern == FxPattern::kTubes && e.stopCount == 6);
  CHECK((e.stops[3].color == hsvToRgb({241, 100, 100})));  // FW-DSP-011 today's firmware default
  CHECK(loadBuiltinPreset(findBuiltinPreset("ocean"), e));
  CHECK(e.pattern == FxPattern::kWave && e.period == 5000 && e.phase == 25 && e.stopCount == 8 && e.hasPulse && e.pulse.min == 170 && e.pulse.period == 7000);
  CHECK(loadBuiltinPreset(findBuiltinPreset("sunset"), e));
  CHECK(e.pattern == FxPattern::kSolid && e.period == 96000 && e.stopCount == 6);
  CHECK(loadBuiltinPreset(findBuiltinPreset("vaporwave"), e));
  CHECK(e.pattern == FxPattern::kSolid && e.period == 16000 && e.stopCount == 8 && e.interpolate == FxInterpolate::kRgb);
  CHECK(loadBuiltinPreset(findBuiltinPreset("synthwave"), e));
  CHECK(e.direction == FxDirection::kRtl && e.period == 10000 && e.phase == 15);
  CHECK(loadBuiltinPreset(findBuiltinPreset("aurora"), e));
  CHECK(e.pattern == FxPattern::kWave && e.period == 18000 && e.phase == 10 && e.drift == 90000 && !e.hasPulse);  // FW-FX-015
  CHECK(loadBuiltinPreset(findBuiltinPreset("lava"), e));
  CHECK(e.pattern == FxPattern::kWave && e.period == 20000 && e.phase == 14 && e.drift == 75000);  // FW-FX-015
  CHECK(loadBuiltinPreset(findBuiltinPreset("ice"), e));
  CHECK(e.pattern == FxPattern::kGradient && e.period == 0);
  CHECK(loadBuiltinPreset(findBuiltinPreset("night"), e));
  CHECK(e.hasPulse && e.pulse.max == 40);
  CHECK(loadBuiltinPreset(findBuiltinPreset("fire"), e));
  CHECK(e.pattern == FxPattern::kFire && e.density == 40 && e.stopCount == 4);
  CHECK(loadBuiltinPreset(findBuiltinPreset("candle"), e));
  CHECK(e.pattern == FxPattern::kFire && e.density == 4 && e.period == 8000);
  CHECK(e.hasAccent && e.accent.style == FxAccentStyle::kFlicker && e.accent.ms == 600);  // FW-FX-016
  CHECK(loadBuiltinPreset(findBuiltinPreset("christmas"), e));
  CHECK(e.pattern == FxPattern::kStrobe && e.period == 1600 && e.phase == 50 && e.duty == 50);
  CHECK(findBuiltinPreset("police") == -1);
  CHECK(loadBuiltinPreset(findBuiltinPreset("matrix"), e));
  CHECK(e.pattern == FxPattern::kMatrix && e.period == 1600 && e.density == 30 && e.glyphs && e.stopCount == 3);  // FW-FX-017
  CHECK(loadBuiltinPreset(findBuiltinPreset("cyberpunk"), e));
  CHECK(e.pattern == FxPattern::kChase && e.period == 2000 && e.count == 1 && e.width == 2);
  CHECK(loadBuiltinPreset(findBuiltinPreset("twinkle"), e));
  CHECK(e.pattern == FxPattern::kSparkle && e.density == 30 && e.stopCount == 3);

  CHECK(isValidPresetName("My preset_1-b"));  // FW-FX-040
  CHECK(isValidPresetName("a"));
  CHECK(isValidPresetName("123456789012345678901234"));
  CHECK(!isValidPresetName("1234567890123456789012345"));  // 25 chars
  CHECK(!isValidPresetName(""));
  CHECK(!isValidPresetName(nullptr));
  CHECK(!isValidPresetName("a/b"));
  CHECK(!isValidPresetName("a\tb"));
  CHECK(!isValidPresetName("a\"b"));
  CHECK(presetNamesEqual("Mine", "mINE"));
  CHECK(!presetNamesEqual("mine", "mine2"));
  CHECK(!presetNamesEqual("mine2", "mine"));

  char line[] = "My one\t{\"pattern\":\"wave\"}";
  const char *name, *json;
  CHECK(splitPresetLine(line, name, json));  // FW-FX-042
  CHECK(std::strcmp(name, "My one") == 0 && std::strcmp(json, "{\"pattern\":\"wave\"}") == 0);
  char noTab[] = "junk";
  CHECK(!splitPresetLine(noTab, name, json));
  char badName[] = "a/b\t{}";
  CHECK(!splitPresetLine(badName, name, json));
  char pattern[12];
  fxPatternOfJson("{\"period\":1}", pattern);
  CHECK(pattern[0] == '\0');

  CHECK(isJsonContentType("application/json", 16));  // FW-FX §6
  CHECK(isJsonContentType("Application/JSON; charset=utf-8", 31));
  CHECK(!isJsonContentType("text/plain", 10));
  CHECK(!isJsonContentType("application/jsonx", 17));
  CHECK(!isJsonContentType("application/json", 0));  // FW-SEC-006 a cleared String's stale bytes
  CHECK(!isJsonContentType("application/json", 5));
}

// FW-FX-031..033, FW-DSP-001, FW-DSP-025
static void testDisplayOptions() {
  DisplayOptions o;
  CHECK(o.leadingZero && o.seconds && o.transition == FxTransition::kCut && o.fadeMs == 200);  // defaults

  std::array<int8_t, kTubes> d = timeDigits(9, 5, 0, true);
  applyDisplayOptions(d, o, false);
  CHECK(d[0] == 0 && d[1] == 9);  // FW-FX-031 09:05:00
  o.leadingZero = false;
  applyDisplayOptions(d, o, false);
  CHECK(d[0] == kBlank && d[1] == 9);  // FW-FX-031
  d = timeDigits(13, 5, 0, true);
  applyDisplayOptions(d, o, false);
  CHECK(d[0] == 1);  // only a zero is blanked
  o = DisplayOptions{};
  o.seconds = false;
  d = timeDigits(13, 5, 9, true);
  applyDisplayOptions(d, o, false);
  CHECK(d[3] == 5 && d[4] == kBlank && d[5] == kBlank);  // FW-FX-032
  d = timeDigits(9, 5, 9, true);
  o.leadingZero = false;
  applyDisplayOptions(d, o, true);
  CHECK(d[0] == 0 && d[5] == 9);  // FW-ATT-002 a flash shows on every tube
  d.fill(kBlank);
  o = DisplayOptions{};
  applyDisplayOptions(d, o, false);
  CHECK(d[0] == kBlank);  // FW-DSP-007 blank stays blank

  // /display.json round trip (FW-FX-050) and the display object (FW-FX-068).
  o.leadingZero = false;
  o.transition = FxTransition::kCrossfade;
  o.fadeMs = 300;
  o.tubeBrightness = {{100, 0, 55, 100, 100, 7}};
  char json[kDisplayJsonBytes];
  size_t n = writeDisplayJson(o, 24, json, sizeof(json));
  CHECK(std::string(json, n) ==
        "{\"hours\":24,\"leadingZero\":false,\"seconds\":true,\"transition\":\"crossfade\",\"fadeMs\":300,\"tubeBrightness\":[100,0,55,100,100,7]}");
  n = writeDisplayJson(o, 0, json, sizeof(json));
  DisplayOptions back;
  CHECK(parseDisplayJson(json, n, back) && back == o);
  // FW-FX-034: the longest object fits, and a file from before tubeBrightness reads as all 100.
  DisplayOptions longest;
  longest.transition = FxTransition::kCrossfade;
  longest.fadeMs = 1000;
  CHECK(writeDisplayJson(longest, 24, json, sizeof(json)) > 0);
  const std::string old = "{\"leadingZero\":false,\"seconds\":true,\"transition\":\"cut\",\"fadeMs\":200}";
  CHECK(parseDisplayJson(old.data(), old.size(), back) && !back.leadingZero && back.tubeBrightness[3] == 100);
  for (const char* badLevels : {"[1,2,3]", "[1,2,3,4,5,101]", "[1,2,3,4,5,\"6\"]", "5"}) {
    const std::string text = "{\"leadingZero\":true,\"seconds\":true,\"transition\":\"cut\",\"fadeMs\":200,\"tubeBrightness\":" + std::string(badLevels) + "}";
    CHECK(!parseDisplayJson(text.data(), text.size(), back));
  }
  std::array<uint8_t, kTubes> levels{};
  CHECK(parseTubeBrightness("100,0,55,100,100,7", levels) && levels == o.tubeBrightness);  // FW-FX-068
  std::array<uint8_t, kTubes> kept = levels;
  for (const char* badText : {"", "1,2,3,4,5", "1,2,3,4,5,6,7", "1,2,3,4,5,101", "1,2,3,4,5,0100", "1,2,3,4,5,x", "1, 2,3,4,5,6", "-1,2,3,4,5,6", "1,2,3,4,5,6,"}) {
    CHECK(!parseTubeBrightness(badText, levels));
  }
  CHECK(!parseTubeBrightness(nullptr, levels) && levels == kept);
  back = DisplayOptions{};
  CHECK(!parseDisplayJson("junk", 4, back));  // FW-FX-052
  CHECK(!parseDisplayJson("{\"leadingZero\":true}", 20, back));
  const std::string badFade = "{\"leadingZero\":true,\"seconds\":true,\"transition\":\"cut\",\"fadeMs\":20}";
  CHECK(!parseDisplayJson(badFade.data(), badFade.size(), back));
  CHECK(back == DisplayOptions{});
  FxTransition tr;
  CHECK(parseFxTransition("cut", tr) && tr == FxTransition::kCut);
  CHECK(parseFxTransition("crossfade", tr) && tr == FxTransition::kCrossfade);
  CHECK(parseFxTransition("scramble", tr) && tr == FxTransition::kScramble);
  CHECK(parseFxTransition("carry", tr) && tr == FxTransition::kCarry && std::strcmp(fxTransitionName(tr), "carry") == 0);
  CHECK(!parseFxTransition("bounce", tr));

  // FW-FX-033: cut is instant.
  const LedLayout layout = LedLayout::ct001();
  auto digits = [](int8_t last) {
    std::array<int8_t, kTubes> a{};
    a[5] = last;
    return a;
  };
  DigitFader cut;
  std::array<TubeState, kTubes> tubes{};
  DisplayOptions cutOptions;
  cut.update(digits(1), 1000, cutOptions);
  cut.update(digits(2), 2000, cutOptions);
  cut.fill(tubes);
  CHECK(tubes[5].digit == 2 && tubes[5].fromDigit == kBlank && tubes[5].mix == 255);

  // FW-FX-033, FW-DSP-001: crossfade: both LEDs over fadeMs, then one again; wrap-safe.
  DisplayOptions fade;
  fade.transition = FxTransition::kCrossfade;
  fade.fadeMs = 200;
  DigitFader fader;
  const uint32_t t0 = 0xFFFFFF80u;
  fader.update(digits(1), t0 - 1000, fade);
  fader.fill(tubes);
  CHECK(tubes[5].digit == 1 && tubes[5].mix == 255);  // nothing to fade from at boot
  fader.update(digits(2), t0, fade);
  fader.fill(tubes);
  CHECK(tubes[5].digit == 2 && tubes[5].fromDigit == 1 && tubes[5].mix == 0);
  CHECK(tubes[0].digit == 0 && tubes[0].mix == 255);  // other tubes are not fading
  fader.update(digits(2), t0 + 100, fade);
  fader.fill(tubes);
  CHECK(tubes[5].mix >= 126 && tubes[5].mix <= 128 && tubes[5].fromDigit == 1);
  for (TubeState& s : tubes) s.color = {200, 200, 200};
  Frame frame = render(layout, tubes, 255);
  CHECK(frame[layout.index[5][1]].r > 90 && frame[layout.index[5][1]].r < 110);  // FW-DSP-001 two LEDs, each half
  CHECK(frame[layout.index[5][2]].r > 90 && frame[layout.index[5][2]].r < 110);
  fader.update(digits(2), t0 + 200, fade);
  fader.fill(tubes);
  CHECK(tubes[5].mix == 255 && tubes[5].fromDigit == kBlank);
  for (TubeState& s : tubes) s.color = {200, 200, 200};
  frame = render(layout, tubes, 255);
  CHECK((frame[layout.index[5][1]] == Rgb{0, 0, 0}) && (frame[layout.index[5][2]] == Rgb{200, 200, 200}));
  fader.update(digits(2), t0 + 200 + 0xFFFFFF00u, fade);  // nothing restarts when millis() comes round again
  fader.fill(tubes);
  CHECK(tubes[5].mix == 255);
  // To blank fades out alone (FW-FX-031, FW-FX-032).
  fader.update(digits(kBlank), 5000, fade);
  fader.fill(tubes);
  CHECK(tubes[5].digit == kBlank && tubes[5].fromDigit == 2 && tubes[5].mix == 0);
}

// FW-FX-036, FW-FX-070, FW-ATT-002, FW-ATT-003, FW-DSP-025
static void testComposeFrame() {
  const LedLayout layout = LedLayout::ct001();
  EffectEngine engine;
  Effect fx;
  CHECK(loadBuiltinPreset(0, fx));
  engine.apply(fx);
  Attention attention;
  DigitFader fader;
  const DisplayOptions options;
  const auto digits = timeDigits(12, 34, 56, true);

  FrameInfo info;
  Frame frame = composeFrame(layout, engine, attention, fader, options, digits, 0, 0, 255, info);
  CHECK(info.lit && !info.attention && info.brightness == 255 && info.digits == digits);
  CHECK((frame[layout.index[0][1]] == Rgb{0xff, 0x20, 0x30}));  // FW-FX-070 digit 1 of tube 0 in the wave's color
  int lit = 0;
  for (const Rgb& px : frame) lit += (px.r || px.g || px.b) ? 1 : 0;
  CHECK(lit == kTubes);  // FW-DSP-001

  // FW-FX-070: a flash replaces all six colors on any effect, and the effect then carries on as if
  // it had never stopped. The time goes on underneath; nothing is reset.
  attention.start({120, 100, 100}, 1000, 3000, false);
  frame = composeFrame(layout, engine, attention, fader, options, digits, 2000, 2000, 99, info);
  CHECK(info.attention && info.brightness == 99);
  for (const Rgb& c : info.colors) CHECK((c == Rgb{0, 255, 0}));  // FW-ATT-002
  frame = composeFrame(layout, engine, attention, fader, options, digits, 4000, 4000, 99, info);
  CHECK(!info.attention);
  for (int t = 0; t < kTubes; t++) CHECK(info.colors[t] == engine.tubeColor(t, 4000));  // FW-FX-070 resumes
  CHECK(!(info.colors[0] == info.colors[1]));

  // FW-ATT-003: the dark half of a blink: no LED at all, on a pulsing effect too.
  EffectEngine pulsing;
  CHECK(parseFx("{\"pattern\":\"solid\",\"pulse\":{\"min\":10,\"max\":100,\"period\":1000}}", nullptr, fx) == FxError::kNone);
  pulsing.apply(fx);
  attention.start({0, 100, 100}, 0, 3000, true);
  frame = composeFrame(layout, pulsing, attention, fader, options, digits, 600, 600, 200, info);
  CHECK(!info.lit && info.brightness == 200);  // FW-ATT-002 the saved brightness, pulse or not
  for (const Rgb& px : frame) CHECK((px == Rgb{0, 0, 0}));
  attention.cancel();

  // FW-FX-036: pulse scales the global brightness down, never up.
  composeFrame(layout, pulsing, attention, fader, options, digits, 500, 500, 200, info);  // envelope at 100
  CHECK(info.brightness == (200 * 101) >> 8);
  composeFrame(layout, pulsing, attention, fader, options, digits, 500, 500, 0, info);
  CHECK(info.brightness == 0);  // FW-DSP-009

  // FW-DSP-025: colors moving never move a digit.
  DigitFader steady;
  for (uint64_t t = 0; t < 5000; t += 50) {
    composeFrame(layout, engine, attention, steady, options, digits, t, static_cast<uint32_t>(t), 255, info);
    CHECK(info.digits == digits);
  }
  // FW-FX-032: hidden seconds still get the wave's colors, ready for when they show.
  DisplayOptions noSeconds;
  noSeconds.seconds = false;
  DigitFader hidden;
  composeFrame(layout, engine, attention, hidden, noSeconds, digits, 0, 0, 255, info);
  CHECK(info.digits[4] == kBlank && info.digits[5] == kBlank && !(info.colors[5] == Rgb{0, 0, 0}));

  // FW-FX-034: each tube scaled by its own level, on the effect and on a flash alike.
  EffectEngine white;
  CHECK(parseFx("{\"pattern\":\"solid\",\"palette\":[\"#c8c8c8\"],\"period\":0}", nullptr, fx) == FxError::kNone);
  white.apply(fx);
  DisplayOptions tamed;
  tamed.tubeBrightness = {{100, 50, 0, 100, 100, 25}};
  DigitFader tamedFader;
  frame = composeFrame(layout, white, attention, tamedFader, tamed, digits, 0, 0, 255, info);
  CHECK((info.colors[0] == Rgb{200, 200, 200}) && (info.colors[1] == Rgb{100, 100, 100}) && (info.colors[2] == Rgb{0, 0, 0}));
  CHECK((info.colors[5] == Rgb{50, 50, 50}));
  CHECK((frame[layout.index[1][digits[1]]] == Rgb{100, 100, 100}));
  attention.start({0, 0, 100}, 0, 3000, false);  // white
  composeFrame(layout, white, attention, tamedFader, tamed, digits, 10, 10, 255, info);
  CHECK((info.colors[0] == Rgb{255, 255, 255}) && (info.colors[1] == Rgb{127, 127, 127}) && (info.colors[2] == Rgb{0, 0, 0}));
  attention.cancel();
}

// FW-FX-016, FW-FX-017, FW-FX-033: accents, matrix rain, scramble and carry.
static void testAccents() {
  const LedLayout layout = LedLayout::ct001();
  Attention attention;
  const DisplayOptions options;
  FrameInfo info;
  auto at = [](int sec) { return timeDigits(12, 34, sec, true); };
  auto engineWith = [](const std::string& accent) {
    return engineFor("{\"pattern\":\"solid\",\"palette\":[\"#808080\"],\"period\":0,\"accent\":" + accent + "}");
  };
  const Rgb gray{0x80, 0x80, 0x80};

  // Dip: dark at the change, back by the end; only the tube that changed; nothing at boot.
  EffectEngine dip = engineWith("{\"style\":\"dip\",\"ms\":400}");
  DigitFader fader;
  composeFrame(layout, dip, attention, fader, options, at(56), 0, 1000, 255, info);
  for (const Rgb& c : info.colors) CHECK(c == gray);  // the first frame is no change
  composeFrame(layout, dip, attention, fader, options, at(57), 0, 2000, 255, info);
  CHECK(info.colors[5].r < 0x08 && (info.colors[4] == gray));
  composeFrame(layout, dip, attention, fader, options, at(57), 0, 2200, 255, info);
  CHECK(info.colors[5].r > 0x08 && info.colors[5].r < 0x80);
  composeFrame(layout, dip, attention, fader, options, at(57), 0, 2400, 255, info);
  CHECK((info.colors[5] == gray));

  // Flash: the accent color at once, fading back; strength scales it.
  EffectEngine flash = engineWith("{\"style\":\"flash\",\"ms\":400,\"color\":\"#ff0000\"}");
  DigitFader f2;
  composeFrame(layout, flash, attention, f2, options, at(1), 0, 0, 255, info);
  composeFrame(layout, flash, attention, f2, options, at(2), 0, 1000, 255, info);
  CHECK(info.colors[5].r > 0xf0 && info.colors[5].g < 0x10);
  composeFrame(layout, flash, attention, f2, options, at(2), 0, 1200, 255, info);
  CHECK(info.colors[5].r > 0x80 && info.colors[5].g < 0x80);
  EffectEngine half = engineWith("{\"style\":\"flash\",\"ms\":400,\"color\":\"#ff0000\",\"strength\":50}");
  DigitFader f3;
  composeFrame(layout, half, attention, f3, options, at(1), 0, 0, 255, info);
  composeFrame(layout, half, attention, f3, options, at(2), 0, 1000, 255, info);
  CHECK(info.colors[5].r > 0xb0 && info.colors[5].r < 0xd0 && info.colors[5].g > 0x30 && info.colors[5].g < 0x50);

  // Glow rises and falls; the accent color defaults to the palette's last stop.
  EffectEngine glow = engineFor("{\"pattern\":\"tubes\",\"palette\":[\"#000000\",\"#00ff00\"],\"accent\":{\"style\":\"glow\",\"ms\":400}}");
  DigitFader f4;
  composeFrame(layout, glow, attention, f4, options, at(1), 0, 0, 255, info);
  composeFrame(layout, glow, attention, f4, options, at(2), 0, 1000, 255, info);
  CHECK((info.colors[4] == Rgb{0, 0, 0}));  // tube 4 shows #000000 and did not change
  composeFrame(layout, glow, attention, f4, options, at(2), 0, 1200, 255, info);
  const Rgb glowMid = info.colors[5];
  CHECK((glowMid == Rgb{0, 255, 0}));  // tube 5 is green already: blending to green keeps it
  EffectEngine glowRed = engineWith("{\"style\":\"glow\",\"ms\":400,\"color\":\"#ff0000\"}");
  DigitFader f5;
  composeFrame(layout, glowRed, attention, f5, options, at(1), 0, 0, 255, info);
  composeFrame(layout, glowRed, attention, f5, options, at(2), 0, 1000, 255, info);
  CHECK(near(info.colors[5], gray, 3));
  composeFrame(layout, glowRed, attention, f5, options, at(2), 0, 1200, 255, info);
  CHECK(info.colors[5].r > 0xf0 && info.colors[5].g < 0x10);  // the top of the bell

  // Flicker changes from one 40 ms step to the next, and calms down by the end.
  EffectEngine flicker = engineWith("{\"style\":\"flicker\",\"ms\":600}");
  bool varied = false;
  Rgb last{};
  for (uint32_t age = 0; age < 300; age += 40) {
    const Rgb c = flicker.accented(5, 3000 + age, gray, age);
    varied = varied || (age && !(c == last));
    last = c;
  }
  CHECK(varied);
  CHECK(near(flicker.accented(5, 9999, gray, 599), gray, 2));

  // Shift moves along the palette and comes back.
  EffectEngine shift = engineFor("{\"pattern\":\"solid\",\"palette\":[\"#ff0000\",\"#0000ff\"],\"interpolate\":\"rgb\",\"period\":0,\"accent\":{\"style\":\"shift\",\"ms\":400}}");
  CHECK(near(shift.accented(0, 0, Rgb{255, 0, 0}, 200), Rgb{0, 0, 255}, 6));  // half a cycle at the top
  CHECK(near(shift.accented(0, 0, Rgb{255, 0, 0}, 399), Rgb{255, 0, 0}, 4));

  // Tubes mask: hm ignores the seconds; a minute change still triggers.
  EffectEngine hm = engineWith("{\"style\":\"dip\",\"ms\":400,\"tubes\":\"hm\"}");
  DigitFader f6;
  composeFrame(layout, hm, attention, f6, options, timeDigits(12, 34, 58, true), 0, 0, 255, info);
  composeFrame(layout, hm, attention, f6, options, timeDigits(12, 34, 59, true), 0, 1000, 255, info);
  for (const Rgb& c : info.colors) CHECK(c == gray);
  composeFrame(layout, hm, attention, f6, options, timeDigits(12, 35, 0, true), 0, 2000, 255, info);
  CHECK(info.colors[3].r < 0x08 && (info.colors[2] == gray));
  CHECK((info.colors[4] == gray) && (info.colors[5] == gray));  // the seconds changed too, but are masked

  // Spread: the accent reaches each neighbour `spread` ms later; a time jump sets off several at once.
  EffectEngine ripple = engineWith("{\"style\":\"dip\",\"ms\":300,\"tubes\":\"000001\",\"spread\":100}");
  DigitFader f7;
  composeFrame(layout, ripple, attention, f7, options, at(1), 0, 0, 255, info);
  composeFrame(layout, ripple, attention, f7, options, at(2), 0, 1000, 255, info);
  CHECK(info.colors[5].r < 0x08 && (info.colors[4] == gray));
  composeFrame(layout, ripple, attention, f7, options, at(2), 0, 1100, 255, info);
  CHECK(info.colors[4].r < 0x08 && (info.colors[3] == gray) && info.colors[5].r > 0x08);
  composeFrame(layout, ripple, attention, f7, options, at(2), 0, 1400, 255, info);
  CHECK(info.colors[1].r < 0x08 && (info.colors[5] == gray));  // four tubes away, 400 ms on

  // No accent during a flash; hidden seconds never trigger.
  DigitFader f8;
  composeFrame(layout, dip, attention, f8, options, at(1), 0, 0, 255, info);
  attention.start({0, 0, 100}, 900, 3000, false);
  composeFrame(layout, dip, attention, f8, options, at(2), 0, 1000, 255, info);
  CHECK((info.colors[5] == Rgb{255, 255, 255}));
  attention.cancel();
  DisplayOptions noSeconds;
  noSeconds.seconds = false;
  DigitFader f9;
  composeFrame(layout, dip, attention, f9, noSeconds, at(1), 0, 0, 255, info);
  composeFrame(layout, dip, attention, f9, noSeconds, at(2), 0, 1000, 255, info);
  for (const Rgb& c : info.colors) CHECK(c == gray);

  // FW-FX-033 scramble: random digits for fadeMs, then the new one; info.digits is the time throughout.
  DisplayOptions scramble;
  scramble.transition = FxTransition::kScramble;
  scramble.fadeMs = 500;
  EffectEngine plain = engineFor("{\"pattern\":\"solid\",\"period\":0}");
  DigitFader sf;
  composeFrame(layout, plain, attention, sf, scramble, at(1), 0, 0, 255, info);
  bool scrambled = false;
  for (uint32_t ms = 1000; ms < 1500; ms += 50) {
    composeFrame(layout, plain, attention, sf, scramble, at(2), 0, ms, 255, info);
    CHECK(info.digits[5] == 2 && info.shown[5] >= 0 && info.shown[5] <= 9);
    scrambled = scrambled || info.shown[5] != 2;
    CHECK(info.shown[4] == 0);  // the tube that did not change stays put
  }
  CHECK(scrambled);
  composeFrame(layout, plain, attention, sf, scramble, at(2), 0, 1500, 255, info);
  CHECK(info.shown[5] == 2);

  // Carry: 5 to 0 counts 6, 7, 8, 9, 0 over fadeMs.
  DisplayOptions carry;
  carry.transition = FxTransition::kCarry;
  carry.fadeMs = 500;
  DigitFader cf;
  composeFrame(layout, plain, attention, cf, carry, at(55), 0, 0, 255, info);
  const int8_t want[] = {6, 7, 8, 9, 0};
  for (int k = 0; k < 5; k++) {
    composeFrame(layout, plain, attention, cf, carry, at(50), 0, 1000 + static_cast<uint32_t>(k) * 100 + 10, 255, info);
    CHECK(info.shown[5] == want[k]);
  }
  composeFrame(layout, plain, attention, cf, carry, at(50), 0, 1600, 255, info);
  CHECK(info.shown[5] == 0 && info.shown[4] == 5);

  // FW-FX-017 matrix: drops leave the background for the head, then fade back; glyphs only on drops,
  // only with glyphs on, and never on a hidden tube.
  EffectEngine rain = engineFor("{\"pattern\":\"matrix\",\"palette\":[\"#001000\",\"#00c000\",\"#c0ffc0\"],\"interpolate\":\"rgb\",\"period\":1500,\"density\":60,\"glyphs\":true}");
  int heads = 0, glyphs = 0, background = 0;
  for (uint64_t t = 0; t < 60000; t += 50) {
    for (int i = 0; i < kTubes; i++) {
      const Rgb c = rain.tubeColor(i, t);
      CHECK(c.g >= 0x10 && c.r <= 0xc0);
      heads += (c == Rgb{0xc0, 0xff, 0xc0}) ? 1 : 0;
      background += (c == Rgb{0x00, 0x10, 0x00}) ? 1 : 0;
      if (rain.glyph(i, t) != kNoGlyph) {
        glyphs++;
        CHECK(!(c == Rgb{0x00, 0x10, 0x00}));  // a glyph is always on a drop
      }
    }
  }
  CHECK(heads > 50 && glyphs > heads && background > 1000);
  EffectEngine dry = engineFor("{\"pattern\":\"matrix\",\"period\":1500,\"density\":100}");
  for (uint64_t t = 0; t < 5000; t += 50) CHECK(dry.glyph(0, t) == kNoGlyph);
  DigitFader mf;
  bool covered = false;
  for (uint32_t ms = 0; ms < 20000; ms += 50) {
    composeFrame(layout, rain, attention, mf, noSeconds, at(1), ms, ms, 255, info);
    CHECK(info.shown[4] == kBlank && info.shown[5] == kBlank && info.digits[0] == 1);
    covered = covered || info.shown[0] != 1 || info.shown[1] != 2;
  }
  CHECK(covered);
}

// FW-MCP-036, HOST-LIB-020: the expected values come from src/color.ts's hexToHsv.
static void testParseColor() {
  struct Expected {
    const char* name;
    Hsv hsv;
  };
  const Expected expected[kNamedColorCount] = {
      {"red", {0, 100, 100}},     {"orange", {25, 100, 100}}, {"yellow", {49, 100, 100}}, {"green", {120, 100, 100}},
      {"cyan", {180, 100, 100}},  {"blue", {240, 100, 100}},  {"purple", {270, 100, 100}}, {"magenta", {300, 100, 100}},
      {"pink", {340, 75, 100}},   {"white", {0, 0, 100}},
  };
  for (int i = 0; i < kNamedColorCount; i++) {
    Hsv c{};
    CHECK(strcmp(kNamedColors[i].name, expected[i].name) == 0);  // FW-MCP-036 same table and order
    CHECK(parseColor(expected[i].name, c));
    CHECK(c.h == expected[i].hsv.h && c.s == expected[i].hsv.s && c.v == expected[i].hsv.v);
  }
  Hsv c{};
  CHECK(parseColor("  ORANGE ", c) && c.h == 25);  // FW-MCP-036 names ignore case and spaces
  CHECK(parseColor("#f00", c) && c.h == 0 && c.s == 100 && c.v == 100);
  CHECK(parseColor("00FF00", c) && c.h == 120);  // '#' optional, as in color.ts
  CHECK(parseColor("#808080", c) && c.s == 0 && c.v == 50);
  CHECK(parseColor("30,100,50", c) && c.h == 30 && c.s == 100 && c.v == 50);
  CHECK(parseColor(" 360 , 0 , 100 ", c) && c.h == 360);
  CHECK(!parseColor("361,0,0", c));
  CHECK(!parseColor("0,101,0", c));
  CHECK(!parseColor("1.5,2,3", c));  // whole numbers only on the clock
  CHECK(!parseColor("1,,3", c));
  CHECK(!parseColor("1,2,3,4", c));
  CHECK(!parseColor("#12345", c));
  CHECK(!parseColor("#ggg", c));
  CHECK(!parseColor("blueish", c));
  CHECK(!parseColor("", c));
  CHECK(!parseColor(nullptr, c));

  char hex[8];
  formatHex({255, 106, 0}, hex);
  CHECK(strcmp(hex, "#ff6a00") == 0);
  CHECK(parseColor("orange", c));
  formatHex(hsvToRgb({c.h, c.s, 100}), hex);
  CHECK(strcmp(hex, "#ff6a00") == 0);  // FW-MCP-030 the example's reply color
}

// FW-ATT-010: two 80 ms pulses 80 ms apart, once, wrap-safe.
static void testBeepPattern() {
  BeepPattern b;
  CHECK(!b.isActive(0) && !b.isOn(0));
  const uint32_t t0 = 0xFFFFFFF0u;  // straddles the millis() rollover
  b.start(t0);
  CHECK(b.isOn(t0) && b.isOn(t0 + 79));
  CHECK(!b.isOn(t0 + 80) && !b.isOn(t0 + 159) && b.isActive(t0 + 100));
  CHECK(b.isOn(t0 + 160) && b.isOn(t0 + 239));
  CHECK(!b.isOn(t0 + 240) && !b.isActive(t0 + 240));
  CHECK(!b.isOn(t0 + 320));  // never repeats
  b.start(100);
  b.cancel();
  CHECK(!b.isOn(100));
}

// FW-MCP-010, FW-MCP-011
static void testMcpHostOrigin() {
  const char* ip = "192.168.1.50";
  const char* hn = "tubeclock";
  CHECK(mcpHostAllowed("tubeclock.local", ip, hn));
  CHECK(mcpHostAllowed("TubeClock.Local", ip, hn));
  CHECK(mcpHostAllowed("tubeclock", ip, hn));
  CHECK(mcpHostAllowed("tubeclock.local:9080", ip, hn));  // port stripped
  CHECK(mcpHostAllowed("192.168.1.50", ip, hn));
  CHECK(mcpHostAllowed("192.168.1.50:80", ip, hn));
  CHECK(!mcpHostAllowed("evil.example", ip, hn));
  CHECK(!mcpHostAllowed("tubeclock.local.evil.example", ip, hn));
  CHECK(!mcpHostAllowed("tubeclock.localx", ip, hn));
  CHECK(!mcpHostAllowed("xtubeclock", ip, hn));
  CHECK(!mcpHostAllowed("tubeclock:", ip, hn));
  CHECK(!mcpHostAllowed("192.168.1.5", ip, hn));
  CHECK(!mcpHostAllowed("localhost:9080", ip, hn));
  CHECK(!mcpHostAllowed("", ip, hn));
  CHECK(!mcpHostAllowed(nullptr, ip, hn));

  CHECK(mcpOriginAllowed("http://tubeclock.local", ip, hn));
  CHECK(mcpOriginAllowed("http://192.168.1.50:80", ip, hn));
  CHECK(!mcpOriginAllowed("https://tubeclock.local", ip, hn));
  CHECK(!mcpOriginAllowed("http://evil.example", ip, hn));
  CHECK(!mcpOriginAllowed("http://tubeclock.local/", ip, hn));
  CHECK(!mcpOriginAllowed("null", ip, hn));
  CHECK(!mcpOriginAllowed("http://", ip, hn));

  McpHeaders h;
  CHECK(mcpCheckHostAndOrigin(h, ip, hn) == 403);  // no Host at all
  const char* host = "Host: tubeclock.local";
  h.addLine(host, strlen(host));
  CHECK(mcpCheckHostAndOrigin(h, ip, hn) == 0);  // FW-MCP-011 no Origin is fine
  const char* origin = "origin:   http://evil.example  ";
  h.addLine(origin, strlen(origin));
  CHECK(strcmp(h.origin, "http://evil.example") == 0);
  CHECK(mcpCheckHostAndOrigin(h, ip, hn) == 403);  // FW-MCP-011 FW-SEC-012

  h.reset();
  h.addLine(host, strlen(host));
  h.addLine(host, strlen(host));
  CHECK(mcpCheckHostAndOrigin(h, ip, hn) == 403);  // two Host headers are refused
  h.reset();
  std::string longHost = "Host: " + std::string(100, 'a');
  h.addLine(longHost.c_str(), longHost.size());
  CHECK(h.hostTooLong && mcpCheckHostAndOrigin(h, ip, hn) == 403);
}

// FW-MCP-007, FW-MCP-012, FW-BUD-004, FW-SEC-013
static void testMcpRequestChecks() {
  auto headers = [](std::initializer_list<const char*> lines) {
    McpHeaders h;
    for (const char* l : lines) h.addLine(l, strlen(l));
    return h;
  };
  CHECK(mcpCheckRequest(headers({"Content-Length: 10"})) == 0);  // FW-MCP-007 no version header
  CHECK(mcpCheckRequest(headers({"Content-Length: 10", "MCP-Protocol-Version: 2025-06-18"})) == 0);
  CHECK(mcpCheckRequest(headers({"Content-Length: 10", "mcp-protocol-version: 2025-11-25"})) == 0);
  CHECK(mcpCheckRequest(headers({"Content-Length: 10", "MCP-Protocol-Version: 2026-07-28"})) == 400);
  CHECK(mcpCheckRequest(headers({"Content-Length: 10", "MCP-Protocol-Version:"})) == 400);
  CHECK(mcpCheckRequest(headers({"Content-Length: 2048"})) == 0);
  CHECK(mcpCheckRequest(headers({"Content-Length: 2049"})) == 413);  // FW-MCP-012
  CHECK(mcpCheckRequest(headers({"Content-Length: 99999999999"})) == 400);
  CHECK(mcpCheckRequest(headers({"Content-Length: 12a"})) == 400);
  CHECK(mcpCheckRequest(headers({"Content-Length: 5", "Content-Length: 6"})) == 400);
  CHECK(mcpCheckRequest(headers({})) == 411);
  CHECK(mcpCheckRequest(headers({"Transfer-Encoding: chunked"})) == 411);

  // FW-SEC-013, FW-BUD-004: every other route with a body the web server would read.
  CHECK(checkBodyHeaders(headers({}), 1024) == 0);  // no length, no body
  CHECK(checkBodyHeaders(headers({"Content-Length: 0"}), 1024) == 0);
  CHECK(checkBodyHeaders(headers({"Content-Length: 1024"}), 1024) == 0);
  CHECK(checkBodyHeaders(headers({"Content-Length: 1025"}), 1024) == 413);
  CHECK(checkBodyHeaders(headers({"content-length: 99999"}), 1024) == 413);
  CHECK(checkBodyHeaders(headers({"Content-Length: 12a"}), 1024) == 411);
  CHECK(checkBodyHeaders(headers({"Content-Length: -1"}), 1024) == 411);
  CHECK(checkBodyHeaders(headers({"Content-Length: 5", "Content-Length: 6"}), 1024) == 411);
  CHECK(checkBodyHeaders(headers({"Transfer-Encoding: chunked"}), 1024) == 411);
  CHECK(checkBodyHeaders(headers({"Content-Type: multipart/form-data; boundary=x"}), 1024) == 415);
  CHECK(checkBodyHeaders(headers({"content-type: Multipart/mixed", "Content-Length: 5"}), 1024) == 415);
  CHECK(checkBodyHeaders(headers({"Content-Type: application/json", "Content-Length: 5"}), 1024) == 0);
  {
    // A block of lines reads the way the server's parser will, stray CR included.
    auto block = [](const std::string& head) {
      McpHeaders h;
      h.addLines(head.data(), head.size());
      return h;
    };
    McpHeaders b = block("Host: a\r\nContent-Length: 7");
    CHECK(b.hostSeen && b.contentLength == 7);
    CHECK(block("").contentLength == -1);
    // The server ends a line at the CR and drops up to the LF, so the length below is the one it obeys.
    CHECK(block("X: y\rZ\nContent-Length: 99999").contentLength == 99999);
    CHECK(checkBodyHeaders(block("X: y\rZ\nContent-Length: 99999"), 1024) == 413);
    CHECK(block("X: y\nContent-Length: 99999").contentLength == -1);  // and a bare LF does not end one
  }

  McpHeaders h = headers({"Authorization: Bearer abc"});
  CHECK(h.authorizationLen == 10 && bearerMatches(h.authorization, h.authorizationLen, "abc"));  // FW-MCP-009
  std::string longAuth = "Authorization: Bearer " + std::string(200, 'x');
  h.addLine(longAuth.c_str(), longAuth.size());
  CHECK(h.authorizationTooLong && h.authorizationLen == 0);

  CHECK(isSupportedMcpVersion("2025-03-26") && isSupportedMcpVersion("2025-06-18") && isSupportedMcpVersion("2025-11-25"));
  CHECK(strcmp(negotiateMcpVersion("2025-06-18"), "2025-06-18") == 0);  // FW-MCP-021
  CHECK(strcmp(negotiateMcpVersion("2099-01-01"), "2025-11-25") == 0);
  CHECK(strcmp(negotiateMcpVersion(nullptr), "2025-11-25") == 0);
  CHECK(strcmp(kMcpAssumedVersion, "2025-03-26") == 0);  // FW-MCP-007

  // FW-MCP-007, FW-MCP-020: names the versions, and is not a modern -32022 (a dual-era client must
  // fall back to initialize on it).
  char body[192];
  const size_t n = writeMcpVersionError(body, sizeof(body));
  CHECK(n > 0 && strstr(body, "2025-03-26 2025-06-18 2025-11-25") != nullptr && strstr(body, "-32600") != nullptr);
  CHECK(strstr(body, "-32022") == nullptr);
}

struct FakeDevice : McpDevice {
  int starts = 0, cancels = 0;
  AttentionCall last{};
  bool buzzer = false;
  std::string log;
  void startAttention(const AttentionCall& call) override {
    starts++;
    last = call;
  }
  void cancelAttention() override { cancels++; }
  const char* state = "{\"version\":\"1.2.3\",\"time\":\"13:05:09\",\"attention\":false,\"tubes\":[{\"h\":241,\"s\":100}]}";
  size_t writeState(char* out, size_t cap) override {
    const size_t len = strlen(state);
    if (len + 1 > cap) return 0;
    memcpy(out, state, len + 1);
    return len;
  }
  bool hasBuzzer() const override { return buzzer; }
  void logToolCall(const char* tool, const char* summary) override {
    log = std::string(tool) + " " + summary;
  }
};

struct McpResult {
  int status;
  std::string body;
  JsonDocument json;
};

static McpResult mcp(FakeDevice& device, const std::string& body, size_t arenaLen = kMcpArenaBytes) {
  static uint8_t arena[kMcpArenaBytes];
  static char out[kMcpMaxResponseBytes];
  const McpReply r = handleMcpMessage(body.data(), body.size(), "1.2.3", device, arena, arenaLen, out, sizeof(out));
  McpResult res{r.status, std::string(out, r.length), JsonDocument()};
  CHECK(r.length < kMcpMaxResponseBytes);  // FW-BUD-005
  if (r.length) CHECK(!deserializeJson(res.json, res.body));  // every body is valid JSON
  return res;
}

static std::string toolCall(const char* name, const char* args, const char* id = "7") {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id + ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
         "\"" + (args ? std::string(",\"arguments\":") + args : std::string()) + "}}";
}

static std::string textOf(const McpResult& r) { return r.json["result"]["content"][0]["text"].as<std::string>(); }

// FW-MCP-003..005, FW-MCP-012, FW-MCP-020..027
static void testMcpProtocol() {
  FakeDevice d;
  for (const char* v : kMcpVersions) {  // FW-MCP-020, FW-MCP-021
    McpResult r = mcp(d, std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"") + v +
                             "\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");
    CHECK(r.status == 200 && r.json["id"] == 1);  // FW-MCP-004
    CHECK(r.json["result"]["protocolVersion"] == v);
    CHECK(r.json["result"]["capabilities"]["tools"].is<JsonObject>());
    CHECK(r.json["result"]["capabilities"]["tools"].size() == 0);  // no listChanged
    CHECK(r.json["result"]["serverInfo"]["name"] == "tubeclock" && r.json["result"]["serverInfo"]["version"] == "1.2.3");
    CHECK(strstr(r.json["result"]["instructions"] | "", "sparingly") != nullptr);
  }
  McpResult r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2099-01-01\"}}");
  CHECK(r.json["result"]["protocolVersion"] == "2025-11-25");  // FW-MCP-021 newest when unsupported
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
  CHECK(r.status == 200 && r.json["error"]["code"] == -32602);

  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  CHECK(r.status == 202 && r.body.empty());  // FW-MCP-022, FW-MCP-005
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":1}}");
  CHECK(r.status == 202 && r.body.empty());
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{}}");
  CHECK(r.status == 202 && r.body.empty());  // FW-MCP-005 a client response
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-1,\"message\":\"x\"}}");
  CHECK(r.status == 202);

  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":\"p-1\",\"method\":\"ping\"}");
  CHECK(r.status == 200 && r.body == "{\"jsonrpc\":\"2.0\",\"id\":\"p-1\",\"result\":{}}");  // FW-MCP-023, FW-MCP-027

  r = mcp(d, " [{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32600 && r.json["id"].isNull());  // FW-MCP-003
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32700 && r.json["id"].isNull());  // FW-MCP-012
  r = mcp(d, "");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32700);
  r = mcp(d, "\"ping\"");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32600);
  r = mcp(d, "{\"id\":1,\"method\":\"ping\"}");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32600);  // no "jsonrpc":"2.0"
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}");
  CHECK(r.status == 400 && r.json["error"]["code"] == -32600);  // FW-MCP-027 ids are strings or integers
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"ping\"}");
  CHECK(r.status == 400);
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":\"" + std::string(65, 'i') + "\",\"method\":\"ping\"}");
  CHECK(r.status == 400);
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":9007199254740993,\"method\":\"ping\"}");
  CHECK(r.status == 200 && r.body.find("\"id\":9007199254740993,") != std::string::npos);
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"pad\":\"" + std::string(1900, 'x') + "\"}}");
  CHECK(r.status == 200);  // a full-size body fits the arena
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"pad\":\"" + std::string(400, 'x') + "\"}}", 256);
  CHECK(r.status == 413);  // more than the arena holds

  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/list\"}");
  CHECK(r.status == 200 && r.json["error"]["code"] == -32601 && r.json["id"] == 2);  // FW-MCP-026, FW-MCP-027
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/list\"}");
  CHECK(r.json["error"]["code"] == -32601);
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"logging/setLevel\",\"params\":{\"level\":\"info\"}}");
  CHECK(r.json["error"]["code"] == -32601);
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"nope\"}");
  CHECK(r.json["error"]["code"] == -32601);

  // FW-MCP-024, FW-MCP-030..034
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{\"cursor\":\"x\"}}");
  CHECK(r.status == 200 && r.json["result"]["nextCursor"].isNull());
  JsonArrayConst tools = r.json["result"]["tools"];
  CHECK(tools.size() == 3);  // FW-MCP-033 attention and status only
  const char* names[3] = {"get_attention", "cancel_attention", "clock_status"};
  for (size_t i = 0; i < tools.size() && i < 3; i++) {
    JsonObjectConst t = tools[i];
    CHECK(t["name"] == names[i]);
    CHECK(t["description"].is<const char*>());
    JsonObjectConst s = t["inputSchema"];  // FW-MCP-034
    CHECK(s["type"] == "object" && s["properties"].is<JsonObjectConst>() && s["required"].is<JsonArrayConst>());
    CHECK(s["additionalProperties"] == false);
  }
  JsonObjectConst props = tools[0]["inputSchema"]["properties"];
  CHECK(props["color"]["default"] == "orange" && props["durationSeconds"]["maximum"] == 30);
  CHECK(props["beep"]["default"] == false && props["message"]["maxLength"] == 200);
  CHECK(tools[0]["annotations"]["idempotentHint"] == true && tools[0]["annotations"]["destructiveHint"] == false);
  CHECK(tools[1]["annotations"]["idempotentHint"] == true);
  CHECK(tools[2]["annotations"]["readOnlyHint"] == true);

  r = mcp(d, toolCall("set_brightness", "{}"));
  CHECK(r.json["error"]["code"] == -32602 && r.json["id"] == 7);  // FW-MCP-025, FW-MCP-033
  r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{}}");
  CHECK(r.json["error"]["code"] == -32602);
  r = mcp(d, toolCall("get_attention", "[1]"));
  CHECK(r.json["error"]["code"] == -32602);
}

// FW-MCP-013, FW-MCP-025, FW-MCP-030..032, FW-MCP-035, FW-ATT-010
static void testMcpTools() {
  FakeDevice d;
  // The example exchange in mcp.md.
  McpResult r = mcp(d, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_attention\",\"arguments\":{\"color\":\"red\",\"durationSeconds\":3}}}");
  CHECK(r.body == "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Clock is showing #ff0000 for 3 s.\"}]}}");
  CHECK(d.starts == 1 && d.last.durationMs == 3000 && d.last.color.h == 0 && !d.last.blink && !d.last.beep);
  CHECK(d.log == "get_attention #ff0000 3000ms blink=0 beep=0");  // FW-MCP-013

  r = mcp(d, toolCall("get_attention", nullptr));  // FW-MCP-030 defaults
  CHECK(textOf(r) == "Clock is showing #ff6a00 for 5 s." && d.last.durationMs == 5000 && d.starts == 2);
  r = mcp(d, toolCall("get_attention", "{\"color\":\"#00f\",\"durationSeconds\":2.5,\"blink\":true}"));
  CHECK(textOf(r) == "Clock is showing #0000ff, blinking, for 2.5 s." && d.last.blink && d.last.durationMs == 2500);
  CHECK(d.starts == 3);  // FW-MCP-035 each call replaces the flash (the device starts anew)

  // FW-ATT-010: beep is accepted either way; the reply says whether a buzzer sounded it.
  r = mcp(d, toolCall("get_attention", "{\"color\":\"green\",\"beep\":true}"));
  CHECK(d.last.beep && textOf(r) == "Clock is showing #00ff00 for 5 s. No beep: this clock has no buzzer configured.");
  d.buzzer = true;
  r = mcp(d, toolCall("get_attention", "{\"color\":\"green\",\"beep\":true,\"blink\":true}"));
  CHECK(textOf(r) == "Clock is showing #00ff00, blinking, with a beep, for 5 s.");
  CHECK(d.log == "get_attention #00ff00 5000ms blink=1 beep=1");

  r = mcp(d, toolCall("get_attention", "{\"color\":\"30,100,40\",\"message\":\"build \\\"done\\\"\\n\\u00e9!\"}"));
  CHECK(textOf(r) == "Clock is showing #ff8000 for 5 s.");  // v is the saved brightness's job
  // FW-MCP-013: the message is logged on one printable line (quote, newline and é become '?').
  CHECK(d.log == "get_attention #ff8000 5000ms blink=0 beep=0 \"build ?done??" "?!\"");

  // FW-MCP-025: bad values are tool errors and start nothing; wrong types are protocol errors.
  const int before = d.starts;
  r = mcp(d, toolCall("get_attention", "{\"color\":\"chartreuse\"}"));
  CHECK(r.json["result"]["isError"] == true && textOf(r).find("Invalid color") == 0);
  CHECK(d.log == "get_attention refused: bad argument value");  // FW-MCP-013 refused calls are logged too
  r = mcp(d, toolCall("get_attention", "{\"durationSeconds\":31}"));
  CHECK(r.json["result"]["isError"] == true);
  r = mcp(d, toolCall("get_attention", "{\"durationSeconds\":0.5}"));
  CHECK(r.json["result"]["isError"] == true);
  r = mcp(d, toolCall("get_attention", ("{\"message\":\"" + std::string(201, 'm') + "\"}").c_str()));
  CHECK(r.json["result"]["isError"] == true);
  r = mcp(d, toolCall("get_attention", "{\"durationSeconds\":\"5\"}"));
  CHECK(r.json["error"]["code"] == -32602);
  r = mcp(d, toolCall("get_attention", "{\"color\":255}"));
  CHECK(r.json["error"]["code"] == -32602);
  CHECK(d.log == "get_attention refused: wrong argument type");  // FW-MCP-013
  r = mcp(d, toolCall("get_attention", "{\"blink\":1}"));
  CHECK(r.json["error"]["code"] == -32602);
  r = mcp(d, toolCall("get_attention", "{\"beep\":\"yes\"}"));
  CHECK(r.json["error"]["code"] == -32602);
  CHECK(d.starts == before);
  r = mcp(d, toolCall("get_attention", ("{\"message\":\"" + std::string(200, 'm') + "\"}").c_str()));
  CHECK(r.json["result"]["isError"].isNull() && d.starts == before + 1);  // exactly 200 is fine

  r = mcp(d, toolCall("cancel_attention", "{}"));  // FW-MCP-031
  CHECK(textOf(r) == "Attention cancelled." && d.cancels == 1 && d.log == "cancel_attention ");
  r = mcp(d, toolCall("cancel_attention", nullptr));
  CHECK(textOf(r) == "Attention cancelled." && d.cancels == 2);

  r = mcp(d, toolCall("clock_status", nullptr));  // FW-MCP-032 pretty JSON of the state
  const std::string text = textOf(r);
  JsonDocument state;
  CHECK(!deserializeJson(state, text) && state["version"] == "1.2.3" && state["tubes"][0]["h"] == 241);
  CHECK(text.find('\n') != std::string::npos);

  // FW-MCP-032, FW-FX-069, FW-BUD-003: the largest state there can be fits the request's arena, laid
  // out exactly as ArduinoJson would have, tricky strings included.
  const std::string big =
      "{\"version\":\"0.2.0-123-gabcdef12-dirty\",\"hostname\":\"tubeclock-with-a-long-name\",\"time\":\"13:05:09\","
      "\"tz\":\"<+0530>-5:30,M3.2.0/02:00:00,M11.1.0/02:00\",\"is24h\":false,\"brightness\":255,"
      "\"preset\":\"a preset name of 24 char\",\"effect\":{\"pattern\":\"gradient\"},"
      "\"display\":{\"hours\":12,\"leadingZero\":false,\"seconds\":false,\"transition\":\"crossfade\"},\"attention\":true,"
      "\"tubes\":[{\"h\":360,\"s\":100},{\"h\":360,\"s\":100},{\"h\":360,\"s\":100},{\"h\":360,\"s\":100},{\"h\":360,\"s\":100},"
      "{\"h\":360,\"s\":100}],\"lastSync\":\"2026-11-03T17:04:12Z\",\"rejectedSyncs\":4294967295,\"e\":[],\"f\":{},"
      "\"esc\":\"a\\\"b:,{[\\\\\"}";
  d.state = big.c_str();
  r = mcp(d, toolCall("clock_status", nullptr));
  JsonDocument expect;
  CHECK(!deserializeJson(expect, big));
  std::string pretty;
  serializeJsonPretty(expect, pretty);
  CHECK(r.json["result"]["isError"].isNull() && textOf(r) == pretty);  // FW-MCP-032
}

int main() {
  testParseColor();
  testParseColorRgb();
  testEffectParse();
  testEffectMath();
  testEffectPatterns();
  testPresets();
  testDisplayOptions();
  testAccents();
  testComposeFrame();
  testLastSyncReference();
  testBeepPattern();
  testMcpHostOrigin();
  testMcpRequestChecks();
  testMcpProtocol();
  testMcpTools();
  testHsvToRgb();
  testScale();
  testTimeDigits();
  testTubeDigits();
  testLayoutAndRender();
  testUpdateProgress();
  testBearer();
  testBackoff();
  testAttention();
  testSettingsRoundTrip();
  testSameSettings();
  testValidation();
  testNtpServers();
  testJudgeSync();
  testTimeSync();
  testRtc();
  testButtons();
  testPins();
  testAnsweringServer();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("All core tests passed\n");
  return 0;
}
