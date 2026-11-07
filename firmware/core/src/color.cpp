#include "clock_core/color.h"

namespace clock_core {

Rgb hsvToRgb(const Hsv& hsv) {
  const float s = hsv.s / 100.0f;
  const float v = hsv.v / 100.0f;
  const float hf = (hsv.h % 360) / 60.0f;
  const int sector = static_cast<int>(hf);
  const float f = hf - sector;
  const float p = v * (1 - s);
  const float q = v * (1 - f * s);
  const float t = v * (1 - (1 - f) * s);
  float r = 0, g = 0, b = 0;
  switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  auto byte = [](float x) { return static_cast<uint8_t>(x * 255.0f + 0.5f); };
  return {byte(r), byte(g), byte(b)};
}

Rgb scale(const Rgb& color, uint8_t brightness) {
  auto ch = [brightness](uint8_t c) { return static_cast<uint8_t>((c * (brightness + 1)) >> 8); };
  return {ch(color.r), ch(color.g), ch(color.b)};
}

const NamedColor kNamedColors[kNamedColorCount] = {
    {"red", "#ff0000"},  {"orange", "#ff6a00"},  {"yellow", "#ffd000"}, {"green", "#00ff00"},
    {"cyan", "#00ffff"}, {"blue", "#0000ff"},    {"purple", "#8000ff"}, {"magenta", "#ff00ff"},
    {"pink", "#ff4080"}, {"white", "#ffffff"},
};

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// [begin, end) of `text` without surrounding white space.
void trimmed(const char* text, const char*& begin, const char*& end) {
  begin = text;
  while (*begin && isSpace(*begin)) begin++;
  end = begin;
  while (*end) end++;
  while (end > begin && isSpace(end[-1])) end--;
}

// Math.round for the non-negative values used here.
int roundHalfUp(double x) { return static_cast<int>(x + 0.5); }

bool parseWhole(const char* begin, const char* end, long max, long& out) {
  while (begin < end && isSpace(*begin)) begin++;
  while (end > begin && isSpace(end[-1])) end--;
  if (begin == end || end - begin > 3) return false;
  long v = 0;
  for (const char* p = begin; p < end; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (*p - '0');
  }
  if (v > max) return false;
  out = v;
  return true;
}

bool hexBytes(const char* text, int& r, int& g, int& b) {
  const char *begin, *end;
  trimmed(text, begin, end);
  if (begin < end && *begin == '#') begin++;
  const long len = end - begin;
  if (len != 3 && len != 6) return false;
  int d[6];
  for (int i = 0; i < len; i++) {
    d[i] = hexDigit(begin[i]);
    if (d[i] < 0) return false;
  }
  if (len == 3) {
    r = d[0] * 17;
    g = d[1] * 17;
    b = d[2] * 17;
  } else {
    r = d[0] * 16 + d[1];
    g = d[2] * 16 + d[3];
    b = d[4] * 16 + d[5];
  }
  return true;
}

// The hex of the named color spelled by [begin, end), or nullptr.
const char* namedHex(const char* begin, const char* end) {
  for (const NamedColor& c : kNamedColors) {
    const char* n = c.name;
    const char* p = begin;
    while (p < end && *n && lower(*p) == *n) {
      p++;
      n++;
    }
    if (p == end && !*n) return c.hex;
  }
  return nullptr;
}

}  // namespace

bool hexToHsv(const char* text, Hsv& out) {
  if (!text) return false;
  int r, g, b;
  if (!hexBytes(text, r, g, b)) return false;
  // Doubles, like the JavaScript it mirrors, so both round the same way.
  const double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
  const double max = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
  const double min = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
  const double delta = max - min;
  double h = 0;
  if (delta > 0) {
    if (max == rf) {
      h = 60 * ((gf - bf) / delta);  // in [-60, 60], so the `% 6` in color.ts never changes it
    } else if (max == gf) {
      h = 60 * ((bf - rf) / delta + 2);
    } else {
      h = 60 * ((rf - gf) / delta + 4);
    }
  }
  if (h < 0) h += 360;
  out.h = static_cast<uint16_t>(roundHalfUp(h));
  out.s = static_cast<uint8_t>(max == 0 ? 0 : roundHalfUp(delta / max * 100));
  out.v = static_cast<uint8_t>(roundHalfUp(max * 100));
  return true;
}

bool parseColor(const char* text, Hsv& out) {
  if (!text) return false;
  const char *begin, *end;
  trimmed(text, begin, end);
  if (const char* hex = namedHex(begin, end)) return hexToHsv(hex, out);
  const char* comma1 = nullptr;
  const char* comma2 = nullptr;
  int commas = 0;
  for (const char* p = begin; p < end; p++) {
    if (*p != ',') continue;
    if (++commas == 1) comma1 = p;
    if (commas == 2) comma2 = p;
  }
  if (commas == 2) {
    long h, s, v;
    if (!parseWhole(begin, comma1, 360, h) || !parseWhole(comma1 + 1, comma2, 100, s) ||
        !parseWhole(comma2 + 1, end, 100, v)) {
      return false;
    }
    out = {static_cast<uint16_t>(h), static_cast<uint8_t>(s), static_cast<uint8_t>(v)};
    return true;
  }
  if (commas != 0) return false;
  return hexToHsv(text, out);
}

bool parseColorRgb(const char* text, Rgb& out) {
  if (!text) return false;
  const char *begin, *end;
  trimmed(text, begin, end);
  const char* hex = namedHex(begin, end);
  bool hasComma = false;
  for (const char* p = begin; p < end; p++) hasComma = hasComma || *p == ',';
  if (hasComma && !hex) {
    Hsv hsv;
    if (!parseColor(text, hsv)) return false;
    out = hsvToRgb(hsv);
    return true;
  }
  int r, g, b;
  if (!hexBytes(hex ? hex : text, r, g, b)) return false;
  out = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
  return true;
}

void formatHex(const Rgb& rgb, char (&out)[8]) {
  static const char kDigits[] = "0123456789abcdef";
  const uint8_t bytes[3] = {rgb.r, rgb.g, rgb.b};
  out[0] = '#';
  for (int i = 0; i < 3; i++) {
    out[1 + 2 * i] = kDigits[bytes[i] >> 4];
    out[2 + 2 * i] = kDigits[bytes[i] & 15];
  }
  out[7] = '\0';
}

}  // namespace clock_core
