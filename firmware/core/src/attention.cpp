#include "clock_core/attention.h"

namespace clock_core {

void Attention::start(const Hsv& color, uint32_t nowMs, uint32_t durationMs, bool blink) {
  color_ = color;
  rgb_ = hsvToRgb(color);
  startMs_ = nowMs;
  durationMs_ = durationMs > kMaxDurationMs ? kMaxDurationMs : durationMs;
  blink_ = blink;
  active_ = durationMs_ > 0;
}

bool Attention::isActive(uint32_t nowMs) const {
  return active_ && static_cast<uint32_t>(nowMs - startMs_) < durationMs_;
}

bool Attention::blinkLit(uint32_t nowMs) const {
  if (!blink_) return true;
  return static_cast<uint32_t>(nowMs - startMs_) % kBlinkPeriodMs < kBlinkPeriodMs / 2;
}

bool Attention::apply(std::array<Hsv, kTubes>& colors, uint32_t nowMs) const {
  if (!isActive(nowMs)) return true;
  colors.fill(color_);
  return blinkLit(nowMs);
}

bool Attention::apply(std::array<Rgb, kTubes>& colors, uint32_t nowMs) const {
  if (!isActive(nowMs)) return true;
  colors.fill(rgb_);
  return blinkLit(nowMs);
}

}  // namespace clock_core
