#pragma once

// ArduinoJson (OQ-11, FW-MCP-040), vendored under firmware/third_party/ArduinoJson (MIT). The same
// settings in every build, so the host tests see what the device does.
#define ARDUINOJSON_POOL_CAPACITY 32   // slots per pool: 256 bytes on the ESP8285 instead of 1 KB
#define ARDUINOJSON_USE_LONG_LONG 1    // 64-bit JSON-RPC ids on the device too
#define ARDUINOJSON_USE_DOUBLE 1
#include <ArduinoJson.h>
