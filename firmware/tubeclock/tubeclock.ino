// Replacement firmware for the ClocTeck CT-001 tube clock ("Nixie Clock V4.5.1", ESP8285).
//
// Differences from the stock firmware:
// - Station mode only. No setup hotspot, and nothing ever sends the Wi-Fi password back out.
// - Every /api route needs "Authorization: Bearer <token>"; /health is the only open route.
// - Attention flashes run on the clock itself and fall back to the saved colors on their own.
// - Time zone is a POSIX TZ rule, so daylight saving is handled on the device.
// - Time comes from a short list of trusted NTP servers and every answer is sanity-checked.
// - An MCP server at POST /mcp lets agents flash the clock and read its state (spec/firmware/mcp.md).
// - Colors are data: an effect plus presets, kept on a small LittleFS (spec/firmware/effects.md).
//
// Builds for the device with scripts/build.sh and on Linux with scripts/emulate.sh (the ESP8266
// core's host emulator, where Wi-Fi is the host's network and LED output is logged).
// Functions are defined before use because the host emulator has no prototype generation.

#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <sys/time.h>
#include <time.h>

#ifndef HOST_MOCK
#include <ArduinoOTA.h>
#include <coredecls.h>
#include <lwip/apps/sntp.h>
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing: run scripts/gen-secrets.sh (it reads the repo's .env)"
#endif

// FW-API-031: the version is the build's `git describe`, passed in by the build scripts.
#ifndef TUBECLOCK_VERSION
#error "TUBECLOCK_VERSION is not set: build with scripts/build.sh or scripts/emulate.sh"
#endif
// FW-TIME-013: no time before the build is believed.
#ifndef TUBECLOCK_BUILD_EPOCH
#error "TUBECLOCK_BUILD_EPOCH is not set: build with scripts/build.sh or scripts/emulate.sh"
#endif
// FW-TIME-010: trusted operators only; build.sh checks an override for public pools.
#ifndef TUBECLOCK_NTP_SERVERS
#define TUBECLOCK_NTP_SERVERS "time.cloudflare.com,time.google.com,time.nist.gov"
#endif

#include <memory>
#include <new>

#include "clock_core/attention.h"
#include "clock_core/auth.h"
#include "clock_core/backoff.h"
#include "clock_core/beep.h"
#include "clock_core/button.h"
#include "clock_core/color.h"
#include "clock_core/display.h"
#include "clock_core/effect.h"
#include "clock_core/mcp.h"
#include "clock_core/pins.h"
#include "clock_core/rtc.h"
#include "clock_core/settings.h"
#include "clock_core/time_sync.h"
#include "clock_core/timefmt.h"

static_assert(clock_core::ntpServerCount(TUBECLOCK_NTP_SERVERS) >= 1 &&
                  clock_core::ntpServerCount(TUBECLOCK_NTP_SERVERS) <= clock_core::kMaxNtpServers,
              "TUBECLOCK_NTP_SERVERS needs 1 to 3 comma-separated servers");

// FW-HW-002: every GPIO below is a build flag, never guessed; build.sh supplies the measured values. A
// feature without its flag is compiled out and leaves its pins alone. The emulator has none of this hardware.

// Without an LED pin the build only logs what the tubes would show.
#if defined(TUBECLOCK_LED_PIN) && !defined(HOST_MOCK)
#include <Adafruit_NeoPixel.h>
static_assert(clock_core::isUsableGpio(TUBECLOCK_LED_PIN) && TUBECLOCK_LED_PIN != 16, "TUBECLOCK_LED_PIN: not GPIO 6-8 or 11, and the LED driver cannot reach GPIO 16");
Adafruit_NeoPixel strip(clock_core::kLedCount, TUBECLOCK_LED_PIN, NEO_GRB + NEO_KHZ800);
#define TUBECLOCK_HAS_LEDS 1
#endif

// FW-ATT-010, FW-HW-007: without a pin the `beep` argument is accepted, logged and nothing is driven.
// With one, TUBECLOCK_BUZZER_ON_LEVEL says whether the transistor conducts on 1 or 0.
#if defined(TUBECLOCK_BUZZER_PIN) && !defined(HOST_MOCK)
#ifndef TUBECLOCK_BUZZER_ON_LEVEL
#error "TUBECLOCK_BUZZER_PIN needs TUBECLOCK_BUZZER_ON_LEVEL=0|1: a wrong guess sounds the buzzer for good"
#endif
static_assert(clock_core::isUsableGpio(TUBECLOCK_BUZZER_PIN), "TUBECLOCK_BUZZER_PIN: not GPIO 6-8 or 11, the flash");
static_assert(TUBECLOCK_BUZZER_ON_LEVEL == 0 || TUBECLOCK_BUZZER_ON_LEVEL == 1, "TUBECLOCK_BUZZER_ON_LEVEL is 0 or 1");
#define TUBECLOCK_HAS_BUZZER 1
#endif

// FW-TIME-006: the GC1307 on I2C.
#if (defined(TUBECLOCK_RTC_SDA) || defined(TUBECLOCK_RTC_SCL)) && !defined(HOST_MOCK)
#if !defined(TUBECLOCK_RTC_SDA) || !defined(TUBECLOCK_RTC_SCL)
#error "the RTC needs both TUBECLOCK_RTC_SDA and TUBECLOCK_RTC_SCL"
#endif
#include <Wire.h>
static_assert(clock_core::isUsableGpio(TUBECLOCK_RTC_SDA) && clock_core::isUsableGpio(TUBECLOCK_RTC_SCL), "TUBECLOCK_RTC_SDA/SCL: not GPIO 6-8 or 11, the flash");
#define TUBECLOCK_HAS_RTC 1
#endif

// FW-HW-006: any of the four buttons, as many as are known. TUBECLOCK_BTN_PRESSED_LEVEL says what a
// pressed one reads: 0 turns on the internal pull-up (GPIO 16 has none), 1 relies on the board's.
#if (defined(TUBECLOCK_BTN_1) || defined(TUBECLOCK_BTN_2) || defined(TUBECLOCK_BTN_3) || defined(TUBECLOCK_BTN_4)) && !defined(HOST_MOCK)
#ifndef TUBECLOCK_BTN_PRESSED_LEVEL
#error "TUBECLOCK_BTN_* need TUBECLOCK_BTN_PRESSED_LEVEL=0|1"
#endif
static_assert(TUBECLOCK_BTN_PRESSED_LEVEL == 0 || TUBECLOCK_BTN_PRESSED_LEVEL == 1, "TUBECLOCK_BTN_PRESSED_LEVEL is 0 or 1");
constexpr int kButtonPins[] = {
#ifdef TUBECLOCK_BTN_1
    TUBECLOCK_BTN_1,
#endif
#ifdef TUBECLOCK_BTN_2
    TUBECLOCK_BTN_2,
#endif
#ifdef TUBECLOCK_BTN_3
    TUBECLOCK_BTN_3,
#endif
#ifdef TUBECLOCK_BTN_4
    TUBECLOCK_BTN_4,
#endif
};
static_assert(clock_core::allUsableGpios(kButtonPins), "TUBECLOCK_BTN_*: not GPIO 6-8 or 11, the flash");
#define TUBECLOCK_HAS_BUTTONS 1
#endif

using namespace clock_core;

// FW-MCP-009: the Authorization header has to fit the fixed header buffer to be checked at all.
static_assert(sizeof(kApiToken) + 7 < sizeof(McpHeaders::authorization), "FW_API_TOKEN is too long for /mcp");

static const char kVersion[] = TUBECLOCK_VERSION;
static const int64_t kBuildEpoch = TUBECLOCK_BUILD_EPOCH;
static const char kJson[] = "application/json";

ESP8266WebServer server(80);
Settings settings;
Attention attention;
AuthBackoff authBackoff;
TimeSync timeSync(kBuildEpoch);
const LedLayout layout = LedLayout::ct001();

// Types are declared above the first function because arduino-cli inserts its generated
// prototypes there.

// What the [leds] log last said, so it follows the tubes without a line per frame (FW-FX-075).
struct Shown {
  FrameInfo info;
  uint32_t atMs = 0;
  bool valid = false;
};
Shown lastShown;
uint32_t lastFrameMs = 0;

// The active effect, and what the digits do (FW-FX-050). `activePreset` empty means a custom effect.
EffectEngine fxEngine;
EffectClock fxClock;
DigitFader digitFader;
DisplayOptions displayOptions;
char activePreset[kPresetNameMax + 1] = "";
bool fxDirty = false;  // FW-FX-051: /effect.json is behind the active effect
uint32_t fxDirtyMs = 0;
uint32_t fxFrameMaxUs = 0;  // FW-FX-072
uint32_t fxFrameLogMs = 0;

static const char kEffectFile[] = "/effect.json";
static const char kDisplayFile[] = "/display.json";
// FW-FX-042: one file. LittleFS gives each file over 64 bytes its own 4 KB block, and the 64 KB
// partition has 16 blocks, so a file per preset would run out at about nine.
static const char kPresetsFile[] = "/presets.tsv";
static const char kPresetsTemp[] = "/presets.tmp";
static const char kFileTemp[] = "/file.tmp";

// The routes, for the 405 answer's Allow header (FW-API-020).
struct Route {
  const char* path;
  const char* allow;
};
const Route kRoutes[] = {
    {"/health", "GET"},
    {"/api/state", "GET"},
    {"/api/tube", "POST"},
    {"/api/brightness", "POST"},
    {"/api/hours", "POST"},
    {"/api/tz", "POST"},
    {"/api/attention", "POST, DELETE"},
    {"/api/restart", "POST"},
    {"/api/effect", "GET, PUT"},
    {"/api/presets", "GET"},
    {"/api/preset", "GET, POST, PUT, DELETE"},
    {"/api/display", "GET, POST"},
};

// NTP servers in the order lwIP asks them; `ntpRotation` moves a different one first after a
// rejected answer (FW-TIME-014). lwIP keeps the name pointers, so the names live here for good.
char ntpServers[kMaxNtpServers][kMaxNtpNameLen + 1];
int ntpServerTotal = 0;
int ntpRotation = 0;
uint8_t ntpReach[kMaxNtpServers] = {};
int lastNtpServer = -1;
bool ntpRetryPending = false;
uint32_t ntpRetryFromMs = 0;
uint32_t ntpRetryDelayMs = 0;

bool restartPending = false;
uint32_t restartFromMs = 0;

BeepPattern beeper;
bool buzzerOn = false;
#ifdef TUBECLOCK_HAS_BUTTONS
ButtonDebouncer buttons[sizeof(kButtonPins) / sizeof(kButtonPins[0])];
#endif

// What the MCP tools act on (FW-MCP-030..032); the members are defined further down, after the
// functions they use.
struct McpGlue : McpDevice {
  void startAttention(const AttentionCall& call) override;
  void cancelAttention() override;
  size_t writeState(char* out, size_t cap) override;
  bool hasBuzzer() const override;
  void logToolCall(const char* tool, const char* summary) override;
};
McpGlue mcpGlue;
McpHeaders mcpHeaders;     // static rather than on the 4 KB loop stack
char mcpClientIp[16] = "";  // for the [mcp] log line (FW-MCP-013)
size_t mcpBodyLeft = 0;     // request body bytes not read yet

// ---- Settings storage ----

void loadSettings() {
  uint8_t blob[kSettingsBlobSize];
  EEPROM.begin(kSettingsBlobSize);
  for (size_t i = 0; i < kSettingsBlobSize; i++) blob[i] = EEPROM.read(i);
  if (!decodeSettings(blob, settings)) {
    settings = Settings::defaults();
    Serial.println(F("[settings] none saved, using defaults"));
  }
}

// FW-SET-003, FW-SET-007: commits only when the write changed something, so a client repeating the
// same write costs no flash wear.
void saveSettings(const Settings& before) {
  if (sameSettings(before, settings)) return;
  uint8_t blob[kSettingsBlobSize];
  encodeSettings(settings, blob);
  for (size_t i = 0; i < kSettingsBlobSize; i++) EEPROM.write(i, blob[i]);
  EEPROM.commit();
  Serial.println(F("[settings] saved"));
}

// ---- Time ----

uint64_t monoUs() {
#ifdef HOST_MOCK
  return static_cast<uint64_t>(millis()) * 1000;  // never used for a sync: the emulator has no SNTP
#else
  return micros64();
#endif
}

int64_t clockUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * kUsPerSec + tv.tv_usec;
}

