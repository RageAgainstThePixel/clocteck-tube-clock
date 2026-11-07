#include "clock_core/auth.h"

#include <cstring>

namespace clock_core {

bool constantTimeEquals(const char* a, size_t aLen, const char* b, size_t bLen) {
  unsigned char diff = aLen == bLen ? 0 : 1;
  const size_t n = aLen < bLen ? aLen : bLen;
  for (size_t i = 0; i < n; i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

bool bearerMatches(const char* header, size_t headerLen, const char* expectedToken) {
  if (!header || !expectedToken || !*expectedToken) return false;
  static const char kPrefix[] = "Bearer ";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (headerLen < prefixLen || std::memcmp(header, kPrefix, prefixLen) != 0) return false;
  return constantTimeEquals(header + prefixLen, headerLen - prefixLen, expectedToken, std::strlen(expectedToken));
}

bool bearerMatches(const char* header, const char* expectedToken) {
  return header && bearerMatches(header, std::strlen(header), expectedToken);
}

}  // namespace clock_core
