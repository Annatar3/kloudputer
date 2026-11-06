// Kloudputer — text helpers shared by the screen, speech and the session store.
#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace ui {
// Font0 is ASCII-only: map common Unicode punctuation from Claude to ASCII, '?' for the rest.
inline String asciify(const String& in) {
  String out;
  out.reserve(in.length());
  const uint8_t* s = reinterpret_cast<const uint8_t*>(in.c_str());
  const size_t n = in.length();
  for (size_t i = 0; i < n;) {
    const uint8_t c = s[i];
    if (c < 0x80) {
      if (c == '\t') out += "  ";
      else if (c >= 0x20 || c == '\n') out += static_cast<char>(c);
      ++i;
      continue;
    }
    uint32_t cp = 0;
    int len = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { ++i; continue; }
    if (i + len > n) break;
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += len;
    switch (cp) {
      case 0x2013: case 0x2014: case 0x2212: out += '-'; break;
      case 0x2018: case 0x2019: case 0x2032: out += '\''; break;
      case 0x201C: case 0x201D: case 0x2033: out += '"'; break;
      case 0x2026: out += "..."; break;
      case 0x2022: case 0x00B7: out += '*'; break;
      case 0x00A0: case 0x2009: case 0x202F: out += ' '; break;
      case 0x2192: out += "->"; break;
      case 0x2190: out += "<-"; break;
      case 0x00D7: out += 'x'; break;
      default: out += '?'; break;
    }
  }
  return out;
}
}  // namespace ui