void setClockUs(int64_t us) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(us / kUsPerSec);
  tv.tv_usec = static_cast<suseconds_t>(us % kUsPerSec);
  settimeofday(&tv, nullptr);
}

void formatIsoUtc(int64_t us, char (&out)[21]) {
  const time_t s = static_cast<time_t>(us / kUsPerSec);
  struct tm t;
  gmtime_r(&s, &t);
  strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &t);
}

#ifdef TUBECLOCK_HAS_RTC
// FW-TIME-006: the time on the chip, or false when it could not be read or has no time to give.
bool readRtc(int64_t& us) {
  Wire.beginTransmission(kRtcI2cAddress);
  Wire.write(kRtcFirstRegister);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(kRtcI2cAddress, static_cast<uint8_t>(kRtcRegisterCount)) != kRtcRegisterCount) {
    Serial.println(F("[rtc] no answer"));
    return false;
  }
  uint8_t regs[kRtcRegisterCount];
  for (uint8_t& reg : regs) reg = Wire.read();
  const RtcStatus status = decodeRtc(regs, us);
  if (status == RtcStatus::kOk) return true;
  Serial.println(status == RtcStatus::kHalted ? F("[rtc] clock halted, no time") : F("[rtc] holds no valid time"));
  return false;
}

// FW-TIME-006, FW-TIME-015: after an accepted answer. Also starts a halted chip.
void writeRtc(int64_t us) {
  uint8_t regs[kRtcRegisterCount];
  if (!encodeRtc(us, regs)) return;
  Wire.beginTransmission(kRtcI2cAddress);
  Wire.write(kRtcFirstRegister);
  Wire.write(regs, kRtcRegisterCount);
  if (Wire.endTransmission() != 0) Serial.println(F("[rtc] write failed"));
}
#endif

const char* ntpServer(int slot) {
  return slot < ntpServerTotal ? ntpServers[(slot + ntpRotation) % ntpServerTotal] : nullptr;
}

// FW-TIME-001, FW-TIME-010: restarting SNTP also sends a request at once, so this doubles as
// "sync now" after a Wi-Fi reconnect (FW-TIME-011).
void applyTimeZone() { configTime(settings.tz, ntpServer(0), ntpServer(1), ntpServer(2)); }

#ifndef HOST_MOCK
// FW-TIME-011: resync every hour, stated here rather than left to the core's default.
extern "C" uint32_t sntp_update_delay_MS_rfc_not_less_than_15000() { return TimeSync::kResyncMs; }

