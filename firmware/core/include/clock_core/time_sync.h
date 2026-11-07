#pragma once

#include <cstddef>
#include <cstdint>

namespace clock_core {

// ---- NTP server list (FW-TIME-010) ----

/// lwIP's SNTP_MAX_SERVERS on the ESP8266 core.
constexpr int kMaxNtpServers = 3;
constexpr size_t kMaxNtpNameLen = 63;

/// Entries in a comma-separated server list, for a compile-time check of TUBECLOCK_NTP_SERVERS.
constexpr int ntpServerCount(const char* list) {
  if (!list || !*list) return 0;
  int n = 1;
  for (const char* p = list; *p; p++) n += *p == ',' ? 1 : 0;
  return n;
}

/// Splits "a,b,c" into up to kMaxNtpServers names. Returns the count, or 0 when the list is empty,
/// has an empty entry, a name over kMaxNtpNameLen, or more than kMaxNtpServers entries.
int splitNtpServers(const char* list, char (&out)[kMaxNtpServers][kMaxNtpNameLen + 1]);

// ---- Sync plausibility (FW-TIME-013, FW-TIME-014, FW-TIME-017) ----

constexpr int64_t kUsPerSec = 1000000;
/// A jump larger than this against a clock with a reference needs a second agreeing answer.
constexpr int64_t kMaxJumpUs = 24LL * 3600 * kUsPerSec;
/// How close the confirming answer must be to the first rejected one.
constexpr int64_t kAgreeUs = 10 * kUsPerSec;

enum class SyncVerdict : uint8_t { kAccept, kReject, kNeedConfirmation };
enum class SyncReason : uint8_t { kNone, kBeforeBuild, kJump, kDisagree, kConfirmed };

struct SyncDecision {
  SyncVerdict verdict;
  SyncReason reason;
};

/// Short name for the serial log: "before-build", "jump", "disagree", "confirmed" or "ok".
const char* syncReasonName(SyncReason reason);

/// Judges one NTP answer. All times are microseconds since the Unix epoch at the same instant:
/// `candidateUs` the answer, `currentUs` the clock before it, `pendingUs` an earlier rejected
/// answer projected to now (nullptr if none). Below the build epoch is always rejected. `hasReference`
/// says `currentUs` can be believed: the RTC was valid at boot (FW-TIME-014) or an answer has been
/// accepted since (FW-TIME-017). Without one everything else is accepted; with one, a jump over 24 h
/// needs a second answer within 10 s of the first. kReject with kDisagree means "replace the pending
/// value".
SyncDecision judgeSync(int64_t candidateUs, int64_t currentUs, int64_t buildEpochUs, bool hasReference,
                       const int64_t* pendingUs);

/// Whether a wall-clock time counts as known: not before the build (FW-TIME-004, FW-TIME-013).
inline bool isTimeKnown(int64_t epochSec, int64_t buildEpochSec) { return epochSec >= buildEpochSec; }

/// FW-API-037: where the current time came from.
enum class TimeSource : uint8_t { kNone, kRtc, kNtp };

/// The state's `timeSource` value: "none", "rtc" or "ntp".
const char* timeSourceName(TimeSource source);

/// Keeps the state around judgeSync: what the clock should read had no answer arrived (a reference
/// plus the monotonic micros64() counter, so a rejected answer can be undone), the pending answer,
/// and the health counters reported in the state (FW-TIME-016). No Arduino dependency.
class TimeSync {
 public:
  /// lwIP's own retry floor; a rejected answer is retried this long after, doubling to the hourly
  /// resync interval while rejections continue.
  static constexpr uint32_t kRetryMinMs = 15000;
  static constexpr uint32_t kResyncMs = 3600000;

  explicit TimeSync(int64_t buildEpochSec) : buildEpochUs_(buildEpochSec * kUsPerSec) {}

  /// Starts tracking at boot. `rtcUs` is the RTC's time when it read as running, else nullptr.
  /// Returns true when the RTC time passes the build-epoch floor; the caller then sets the system
  /// clock to it, and later NTP answers are checked against it (FW-TIME-006, FW-TIME-014).
  bool begin(int64_t systemUs, uint64_t monoUs, const int64_t* rtcUs);

  /// The clock as it would read now if nothing had changed it since the last accepted time. Once an
  /// answer has been accepted that answer is the reference, whatever the RTC said (FW-TIME-017).
  int64_t expectedUs(uint64_t monoUs) const { return refUs_ + static_cast<int64_t>(monoUs - refMonoUs_); }

  /// Judges an answer SNTP has already written to the clock. On anything but kAccept the caller puts
  /// the clock back to `expectedUs(monoUs)` and does not write the RTC (FW-TIME-015).
  SyncDecision onAnswer(int64_t candidateUs, uint64_t monoUs);

  bool hasSynced() const { return hasSynced_; }
  int64_t lastSyncUs() const { return lastSyncUs_; }
  uint32_t rejectedSyncs() const { return rejected_; }
  bool rtcValid() const { return rtcValid_; }
  /// FW-TIME-006: NTP once an answer was accepted, the RTC before that, a rejected answer changes nothing.
  TimeSource source() const { return hasSynced_ ? TimeSource::kNtp : rtcValid_ ? TimeSource::kRtc : TimeSource::kNone; }
  /// When to ask again after the last answer was rejected; 0 after an accepted one.
  uint32_t retryDelayMs() const;

 private:
  int64_t buildEpochUs_;
  int64_t refUs_ = 0;
  uint64_t refMonoUs_ = 0;
  bool rtcValid_ = false;
  bool hasPending_ = false;
  int64_t pendingOffsetUs_ = 0;  // pending answer minus the monotonic counter, so it ages with time
  bool hasSynced_ = false;
  int64_t lastSyncUs_ = 0;
  uint32_t rejected_ = 0;
  uint32_t rejectsInRow_ = 0;
};

/// Picks which server answered, for the log line, from lwIP's per-server reachability registers
/// (shifted left on each request, bit 0 set on a reply) read before and after the answer. lwIP does
/// not expose its current server; the register that changed and has bit 0 set is the best guess,
/// then the last server that answered. Returns -1 when nothing fits.
int answeringServer(const uint8_t* before, const uint8_t* after, int count, int lastIdx);

}  // namespace clock_core
