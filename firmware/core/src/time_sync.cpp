#include "clock_core/time_sync.h"

namespace clock_core {

int splitNtpServers(const char* list, char (&out)[kMaxNtpServers][kMaxNtpNameLen + 1]) {
  if (!list || !*list) return 0;
  int count = 0;
  size_t len = 0;
  for (const char* p = list;; p++) {
    if (*p == ',' || *p == '\0') {
      if (len == 0 || count >= kMaxNtpServers) return 0;
      out[count++][len] = '\0';
      len = 0;
      if (*p == '\0') return count;
    } else {
      if (len >= kMaxNtpNameLen || count >= kMaxNtpServers) return 0;
      out[count][len++] = *p;
    }
  }
}

const char* syncReasonName(SyncReason reason) {
  switch (reason) {
    case SyncReason::kBeforeBuild: return "before-build";
    case SyncReason::kJump: return "jump";
    case SyncReason::kDisagree: return "disagree";
    case SyncReason::kConfirmed: return "confirmed";
    default: return "ok";
  }
}

const char* timeSourceName(TimeSource source) {
  switch (source) {
    case TimeSource::kRtc: return "rtc";
    case TimeSource::kNtp: return "ntp";
    default: return "none";
  }
}

SyncDecision judgeSync(int64_t candidateUs, int64_t currentUs, int64_t buildEpochUs, bool hasReference,
                       const int64_t* pendingUs) {
  auto distance = [](int64_t a, int64_t b) { return a > b ? a - b : b - a; };
  if (candidateUs < buildEpochUs) return {SyncVerdict::kReject, SyncReason::kBeforeBuild};
  if (!hasReference || distance(candidateUs, currentUs) <= kMaxJumpUs) return {SyncVerdict::kAccept, SyncReason::kNone};
  if (!pendingUs) return {SyncVerdict::kNeedConfirmation, SyncReason::kJump};
  if (distance(candidateUs, *pendingUs) <= kAgreeUs) return {SyncVerdict::kAccept, SyncReason::kConfirmed};
  return {SyncVerdict::kReject, SyncReason::kDisagree};
}

bool TimeSync::begin(int64_t systemUs, uint64_t monoUs, const int64_t* rtcUs) {
  rtcValid_ = rtcUs && *rtcUs >= buildEpochUs_;
  refUs_ = rtcValid_ ? *rtcUs : systemUs;
  refMonoUs_ = monoUs;
  return rtcValid_;
}

SyncDecision TimeSync::onAnswer(int64_t candidateUs, uint64_t monoUs) {
  const int64_t mono = static_cast<int64_t>(monoUs);
  const int64_t pending = pendingOffsetUs_ + mono;
  // FW-TIME-017: a clock that has synced once needs no RTC to spot a resync that moves it by days.
  const SyncDecision d = judgeSync(candidateUs, expectedUs(monoUs), buildEpochUs_, rtcValid_ || hasSynced_,
                                   hasPending_ ? &pending : nullptr);
  if (d.verdict == SyncVerdict::kAccept) {
    refUs_ = candidateUs;
    refMonoUs_ = monoUs;
    hasPending_ = false;
    hasSynced_ = true;
    lastSyncUs_ = candidateUs;
    rejectsInRow_ = 0;
    return d;
  }
  // A big jump becomes (or replaces) the value the next answer has to agree with; an answer from
  // before the build is not a candidate at all.
  if (d.reason == SyncReason::kJump || d.reason == SyncReason::kDisagree) {
    hasPending_ = true;
    pendingOffsetUs_ = candidateUs - mono;
  }
  rejected_++;
  rejectsInRow_++;
  return d;
}

uint32_t TimeSync::retryDelayMs() const {
  if (rejectsInRow_ == 0) return 0;
  uint32_t delay = kRetryMinMs;
  for (uint32_t i = 1; i < rejectsInRow_ && delay < kResyncMs; i++) delay *= 2;
  return delay < kResyncMs ? delay : kResyncMs;
}

int answeringServer(const uint8_t* before, const uint8_t* after, int count, int lastIdx) {
  int replied = -1, changed = -1, replies = 0, changes = 0;
  for (int i = 0; i < count; i++) {
    if (!(after[i] & 1)) continue;
    if (replied < 0) replied = i;
    replies++;
    if (after[i] != before[i]) {
      if (changed < 0) changed = i;
      changes++;
    }
  }
  if (replies == 1) return replied;
  if (changes == 1) return changed;
  if (lastIdx >= 0 && lastIdx < count && (after[lastIdx] & 1)) return lastIdx;
  return replied;
}

}  // namespace clock_core