// FW-TIME-015: runs (from the loop, via the core's scheduler) after SNTP has set the clock. A
// rejected answer is undone and the RTC is left alone.
void onTimeSet(bool fromSntp) {
  if (!fromSntp) return;  // our own settimeofday() undoing an answer
  const uint64_t mono = monoUs();
  const int64_t answer = clockUs();
  const int64_t have = timeSync.expectedUs(mono);

  uint8_t reach[kMaxNtpServers] = {};
#if SNTP_MONITOR_SERVER_REACHABILITY
  for (int i = 0; i < ntpServerTotal; i++) reach[i] = sntp_getreachability(i);
#endif
  const int idx = answeringServer(ntpReach, reach, ntpServerTotal, lastNtpServer);
  memcpy(ntpReach, reach, sizeof(ntpReach));
  const char* server = idx >= 0 && sntp_getservername(idx) ? sntp_getservername(idx) : "?";

  const SyncDecision d = timeSync.onAnswer(answer, mono);
  if (d.verdict == SyncVerdict::kAccept) {
    lastNtpServer = idx;
    ntpRetryPending = false;
    Serial.printf("[time] synced from %s\n", server);
#ifdef TUBECLOCK_HAS_RTC
    writeRtc(answer);  // FW-TIME-006
#endif
    return;
  }
  setClockUs(have);
  char ntpIso[21], haveIso[21];
  formatIsoUtc(answer, ntpIso);
  formatIsoUtc(have, haveIso);
  Serial.printf("[time] rejected %s: ntp=%s have=%s server=%s\n", syncReasonName(d.reason), ntpIso, haveIso, server);
  // FW-TIME-014: ask again, starting with a different server, so a confirmation comes from elsewhere.
  if (ntpServerTotal > 1) ntpRotation = (ntpRotation + 1) % ntpServerTotal;
  ntpRetryPending = true;
  ntpRetryFromMs = millis();
  ntpRetryDelayMs = timeSync.retryDelayMs();
}
#endif

void setupTime() {
  ntpServerTotal = splitNtpServers(TUBECLOCK_NTP_SERVERS, ntpServers);
  Serial.printf("[time] ntp servers %s\n", TUBECLOCK_NTP_SERVERS);
  // FW-TIME-006: nullptr = no valid RTC time. TimeSync decides whether the time is new enough
  // (FW-TIME-013), and later answers are checked against it (FW-TIME-014).
  const int64_t* rtcUs = nullptr;
#ifdef TUBECLOCK_HAS_RTC
  int64_t rtcTime = 0;
  Wire.begin(TUBECLOCK_RTC_SDA, TUBECLOCK_RTC_SCL);
  if (readRtc(rtcTime)) rtcUs = &rtcTime;
#endif
  if (timeSync.begin(clockUs(), monoUs(), rtcUs)) {
    setClockUs(*rtcUs);
    char iso[21];
    formatIsoUtc(*rtcUs, iso);
    Serial.printf("[rtc] time %s\n", iso);
  } else if (rtcUs) {
    Serial.println(F("[rtc] time is before this build, not used"));
  }
#ifndef HOST_MOCK
  settimeofday_cb(onTimeSet);
#endif
  applyTimeZone();
}

// ---- Effects storage (FW-FX-050..053) ----

// Writes through a temporary file: a power cut mid-write leaves the old file, not half a new one.
bool writeFileAtomic(const char* path, const char* data, size_t len) {
  File f = LittleFS.open(kFileTemp, "w");
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  f.close();
  if (ok && LittleFS.rename(kFileTemp, path)) return true;
  LittleFS.remove(kFileTemp);
  return false;
}

// The file as text, or 0 when it is missing or does not fit `cap`.
size_t readSmallFile(const char* path, char* out, size_t cap) {
  File f = LittleFS.open(path, "r");
  if (!f || f.size() >= cap) return 0;
  const int got = f.read(reinterpret_cast<uint8_t*>(out), cap - 1);
  if (got <= 0) return 0;
  out[got] = '\0';
  return static_cast<size_t>(got);
}

void saveEffect() {
  fxDirty = false;
  char json[kFxJsonBytes];
  const size_t n = writeEffectJson(fxEngine.effect(), json, sizeof(json), true, activePreset);
  const bool saved = n && writeFileAtomic(kEffectFile, json, n);
  Serial.println(saved ? F("[fx] /effect.json saved") : F("[fx] /effect.json not saved"));
}

void saveDisplayOptions() {
  char json[kDisplayJsonBytes];
  const size_t n = writeDisplayJson(displayOptions, 0, json, sizeof(json));
  if (!n || !writeFileAtomic(kDisplayFile, json, n)) Serial.println(F("[fx] /display.json not saved"));
}

// FW-FX-072: the slowest frame since the last line, for the pattern it was measured on.
void logFrameMax() {
  Serial.printf("[fx] frame max=%lu pattern=%s\n", static_cast<unsigned long>(fxFrameMaxUs), fxPatternName(fxEngine.effect().pattern));
  fxFrameMaxUs = 0;
  fxFrameLogMs = millis();
}

// FW-FX-043: every change to the active effect comes here; `preset` is nullptr unless a preset was
// applied. FW-FX-051: the file follows 2 s after the last change.
void setActiveEffect(const Effect& effect, const char* preset) {
  const char* name = preset ? preset : "";
  if (effect == fxEngine.effect() && strcmp(name, activePreset) == 0) return;
  logFrameMax();
  fxEngine.apply(effect);
  strncpy(activePreset, name, kPresetNameMax);
  activePreset[kPresetNameMax] = '\0';
  fxDirty = true;
  fxDirtyMs = millis();
}

// FW-FX-052, FW-FX-053: what is on the file system, or the defaults. Runs before the first frame.
void setupEffects() {
  LittleFSConfig config;
  config.setAutoFormat(false);
  LittleFS.setConfig(config);
  if (!LittleFS.begin()) {
    Serial.println(F("[fx] LittleFS mount failed, formatting"));
    if (!LittleFS.format() || !LittleFS.begin()) Serial.println(F("[fx] LittleFS format failed, nothing will be saved"));
  }

  Effect effect;
  char json[kFxJsonBytes];
  size_t n = readSmallFile(kEffectFile, json, sizeof(json));
  if (!n || parseActiveEffect(json, n, effect, activePreset) != FxError::kNone) {
    Serial.printf("[fx] %s invalid, using defaults\n", kEffectFile);
    loadBuiltinPreset(0, effect);  // OQ-22: rainbow-wave
    builtinPresetName(0, activePreset);
  }
  fxEngine.apply(effect);

  n = readSmallFile(kDisplayFile, json, sizeof(json));
  if (!n || !parseDisplayJson(json, n, displayOptions)) {
    Serial.printf("[fx] %s invalid, using defaults\n", kDisplayFile);
    displayOptions = DisplayOptions{};
  }
  Serial.printf("[fx] effect %s, preset %s\n", fxPatternName(effect.pattern), activePreset[0] ? activePreset : "none");
}

// One stored preset line into `line` (kPresetLineBytes), without its newline; false at the end.
bool readPresetLine(File& f, char* line) {
  const size_t start = f.position();
  const int got = f.read(reinterpret_cast<uint8_t*>(line), kPresetLineBytes - 1);
  if (got <= 0) return false;
  char* newline = static_cast<char*>(memchr(line, '\n', got));
  if (!newline) return false;
  *newline = '\0';
  f.seek(start + (newline - line) + 1);
  return true;
}

// FW-FX-040: finds a user preset; `name` and `json` then point into `line`.
bool findUserPreset(const char* wanted, char* line, const char*& name, const char*& json) {
  File f = LittleFS.open(kPresetsFile, "r");
  if (!f) return false;
  while (readPresetLine(f, line)) {
    if (splitPresetLine(line, name, json) && presetNamesEqual(name, wanted)) return true;
  }
  return false;
}

int userPresetCount(char* line) {
  File f = LittleFS.open(kPresetsFile, "r");
  int count = 0;
  while (f && readPresetLine(f, line)) count++;
  return count;
}

// FW-FX-042: the store with `name` left out and, when `json` is given, written anew. False if it does
// not fit, with the old store still in place.
bool rewritePresets(const char* name, const char* json, size_t jsonLen, char* line) {
  File out = LittleFS.open(kPresetsTemp, "w");
  if (!out) return false;
  auto put = [&out](const char* text, size_t n) { return out.write(reinterpret_cast<const uint8_t*>(text), n) == n; };
  bool ok = true;
  File in = LittleFS.open(kPresetsFile, "r");
  while (ok && in && readPresetLine(in, line)) {
    char* tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = '\0';
    const bool same = presetNamesEqual(line, name);
    *tab = '\t';
    if (!same) ok = put(line, strlen(line)) && put("\n", 1);
  }
  if (in) in.close();
  if (ok && json) ok = put(name, strlen(name)) && put("\t", 1) && put(json, jsonLen) && put("\n", 1);
  out.close();
  if (ok && LittleFS.rename(kPresetsTemp, kPresetsFile)) return true;
  LittleFS.remove(kPresetsTemp);
  return false;
}

