#pragma once

#include <cstddef>
#include <cstdint>

namespace clock_core {

/// FW-TIME-006: the GC1307 (DS1307-compatible) time registers as bytes; the I2C transfer itself is the
/// sketch's. The chip's seven registers from 0x00 hold seconds (bit 7 is clock halt), minutes, hours,
/// day of week, day of month, month and a two-digit year, all BCD. It stores UTC.
constexpr uint8_t kRtcI2cAddress = 0x68;
constexpr uint8_t kRtcFirstRegister = 0x00;
constexpr size_t kRtcRegisterCount = 7;

enum class RtcStatus : uint8_t { kOk, kHalted, kBadValue };

/// The registers as microseconds since the Unix epoch. kHalted: the clock-halt bit is set (a fresh or
/// a stopped chip). kBadValue: a field is not BCD or not a real date, such as February 30 or a floating
/// bus. Either hour mode is read, since the chip may hold what the stock firmware wrote. The day of
/// week is not looked at: nothing here uses it, and it must not cost a good time at boot. Whether the
/// time is new enough to believe is TimeSync::begin's call (FW-TIME-013).
RtcStatus decodeRtc(const uint8_t (&regs)[kRtcRegisterCount], int64_t& epochUs);

/// Microseconds since the Unix epoch as registers: clock running, 24-hour mode, rounded to the nearest
/// second (the chip restarts its second when the seconds are written). False outside 2000..2099, the
/// years the chip can hold.
bool encodeRtc(int64_t epochUs, uint8_t (&regs)[kRtcRegisterCount]);

}  // namespace clock_core
