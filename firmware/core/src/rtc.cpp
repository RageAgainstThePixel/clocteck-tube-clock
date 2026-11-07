#include "clock_core/rtc.h"

namespace clock_core {

namespace {

constexpr int64_t kUsPerSecond = 1000000;
constexpr int64_t kSecPerDay = 86400;
constexpr int64_t kDaysTo2000 = 10957;  // 1970-01-01 to 2000-01-01
constexpr int kCenturyYears = 100;

// Every fourth year from 2000 to 2099 is a leap year, 2000 included.
bool isLeap(int year) { return year % 4 == 0; }
int yearDays(int year) { return isLeap(year) ? 366 : 365; }
int monthDays(int year, int month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return kDays[month - 1] + (month == 2 && isLeap(year) ? 1 : 0);
}

bool fromBcd(uint8_t value, int min, int max, int& out) {
  const int tens = value >> 4;
  const int ones = value & 0x0F;
  if (tens > 9 || ones > 9) return false;
  out = tens * 10 + ones;
  return out >= min && out <= max;
}

uint8_t toBcd(int value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }

}  // namespace

RtcStatus decodeRtc(const uint8_t (&regs)[kRtcRegisterCount], int64_t& epochUs) {
  if (regs[0] & 0x80) return RtcStatus::kHalted;
  int sec, min, hour, day, month, year;
  if (!fromBcd(regs[0], 0, 59, sec) || !fromBcd(regs[1], 0, 59, min)) return RtcStatus::kBadValue;
  if (regs[2] & 0x80) return RtcStatus::kBadValue;
  if (regs[2] & 0x40) {  // 12-hour mode: bit 5 is PM
    if (!fromBcd(regs[2] & 0x1F, 1, 12, hour)) return RtcStatus::kBadValue;
    hour = hour % 12 + (regs[2] & 0x20 ? 12 : 0);
  } else if (!fromBcd(regs[2], 0, 23, hour)) {
    return RtcStatus::kBadValue;
  }
  if (!fromBcd(regs[5], 1, 12, month) || !fromBcd(regs[6], 0, kCenturyYears - 1, year)) return RtcStatus::kBadValue;
  if (!fromBcd(regs[4], 1, monthDays(year, month), day)) return RtcStatus::kBadValue;

  int64_t days = kDaysTo2000 + day - 1;
  for (int y = 0; y < year; y++) days += yearDays(y);
  for (int m = 1; m < month; m++) days += monthDays(year, m);
  epochUs = (days * kSecPerDay + hour * 3600 + min * 60 + sec) * kUsPerSecond;
  return RtcStatus::kOk;
}

bool encodeRtc(int64_t epochUs, uint8_t (&regs)[kRtcRegisterCount]) {
  if (epochUs < 0) return false;
  const int64_t sec = (epochUs + kUsPerSecond / 2) / kUsPerSecond;
  int64_t left = sec / kSecPerDay - kDaysTo2000;
  if (left < 0) return false;
  const int weekday = static_cast<int>((sec / kSecPerDay + 4) % 7);  // 1970-01-01 was a Thursday
  int year = 0;
  while (year < kCenturyYears && left >= yearDays(year)) left -= yearDays(year++);
  if (year == kCenturyYears) return false;
  int month = 1;
  while (left >= monthDays(year, month)) left -= monthDays(year, month++);

  const int secOfDay = static_cast<int>(sec % kSecPerDay);
  regs[0] = toBcd(secOfDay % 60);
  regs[1] = toBcd(secOfDay / 60 % 60);
  regs[2] = toBcd(secOfDay / 3600);
  regs[3] = static_cast<uint8_t>(weekday + 1);  // 1 = Sunday
  regs[4] = toBcd(static_cast<int>(left) + 1);
  regs[5] = toBcd(month);
  regs[6] = toBcd(year);
  return true;
}

}  // namespace clock_core