// ---- Display ----

// FW-DSP-006, FW-FX-075: digits are - when blank, colors are what the tubes get before brightness.
void logShown(const FrameInfo& s) {
  char digits[kTubes + 1];
  for (int t = 0; t < kTubes; t++) digits[t] = s.shown[t] == kBlank ? '-' : static_cast<char>('0' + s.shown[t]);
  digits[kTubes] = '\0';
  Serial.printf("[leds] lit=%d brightness=%u digits=%s tubes=", s.lit ? 1 : 0, s.brightness, digits);
  for (int t = 0; t < kTubes; t++) Serial.printf("%s#%02x%02x%02x", t ? "," : "", s.colors[t].r, s.colors[t].g, s.colors[t].b);
  Serial.println();
}

void updateDisplay() {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  const uint32_t nowMs = millis();
  const uint32_t startUs = micros();

  // FW-TIME-004, FW-DSP-007: blank until the time is known; a flash then lights digit 0.
  const auto digits = tubeDigits(isTimeKnown(now, kBuildEpoch), attention.isActive(nowMs), local.tm_hour,
                                 local.tm_min, local.tm_sec, settings.is24Hour);
  FrameInfo s;
  const Frame frame = composeFrame(layout, fxEngine, attention, digitFader, displayOptions, digits, fxClock.advance(nowMs),
                                   nowMs, settings.brightness, s);

  // FW-FX-072: the engine's own time, without show() or the log.
  const uint32_t usedUs = micros() - startUs;
  if (usedUs > fxFrameMaxUs) fxFrameMaxUs = usedUs;
  if (nowMs - fxFrameLogMs >= 60000) logFrameMax();

#ifdef TUBECLOCK_HAS_LEDS
  for (int i = 0; i < kLedCount; i++) strip.setPixelColor(i, frame[i].r, frame[i].g, frame[i].b);
  strip.show();
#else
  (void)frame;
#endif

  // FW-FX-075: at once when the digits or a flash change; moving colors, and the digits a scramble,
  // carry or glyph shows on the way, at most once a second.
  const FrameInfo& was = lastShown.info;
  const bool atOnce = !lastShown.valid || s.digits != was.digits || s.lit != was.lit || s.attention != was.attention;
  const bool moved = s.brightness != was.brightness || s.colors != was.colors || s.shown != was.shown;
  if (atOnce || (moved && nowMs - lastShown.atMs >= 1000)) {
    logShown(s);
    lastShown.info = s;
    lastShown.atMs = nowMs;
    lastShown.valid = true;
  }
}

// FW-ATT-010: from every wait as well as the loop, so the 80 ms pulses keep their timing and a pulse
// can never be stretched by a slow request (FW-HW-007).
void driveBuzzer() {
  const bool on = beeper.isOn(millis());
  if (on == buzzerOn) return;
  buzzerOn = on;
#ifdef TUBECLOCK_HAS_BUZZER
  digitalWrite(TUBECLOCK_BUZZER_PIN, on ? TUBECLOCK_BUZZER_ON_LEVEL : !TUBECLOCK_BUZZER_ON_LEVEL);
#endif
}

// FW-ATT-008: /api/attention and the MCP tool both come here; the last caller wins (FW-ATT-006).
void startFlash(const Hsv& color, uint32_t ms, bool blink, bool beep) {
  attention.start({color.h, color.s, 100}, millis(), ms, blink);
  if (!beep) return;
#ifdef TUBECLOCK_HAS_BUZZER
  beeper.start(millis());
#else
  Serial.println(F("[beep] requested; no buzzer pin (TUBECLOCK_BUZZER_PIN unset), nothing driven"));
#endif
}

void cancelFlash() {
  attention.cancel();
  beeper.cancel();
}

// FW-HW-006, FW-ATT-011: any button ends a running flash, and does nothing else.
void pollButtons() {
#ifdef TUBECLOCK_HAS_BUTTONS
  const uint32_t nowMs = millis();
  size_t i = 0;
  for (ButtonDebouncer& button : buttons) {
    const bool down = digitalRead(kButtonPins[i++]) == TUBECLOCK_BTN_PRESSED_LEVEL;
    if (button.update(down, nowMs) && attention.isActive(nowMs)) {
      cancelFlash();
      Serial.println(F("[button] flash cancelled"));
    }
  }
#endif
}

void renderIfDue() {
  driveBuzzer();
  pollButtons();
  if (millis() - lastFrameMs >= 50) {
    lastFrameMs = millis();
    updateDisplay();
  }
}

// Waits without freezing the tubes (used by the auth backoff).
void waitRendering(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    renderIfDue();
    delay(5);
  }
}

// ---- HTTP ----

void sendError(int code, const char* message) {
  String body = F("{\"error\":\"");
  body += message;
  body += F("\"}");
  server.send(code, kJson, body);
}

bool authorized() {
  // Pass the length: a cleared header's c_str() still holds the previous request's token.
  const String& header = server.header("Authorization");
  const uint32_t ip = server.client().remoteIP();
  if (bearerMatches(header.c_str(), header.length(), kApiToken)) {
    authBackoff.onSuccess(ip);
    return true;
  }
  // FW-SEC-014: from the 6th failure in a row an IP waits 1 s for each 401.
  waitRendering(authBackoff.onFailure(ip, millis()));
  server.sendHeader(F("WWW-Authenticate"), F("Bearer"));
  sendError(401, "unauthorized");
  return false;
}

bool argInt(const char* name, long min, long max, long& out) {
  if (!server.hasArg(name)) return false;
  return parseInt(server.arg(name).c_str(), min, max, out);
}

// FW-API-030: the state object, shared by GET /api/state, every write and the clock_status tool.
String stateJson() {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char clock[9] = "unset";
  if (isTimeKnown(now, kBuildEpoch)) {
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
  }

  String body = F("{\"version\":\"");
  body += kVersion;
  body += F("\",\"hostname\":\"");  // FW-API-039
  body += kHostname;
  body += F("\",\"time\":\"");
  body += clock;
  body += F("\",\"tz\":\"");
  body += settings.tz;
  body += F("\",\"is24h\":");
  body += settings.is24Hour ? F("true") : F("false");
  body += F(",\"brightness\":");
  body += settings.brightness;
  // FW-API-041, FW-FX-069: the full effect is on /api/effect, to keep this small.
  body += F(",\"preset\":");
  if (activePreset[0]) {
    body += '"';
    body += activePreset;
    body += '"';
  } else {
    body += F("null");
  }
  body += F(",\"effect\":{\"pattern\":\"");
  body += fxPatternName(fxEngine.effect().pattern);
  body += F("\"},\"display\":{\"hours\":");
  body += settings.is24Hour ? 24 : 12;
  body += F(",\"leadingZero\":");
  body += displayOptions.leadingZero ? F("true") : F("false");
  body += F(",\"seconds\":");
  body += displayOptions.seconds ? F("true") : F("false");
  body += F(",\"transition\":\"");
  body += fxTransitionName(displayOptions.transition);
  body += F("\"},\"attention\":");
  body += attention.isActive(millis()) ? F("true") : F("false");
  body += F(",\"tubes\":[");
  for (int t = 0; t < kTubes; t++) {
    if (t) body += ',';
    body += F("{\"h\":");
    body += settings.tubes[t].h;
    body += F(",\"s\":");
    body += settings.tubes[t].s;
    body += '}';
  }
  // FW-API-037, FW-API-040, FW-TIME-016
  body += F("],\"timeSource\":\"");
  body += timeSourceName(timeSync.source());
  body += F("\",\"lastSync\":");
  if (timeSync.hasSynced()) {
    char iso[21];
    formatIsoUtc(timeSync.lastSyncUs(), iso);
    body += '"';
    body += iso;
    body += '"';
  } else {
    body += F("null");
  }
  body += F(",\"rejectedSyncs\":");
  body += timeSync.rejectedSyncs();
  body += '}';
  return body;
}

