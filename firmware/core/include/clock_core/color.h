#pragma once

#include <cstdint>

namespace clock_core {

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

/// Hue 0-360, saturation and value 0-100, matching the stock web UI's units.
struct Hsv {
  uint16_t h = 0;
  uint8_t s = 0;
  uint8_t v = 0;
};

Rgb hsvToRgb(const Hsv& hsv);

/// Scales a color by brightness 0-255.
Rgb scale(const Rgb& color, uint8_t brightness);

/// The named colors shared with the host software's NAMED_COLORS (src/color.ts, HOST-LIB-020).
struct NamedColor {
  const char* name;
  const char* hex;
};
constexpr int kNamedColorCount = 10;
extern const NamedColor kNamedColors[kNamedColorCount];

/// "#rrggbb" or "#rgb" (the '#' is optional, hex digits in either case, surrounding spaces allowed)
/// to HSV, rounding like hexToHsv in src/color.ts.
bool hexToHsv(const char* text, Hsv& out);

/// FW-MCP-036: the grammar of parseColor in src/color.ts: a name from kNamedColors (any case),
/// "#rrggbb", "#rgb" or "h,s,v" (h 0-360, s and v 0-100). The firmware takes whole numbers only in
/// the h,s,v form. False for anything else.
bool parseColor(const char* text, Hsv& out);

/// The same grammar to RGB. Hex and named colors keep their exact bytes, which the trip through
/// parseColor's whole-number HSV would not (FW-FX-002).
bool parseColorRgb(const char* text, Rgb& out);

/// "#rrggbb" in lower case.
void formatHex(const Rgb& rgb, char (&out)[8]);

}  // namespace clock_core
