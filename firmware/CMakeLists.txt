# Host build of the hardware-independent firmware core, for unit tests on a PC.
# The ESP8285 build uses the Arduino toolchain instead (see README.md).
cmake_minimum_required(VERSION 3.20)
project(clock_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(clock_core
  core/src/attention.cpp
  core/src/auth.cpp
  core/src/backoff.cpp
  core/src/color.cpp
  core/src/display.cpp
  core/src/effect.cpp
  core/src/mcp.cpp
  core/src/rtc.cpp
  core/src/settings.cpp
  core/src/time_sync.cpp
  core/src/timefmt.cpp
)
target_include_directories(clock_core PUBLIC core/include)
# ArduinoJson (MIT), vendored as a single header; the device and emulator builds use the same file.
target_include_directories(clock_core SYSTEM PUBLIC third_party/ArduinoJson)
target_compile_options(clock_core PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(clock_core_tests test/core_tests.cpp)
target_link_libraries(clock_core_tests PRIVATE clock_core)
target_compile_options(clock_core_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME clock_core_tests COMMAND clock_core_tests)