void sendState() { server.send(200, kJson, stateJson()); }

void handleState() {
  if (authorized()) sendState();
}

// POST /api/tube?t=1-6|all&h=0-360&s=0-100
void handleTube() {
  if (!authorized()) return;
  long h, s, t = 0;
  const bool all = server.arg("t") == "all";
  if ((!all && !argInt("t", 1, kTubes, t)) || !argInt("h", 0, 360, h) || !argInt("s", 0, 100, s)) {
    return sendError(400, "need t=1-6|all, h=0-360, s=0-100");
  }
  const Settings before = settings;
  for (int i = 0; i < kTubes; i++) {
    if (all || i == t - 1) settings.tubes[i] = {static_cast<uint16_t>(h), static_cast<uint8_t>(s), 100};
  }
  saveSettings(before);
  // FW-FX-062, FW-DSP-011: the saved colors are the `tubes` pattern's palette.
  Effect effect = Effect::defaults(FxPattern::kTubes);
  effect.stopCount = kTubes;
  for (int i = 0; i < kTubes; i++) effect.stops[i] = {hsvToRgb({settings.tubes[i].h, settings.tubes[i].s, 100}), kFxNoAt};
  setActiveEffect(effect, nullptr);
  sendState();
}

// POST /api/brightness?v=0-255
void handleBrightness() {
  if (!authorized()) return;
  long v;
  if (!argInt("v", 0, 255, v)) return sendError(400, "need v=0-255");
  const Settings before = settings;
  settings.brightness = static_cast<uint8_t>(v);
  saveSettings(before);
  sendState();
}

// POST /api/hours?format=12|24
void handleHours() {
  if (!authorized()) return;
  long f;
  if (!argInt("format", 12, 24, f) || (f != 12 && f != 24)) return sendError(400, "need format=12|24");
  const Settings before = settings;
  settings.is24Hour = f == 24;
  saveSettings(before);
  sendState();
}

// POST /api/tz?tz=<POSIX TZ rule>
void handleTz() {
  if (!authorized()) return;
  const String tz = server.arg("tz");
  if (!isValidTz(tz.c_str())) return sendError(400, "need tz=<POSIX TZ rule>, e.g. EST5EDT,M3.2.0,M11.1.0");
  const Settings before = settings;
  strncpy(settings.tz, tz.c_str(), kTzMaxLen);
  settings.tz[kTzMaxLen] = '\0';
  saveSettings(before);
  // Reconfiguring restarts SNTP, so only do it when the rule actually changed.
  if (strcmp(before.tz, settings.tz) != 0) applyTimeZone();
  sendState();
}

// POST /api/attention?h=0-360&s=0-100&ms=1-30000&blink=0|1&beep=0|1   DELETE /api/attention cancels.
void handleAttention() {
  if (!authorized()) return;
  if (server.method() == HTTP_DELETE) {
    cancelFlash();
    return sendState();
  }
  long h, s, ms = 5000, blink = 0, beep = 0;
  if (!argInt("h", 0, 360, h) || !argInt("s", 0, 100, s)) return sendError(400, "need h=0-360, s=0-100");
  if (server.hasArg("ms") && !argInt("ms", 1, Attention::kMaxDurationMs, ms)) return sendError(400, "ms is 1-30000");
  if (server.hasArg("blink") && !argInt("blink", 0, 1, blink)) return sendError(400, "blink is 0 or 1");
  if (server.hasArg("beep") && !argInt("beep", 0, 1, beep)) return sendError(400, "beep is 0 or 1");  // FW-ATT-010
  startFlash({static_cast<uint16_t>(h), static_cast<uint8_t>(s), 100}, ms, blink == 1, beep == 1);
  sendState();
}

// POST /api/restart: answers, then reboots from the loop ~100 ms later (FW-API-018).
void handleRestart() {
  if (!authorized()) return;
  server.send(200, kJson, "{\"ok\":true}");
  server.client().flush();
  restartPending = true;
  restartFromMs = millis();
}

// ---- Effects, presets, display options (spec/firmware/effects.md section 6) ----

void sendFxError(FxError error) {
  char text[80];
  fxErrorText(error, text, sizeof(text));
  sendError(error == FxError::kTooLarge ? 413 : 400, text);
}

void sendEffectJson(const Effect& effect, bool withPreset) {
  char json[kFxJsonBytes];
  writeEffectJson(effect, json, sizeof(json), withPreset, activePreset);
  server.send(200, kJson, json);
}

// The request's JSON body, empty when it has none; false after answering 415 (OQ-26). The size was
// checked by jsonBodyFits() before the server read it. A body needs a JSON Content-Type; the header's
// length is checked first, since a cleared String still holds the previous request's bytes.
bool jsonBody(const char*& body, size_t& len) {
  const String& type = server.header("Content-Type");
  const bool hasBody = server.hasArg("plain");
  if ((hasBody || type.length()) && !isJsonContentType(type.c_str(), type.length())) {
    sendError(415, "need Content-Type: application/json");
    return false;
  }
  const String& plain = server.arg("plain");
  body = hasBody ? plain.c_str() : "";
  len = hasBody ? plain.length() : 0;
  return true;
}

// GET /api/effect (FW-FX-060), PUT /api/effect (FW-FX-061)
void handleEffect() {
  if (!authorized()) return;
  if (server.method() == HTTP_PUT) {
    const char* body;
    size_t len;
    if (!jsonBody(body, len)) return;
    Effect next;
    const FxError error = parseEffect(body, len, &fxEngine.effect(), next);
    if (error != FxError::kNone) return sendFxError(error);
    setActiveEffect(next, nullptr);
  }
  sendEffectJson(fxEngine.effect(), true);
}

void appendPresetEntry(String& body, const char* name, bool builtin, const char* pattern) {
  if (body.length() > 1) body += ',';
  body += F("{\"name\":\"");
  body += name;
  body += F("\",\"builtin\":");
  body += builtin ? F("true") : F("false");
  body += F(",\"pattern\":\"");
  body += pattern;
  body += '"';
  if (activePreset[0] && presetNamesEqual(name, activePreset)) body += F(",\"active\":true");
  body += '}';
}

// GET /api/presets (FW-FX-044, FW-FX-063)
void handlePresets() {
  if (!authorized()) return;
  std::unique_ptr<char[]> line(new (std::nothrow) char[kPresetLineBytes]);
  if (!line) return sendError(503, "out of memory");
  String body = F("[");
  body.reserve(2200);
  char name[kPresetNameMax + 1];
  char pattern[12];
  for (int i = 0; i < kBuiltinPresetCount; i++) {
    builtinPresetName(i, name);
    builtinPresetJson(i, line.get(), kPresetLineBytes);
    fxPatternOfJson(line.get(), pattern);
    appendPresetEntry(body, name, true, pattern);
  }
  File f = LittleFS.open(kPresetsFile, "r");
  while (f && readPresetLine(f, line.get())) {
    const char* userName;
    const char* json;
    if (!splitPresetLine(line.get(), userName, json)) continue;
    fxPatternOfJson(json, pattern);
    appendPresetEntry(body, userName, false, pattern);
  }
  body += ']';
  server.send(200, kJson, body);
}

// A preset by name, built-in or stored. `canonical` is the name as it was saved.
bool loadPreset(const char* wanted, char* line, Effect& effect, char (&canonical)[kPresetNameMax + 1]) {
  const int builtin = findBuiltinPreset(wanted);
  if (builtin >= 0) {
    builtinPresetName(builtin, canonical);
    return loadBuiltinPreset(builtin, effect);
  }
  const char* name;
  const char* json;
  if (!findUserPreset(wanted, line, name, json)) return false;
  strcpy(canonical, name);
  if (parseEffect(json, strlen(json), nullptr, effect) == FxError::kNone) return true;
  Serial.printf("[fx] preset %s invalid\n", canonical);  // FW-FX-052
  return false;
}

