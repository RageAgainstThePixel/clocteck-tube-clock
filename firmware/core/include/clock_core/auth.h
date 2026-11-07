#pragma once

#include <cstddef>

namespace clock_core {

/// Compares in time that depends only on the lengths, so response timing doesn't leak the token.
bool constantTimeEquals(const char* a, size_t aLen, const char* b, size_t bLen);

/// True when the first `headerLen` bytes of `header` are "Bearer <token>" for the expected token. An
/// empty expected token never matches. Takes an explicit length because the ESP8266 core's
/// String::clear() only zeroes the length: c_str() on a cleared header still returns the previous
/// request's bytes, which would let a request with no header reuse the last caller's token.
bool bearerMatches(const char* header, size_t headerLen, const char* expectedToken);

/// Same, for a NUL-terminated header.
bool bearerMatches(const char* header, const char* expectedToken);

}  // namespace clock_core
