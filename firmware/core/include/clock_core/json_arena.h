#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "clock_core/json.h"

namespace clock_core {

/// A bump allocator over the caller's buffer: the JSON documents never touch the heap, and a request
/// can never take more than the buffer (FW-BUD-003, FW-FX-073). Each block starts with its size so
/// that reallocate() can copy; freeing or growing the newest block works in place.
class FixedArena : public ArduinoJson::Allocator {
 public:
  FixedArena(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

  void* allocate(size_t size) override {
    const size_t need = kHeader + align(size);
    if (need < size || used_ + need > cap_) return nullptr;
    last_ = used_;
    memcpy(buf_ + last_, &size, sizeof(size));
    used_ += need;
    return buf_ + last_ + kHeader;
  }

  void deallocate(void* ptr) override {
    if (ptr && last_ != kNone && ptr == buf_ + last_ + kHeader) {
      used_ = last_;
      last_ = kNone;
    }
  }

  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    if (last_ != kNone && ptr == buf_ + last_ + kHeader) {
      const size_t end = last_ + kHeader + align(size);
      if (end < size || end > cap_) return nullptr;
      memcpy(buf_ + last_, &size, sizeof(size));
      used_ = end;
      return ptr;
    }
    size_t old;
    memcpy(&old, static_cast<uint8_t*>(ptr) - kHeader, sizeof(old));
    void* fresh = allocate(size);
    if (fresh) memcpy(fresh, ptr, old < size ? old : size);
    return fresh;
  }

  size_t mark() const { return used_; }
  void release(size_t mark) {
    used_ = mark;
    last_ = kNone;
  }

 private:
  static constexpr size_t kHeader = 8;
  static constexpr size_t kNone = static_cast<size_t>(-1);
  static size_t align(size_t n) { return (n + 7) & ~static_cast<size_t>(7); }

  uint8_t* buf_;
  size_t cap_;
  size_t used_ = 0;
  size_t last_ = kNone;
};

}  // namespace clock_core