// PUT /api/preset?name= (FW-FX-066)
void savePreset(const char* name, char* line) {
  if (!isValidPresetName(name)) return sendError(400, "name: need 1-24 characters of A-Z a-z 0-9 - _ and space");
  if (findBuiltinPreset(name) >= 0) return sendError(403, "built-in presets are read-only");
  const char* body;
  size_t len;
  if (!jsonBody(body, len)) return;
  Effect effect = fxEngine.effect();  // FW-FX-066: an empty body saves the active effect
  if (len) {
    const FxError error = parseEffect(body, len, nullptr, effect);
    if (error != FxError::kNone) return sendFxError(error);
  }
  const char* found;
  const char* foundJson;
  if (!findUserPreset(name, line, found, foundJson) && userPresetCount(line) >= kMaxUserPresets) {
    return sendError(507, "preset storage full (16 presets)");
  }
  char json[kFxJsonBytes];
  const size_t n = writeEffectJson(effect, json, sizeof(json), false, nullptr);
  if (!n || !rewritePresets(name, json, n, line)) return sendError(507, "preset storage full");
  // FW-FX-043: what was applied under this name is no longer what the name holds.
  if (len && presetNamesEqual(name, activePreset)) setActiveEffect(fxEngine.effect(), nullptr);
  server.send(200, kJson, json);
}

// GET (FW-FX-064), POST (FW-FX-065), PUT (FW-FX-066), DELETE (FW-FX-067) /api/preset?name=
void handlePreset() {
  if (!authorized()) return;
  std::unique_ptr<char[]> line(new (std::nothrow) char[kPresetLineBytes]);
  if (!line) return sendError(503, "out of memory");
  const String& wanted = server.arg("name");
  const char* name = wanted.length() ? wanted.c_str() : "";
  if (server.method() == HTTP_PUT) return savePreset(name, line.get());

  if (!isValidPresetName(name)) return sendError(404, "no such preset");
  if (server.method() == HTTP_DELETE) {
    if (findBuiltinPreset(name) >= 0) return sendError(403, "built-in presets are read-only");
    const char* found;
    const char* json;
    if (!findUserPreset(name, line.get(), found, json)) return sendError(404, "no such preset");
    if (!rewritePresets(name, nullptr, 0, line.get())) return sendError(507, "preset storage full");
    if (presetNamesEqual(name, activePreset)) setActiveEffect(fxEngine.effect(), nullptr);  // the name is gone
    server.send(200, kJson, "{\"ok\":true}");
    return;
  }
  Effect effect;
  char canonical[kPresetNameMax + 1];
  if (!loadPreset(name, line.get(), effect, canonical)) return sendError(404, "no such preset");
  if (server.method() == HTTP_POST) {
    setActiveEffect(effect, canonical);  // FW-FX-043
    return sendState();
  }
  sendEffectJson(effect, false);
}

// GET and POST /api/display (FW-FX-068). The hour format has its own route (FW-API-014).
void handleDisplay() {
  if (!authorized()) return;
  if (server.method() == HTTP_POST) {
    // Named by the spec for v1.1: refused rather than silently ignored under FW-API-007.
    if (server.hasArg("night")) return sendError(400, "night is not in this firmware yet");
    DisplayOptions next = displayOptions;
    if (server.hasArg("tubeBrightness") && !parseTubeBrightness(server.arg("tubeBrightness").c_str(), next.tubeBrightness)) {
      return sendError(400, "need tubeBrightness=a,b,c,d,e,f with each 0-100");  // FW-FX-034
    }
    long v;
    if (server.hasArg("leadingZero")) {
      if (!argInt("leadingZero", 0, 1, v)) return sendError(400, "need leadingZero=0|1");
      next.leadingZero = v == 1;
    }
    if (server.hasArg("seconds")) {
      if (!argInt("seconds", 0, 1, v)) return sendError(400, "need seconds=0|1");
      next.seconds = v == 1;
    }
    if (server.hasArg("transition") && !parseFxTransition(server.arg("transition").c_str(), next.transition)) {
      return sendError(400, "need transition=cut|crossfade|scramble|carry");
    }
    if (server.hasArg("fadeMs")) {
      if (!argInt("fadeMs", kFadeMinMs, kFadeMaxMs, v)) return sendError(400, "need fadeMs=50-1000");
      next.fadeMs = static_cast<uint16_t>(v);
    }
    // FW-FX-051: display options save at once; FW-SET-007's rule keeps repeats off the flash.
    if (!(next == displayOptions)) {
      displayOptions = next;
      saveDisplayOptions();
    }
  }
  char json[kDisplayJsonBytes];
  writeDisplayJson(displayOptions, settings.is24Hour ? 24 : 12, json, sizeof(json));
  server.send(200, kJson, json);
}

// FW-API-019, FW-API-020: a known path with the wrong method is 405 with Allow; anything else 404.
// /api paths still check the token first, so nothing about them is answered without it.
void handleNotFound() {
  for (const Route& r : kRoutes) {
    if (server.uri() != r.path) continue;
    if (server.uri().startsWith("/api/") && !authorized()) return;
    server.sendHeader(F("Allow"), r.allow);
    return sendError(405, "method not allowed");
  }
  sendError(404, "not found");
}

// ---- MCP (spec/firmware/mcp.md) ----
//
// /mcp is taken off the web server before it reads the request, through its hook: the server would
// buffer any body size it is sent, while this path checks Host, Origin, the token and the length
// first (FW-SEC-012, FW-SEC-013) and only then reads at most kMcpMaxBodyBytes. The JSON-RPC work is
// the core's handleMcpMessage().

void McpGlue::startAttention(const AttentionCall& call) { startFlash(call.color, call.durationMs, call.blink, call.beep); }

void McpGlue::cancelAttention() { cancelFlash(); }

size_t McpGlue::writeState(char* out, size_t cap) {
  const String state = stateJson();
  if (state.length() >= cap) return 0;
  memcpy(out, state.c_str(), state.length() + 1);
  return state.length();
}

bool McpGlue::hasBuzzer() const {
#ifdef TUBECLOCK_HAS_BUZZER
  return true;
#else
  return false;
#endif
}

// FW-MCP-013, FW-SEC-003: never the token; the message was made one printable line by the core.
void McpGlue::logToolCall(const char* tool, const char* summary) {
  Serial.printf("[mcp] %s %s%s%s\n", mcpClientIp, tool, *summary ? " " : "", summary);
}

const char* httpReason(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    default: return "Service Unavailable";
  }
}

// One response, then the connection closes (the hook tells the server to stop the client).
void mcpRespond(WiFiClient& client, int status, const char* body, size_t len, const char* extraHeaders) {
  char head[192];
  const int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n%sContent-Length: %u\r\nConnection: close\r\n%s\r\n",
                         status, httpReason(status), len ? "Content-Type: application/json\r\n" : "",
                         static_cast<unsigned>(len), extraHeaders);
  client.write(reinterpret_cast<const uint8_t*>(head), n);
  if (len) client.write(reinterpret_cast<const uint8_t*>(body), len);
  client.flush();

  // Closing with unread data sends a TCP reset, which can cost the client this very answer. So a
  // body refused unread (401, 403, 413...) is swallowed first, within limits: FW-BUD-006.
  const uint32_t drainFromMs = millis();
  size_t left = mcpBodyLeft < 8192 ? mcpBodyLeft : 8192;
  uint8_t scrap[64];
  while (left && client.connected() && millis() - drainFromMs < 150) {
    const int got = client.available() ? client.read(scrap, left < sizeof(scrap) ? left : sizeof(scrap)) : 0;
    if (got > 0) {
      left -= got;
    } else {
      renderIfDue();
      delay(1);
    }
  }
  mcpBodyLeft = 0;
}

// A JSON-RPC error with id null, for the answers given before the body is read.
void mcpRespondError(WiFiClient& client, int status, int code, const char* message) {
  char body[192];
  mcpRespond(client, status, body, writeMcpError(code, message, body, sizeof(body)), "");
}

// Waits for a byte without freezing the tubes; false once `startMs` is 2 s ago or the peer is gone.
bool mcpWaitForData(WiFiClient& client, uint32_t startMs) {
  while (!client.available()) {
    if (!client.connected() || millis() - startMs >= 2000) return false;
    renderIfDue();
    delay(1);
  }
  return true;
}

// One header line without CR LF: its length, -1 on timeout, -2 when it is longer than the buffer.
int mcpReadLine(WiFiClient& client, char* buf, size_t cap, uint32_t startMs) {
  size_t len = 0;
  bool tooLong = false;
  while (mcpWaitForData(client, startMs)) {
    const int c = client.read();
    if (c < 0) continue;
    if (c == '\n') {
      if (len && buf[len - 1] == '\r') len--;
      buf[len] = '\0';
      return tooLong ? -2 : static_cast<int>(len);
    }
    if (len + 1 < cap) {
      buf[len++] = static_cast<char>(c);
    } else {
      tooLong = true;
    }
  }
  return -1;
}

bool mcpReadBody(WiFiClient& client, char* buf, size_t len, uint32_t startMs) {
  size_t got = 0;
  while (got < len && mcpWaitForData(client, startMs)) {
    const int n = client.read(reinterpret_cast<uint8_t*>(buf) + got, len - got);
    if (n > 0) got += n;
  }
  return got == len;
}

void handleMcp(const String& method, WiFiClient& client) {
  const uint32_t startMs = millis();
  mcpBodyLeft = 0;
  strncpy(mcpClientIp, client.remoteIP().toString().c_str(), sizeof(mcpClientIp) - 1);

  // The request line is already read; the headers follow.
  mcpHeaders.reset();
  char line[256];
  bool ended = false;
  for (int i = 0; i < 40 && !ended; i++) {
    const int n = mcpReadLine(client, line, sizeof(line), startMs);
    if (n == -1) return;  // timed out or gone: just close
    if (n == -2) return mcpRespondError(client, 431, -32000, "Header line too long");
    if (n == 0) {
      ended = true;
    } else {
      mcpHeaders.addLine(line, n);
    }
  }
  if (!ended) return mcpRespondError(client, 431, -32000, "Too many header lines");
  mcpBodyLeft = mcpHeaders.contentLength > 0 ? static_cast<size_t>(mcpHeaders.contentLength) : 0;

  // FW-MCP-010, FW-MCP-011, FW-SEC-012: before anything else, so a browser cannot relay requests.
  if (mcpCheckHostAndOrigin(mcpHeaders, WiFi.localIP().toString().c_str(), kHostname)) {
    return mcpRespondError(client, 403, -32000, "Forbidden: Host or Origin not allowed");
  }
  // FW-MCP-002: no server-to-client stream (GET) and no sessions (DELETE).
  if (method != "POST") {
    static const char kBody[] = "{\"error\":\"method not allowed\"}";
    return mcpRespond(client, 405, kBody, method == "HEAD" ? 0 : sizeof(kBody) - 1, "Allow: POST\r\n");
  }
  // FW-MCP-009: the same check, backoff included, as authorized() for /api (FW-SEC-005..009, 014).
  const uint32_t ip = client.remoteIP();
  if (!bearerMatches(mcpHeaders.authorization, mcpHeaders.authorizationLen, kApiToken)) {
    waitRendering(authBackoff.onFailure(ip, millis()));
    static const char kBody[] = "{\"error\":\"unauthorized\"}";
    return mcpRespond(client, 401, kBody, sizeof(kBody) - 1, "WWW-Authenticate: Bearer\r\n");
  }
  authBackoff.onSuccess(ip);

  switch (mcpCheckRequest(mcpHeaders)) {
    case 0: break;
    case 400:
      if (mcpHeaders.contentLengthBad) return mcpRespondError(client, 400, -32600, "Bad Content-Length");
      {  // FW-MCP-007
        char versionError[192];
        const size_t n = writeMcpVersionError(versionError, sizeof(versionError));
        return mcpRespond(client, 400, versionError, n, "");
      }
    case 411: return mcpRespondError(client, 411, -32600, "Content-Length required");
    default: return mcpRespondError(client, 413, -32000, "Request body too large (limit 2048 bytes)");  // FW-MCP-012
  }

  // FW-BUD-003..005: per-request buffers, none over 4 KB, freed when the request is done.
  const size_t len = static_cast<size_t>(mcpHeaders.contentLength);
  std::unique_ptr<char[]> body(new (std::nothrow) char[len + 1]);
  if (!body) return mcpRespondError(client, 503, -32000, "Out of memory");
  if (!mcpReadBody(client, body.get(), len, startMs)) return;
  mcpBodyLeft = 0;
  body[len] = '\0';
  std::unique_ptr<uint8_t[]> arena(new (std::nothrow) uint8_t[kMcpArenaBytes]);
  std::unique_ptr<char[]> out(new (std::nothrow) char[kMcpMaxResponseBytes]);
  if (!arena || !out) return mcpRespondError(client, 503, -32000, "Out of memory");

  const McpReply r = handleMcpMessage(body.get(), len, kVersion, mcpGlue, arena.get(), kMcpArenaBytes, out.get(), kMcpMaxResponseBytes);
  mcpRespond(client, r.status, out.get(), r.length, "");
}

// FW-SEC-013, FW-BUD-004: the size check of every route but /mcp. The hook only gets the path, and
// these routes need their query, so unlike /mcp the server keeps the request: this peeks at the
// headers and leaves them unread. It answers and returns false when the server must not read the
// body. peekBytes sees one TCP segment, so headers split across segments are refused; a split could
// otherwise hide a Content-Length from here, and no client here sends them so.
bool bodyFits(const String& url, WiFiClient& client) {
  char head[768];
  const uint32_t startMs = millis();
  size_t end = 0;
  bool found = false;
  mcpBodyLeft = 0;
  while (!found) {
    const int have = client.available();
    const size_t want = static_cast<size_t>(have) < sizeof(head) ? static_cast<size_t>(have) : sizeof(head);
    const size_t got = want ? client.peekBytes(reinterpret_cast<uint8_t*>(head), want) : 0;
    for (size_t i = 0; i + 1 < got && !found; i += 1) {
      if (head[i] == '\r' && head[i + 1] == '\n' && (i == 0 || (i >= 2 && head[i - 2] == '\r' && head[i - 1] == '\n'))) {
        found = true;
        end = i;
      }
    }
    if (found) break;
    // got < want: the first segment ended and the peek will never get further.
    if (got == sizeof(head) || got < want || !client.connected() || millis() - startMs >= 2000) {
      static const char kHeaders[] = "{\"error\":\"request headers too long\"}";
      mcpBodyLeft = have;  // swallowed before closing: see mcpRespond
      mcpRespond(client, 431, kHeaders, sizeof(kHeaders) - 1, "");
      return false;
    }
    renderIfDue();
    delay(1);
  }

  mcpHeaders.reset();
  mcpHeaders.addLines(head, end);
  mcpBodyLeft = end + 2 + (mcpHeaders.contentLength > 0 ? static_cast<size_t>(mcpHeaders.contentLength) : 0);

  const int refusal = checkBodyHeaders(mcpHeaders, kFxMaxBodyBytes);
  if (refusal == 0) {
    // FW-BUD-006: the server's own body read blocks for seconds on a stalled client, freezing the tubes
    // and stretching a beep. So it only gets a body that has already arrived whole.
    const size_t whole = end + 2 + (mcpHeaders.contentLength > 0 ? static_cast<size_t>(mcpHeaders.contentLength) : 0);
    while (static_cast<size_t>(client.available()) < whole) {
      if (!client.connected() || millis() - startMs >= 2000) {
        static const char kStalled[] = "{\"error\":\"request body too slow\"}";
        mcpBodyLeft = client.available();
        mcpRespond(client, 408, kStalled, sizeof(kStalled) - 1, "");
        return false;
      }
      renderIfDue();
      delay(1);
    }
    mcpBodyLeft = 0;
    return true;
  }
  if (refusal == 411) {
    static const char kUnreadable[] = "{\"error\":\"need a Content-Length\"}";
    mcpRespond(client, 411, kUnreadable, sizeof(kUnreadable) - 1, "");
    return false;
  }
  // FW-SEC-005: under /api the token still comes first, with its backoff.
  if (url.startsWith("/api/")) {
    const uint32_t ip = client.remoteIP();
    if (!bearerMatches(mcpHeaders.authorization, mcpHeaders.authorizationLen, kApiToken)) {
      waitRendering(authBackoff.onFailure(ip, millis()));
      static const char kUnauthorized[] = "{\"error\":\"unauthorized\"}";
      mcpRespond(client, 401, kUnauthorized, sizeof(kUnauthorized) - 1, "WWW-Authenticate: Bearer\r\n");
      return false;
    }
    authBackoff.onSuccess(ip);
  }
  static const char kTooLarge[] = "{\"error\":\"body too large (limit 1024 bytes)\"}";
  static const char kNotPlain[] = "{\"error\":\"multipart bodies are not read\"}";
  if (refusal == 415) {
    mcpRespond(client, 415, kNotPlain, sizeof(kNotPlain) - 1, "");
  } else {
    mcpRespond(client, 413, kTooLarge, sizeof(kTooLarge) - 1, "");
  }
  return false;
}

// Only these methods make the web server read a body; for the rest it reads no further than the
// headers, so a request needs no check here and headers of any shape keep working.
bool methodHasBody(const String& method) {
  return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
}

void setupServer() {
  // The core always collects Authorization; the JSON routes also need Content-Type (OQ-26).
  server.collectHeaders("Content-Type");
  server.addHook([](const String& method, const String& url, WiFiClient* client, ESP8266WebServer::ContentTypeFunction) {
    if (url == "/mcp") {
      handleMcp(method, *client);
      return ESP8266WebServer::CLIENT_MUST_STOP;
    }
    if (methodHasBody(method) && !bodyFits(url, *client)) return ESP8266WebServer::CLIENT_MUST_STOP;
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  });
  server.on("/health", HTTP_GET, [] { server.send(200, "text/plain", "ok"); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/tube", HTTP_POST, handleTube);
  server.on("/api/brightness", HTTP_POST, handleBrightness);
  server.on("/api/hours", HTTP_POST, handleHours);
  server.on("/api/tz", HTTP_POST, handleTz);
  server.on("/api/attention", HTTP_POST, handleAttention);
  server.on("/api/attention", HTTP_DELETE, handleAttention);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/effect", HTTP_GET, handleEffect);
  server.on("/api/effect", HTTP_PUT, handleEffect);
  server.on("/api/presets", HTTP_GET, handlePresets);
  server.on("/api/preset", HTTP_GET, handlePreset);
  server.on("/api/preset", HTTP_POST, handlePreset);
  server.on("/api/preset", HTTP_PUT, handlePreset);
  server.on("/api/preset", HTTP_DELETE, handlePreset);
  server.on("/api/display", HTTP_GET, handleDisplay);
  server.on("/api/display", HTTP_POST, handleDisplay);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ---- Wi-Fi ----

void setupWifi() {
  WiFi.persistent(false);  // keep credentials out of the SDK's config sector
  WiFi.mode(WIFI_STA);     // never start the access point
  WiFi.hostname(kHostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(kWifiSsid, kWifiPassword);
  Serial.printf("[wifi] joining %s\n", kWifiSsid);
}

#ifndef HOST_MOCK
static const uint8_t kUpdateBrightness = 128;
static const uint32_t kUpdateFrameMs = 80;

void showUpdateFrame(const Frame& frame) {
#ifdef TUBECLOCK_HAS_LEDS
  for (int i = 0; i < kLedCount; i++) strip.setPixelColor(i, frame[i].r, frame[i].g, frame[i].b);
  strip.show();
#else
  (void)frame;
#endif
}

void showUpdateProgress(uint32_t done, uint32_t total) {
  showUpdateFrame(renderUpdateProgress(layout, done, total, millis() / kUpdateFrameMs, kUpdateBrightness));
}

void showUpdateFailed() {
  std::array<TubeState, kTubes> tubes{};
  for (TubeState& tube : tubes) tube = {0, {255, 0, 0}};
  showUpdateFrame(render(layout, tubes, kUpdateBrightness));
}
#endif

void onConnected() {
  Serial.printf("[wifi] connected, http://%s/ (%s.local)\n", WiFi.localIP().toString().c_str(), kHostname);
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
#ifndef HOST_MOCK
  ArduinoOTA.setHostname(kHostname);
  ArduinoOTA.setPassword(kApiToken);
  ArduinoOTA.onStart([] {
#ifdef TUBECLOCK_HAS_BUZZER
    // FW-HW-007: an update blocks the loop for seconds, and a pulse must not last that long.
    beeper.cancel();
    driveBuzzer();
#endif
    showUpdateProgress(0, 1);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    // FW-OTA-005: at most every 80 ms, since show() stops interrupts while the upload is running.
    static uint32_t lastShownMs = 0;
    const uint32_t nowMs = millis();
    if (nowMs - lastShownMs < kUpdateFrameMs) return;
    lastShownMs = nowMs;
    showUpdateProgress(done, total);
  });
  ArduinoOTA.onEnd([] {
    if (fxDirty) saveEffect();  // the restart would lose a change still waiting out its debounce
    showUpdateProgress(1, 1);
    delay(500);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if (error == OTA_AUTH_ERROR) return;  // a refused token must not disturb the clock
    showUpdateFailed();
    delay(1000);
  });
  ArduinoOTA.begin();
#endif
  applyTimeZone();  // FW-TIME-011: sync at once after (re)connecting
}

// ---- Main ----

void setup() {
  Serial.begin(115200);
  Serial.printf("\n[tubeclock] %s\n", kVersion);
  loadSettings();
  setupEffects();
#ifdef TUBECLOCK_HAS_LEDS
  strip.begin();
#endif
#ifdef TUBECLOCK_HAS_BUZZER
  digitalWrite(TUBECLOCK_BUZZER_PIN, !TUBECLOCK_BUZZER_ON_LEVEL);  // off before it becomes an output
  pinMode(TUBECLOCK_BUZZER_PIN, OUTPUT);
#endif
#ifdef TUBECLOCK_HAS_BUTTONS
  for (int pin : kButtonPins) pinMode(pin, TUBECLOCK_BTN_PRESSED_LEVEL == 0 && pin != 16 ? INPUT_PULLUP : INPUT);  // GPIO 16 has no pull-up
#endif
  setupTime();
  updateDisplay();  // FW-FX-053: EEPROM, LittleFS, first frame, and only then Wi-Fi
  setupWifi();
  setupServer();
}

void loop() {
  static bool wasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) onConnected();
  if (!connected && wasConnected) Serial.println(F("[wifi] disconnected, retrying"));
  wasConnected = connected;

  server.handleClient();
  if (connected) {
    MDNS.update();
#ifndef HOST_MOCK
    ArduinoOTA.handle();
#endif
    if (ntpRetryPending && millis() - ntpRetryFromMs >= ntpRetryDelayMs) {
      ntpRetryPending = false;
      applyTimeZone();
    }
  }

  if (fxDirty && millis() - fxDirtyMs >= kFxSaveDebounceMs) saveEffect();  // FW-FX-051

  attention.expire(millis());  // FW-ATT-004
  beeper.expire(millis());

  if (restartPending && millis() - restartFromMs >= 100) {
    if (fxDirty) saveEffect();
    Serial.println(F("[tubeclock] restarting"));
    ESP.restart();
  }

  renderIfDue();
}
