// Kloudputer — the language Whisper listens for. "Auto" lets it guess, which can misjudge an accent and
// answer in the wrong language. Only Latin-script languages are listed: the screen font is ASCII-only, so
// anything else would show as '?'.
#pragma once
#include <cstring>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace lang {

struct Language {
  const char* code;  // ISO 639-1; empty means let Whisper decide
  const char* name;
};

inline constexpr Language kLanguages[] = {
    {"", "Auto"},       {"en", "English"}, {"de", "German"}, {"fr", "French"}, {"es", "Spanish"},
    {"it", "Italian"},  {"pt", "Portuguese"}, {"nl", "Dutch"}, {"pl", "Polish"}, {"tr", "Turkish"},
};
inline constexpr int kCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

// A saved code that is no longer in the table falls back to Auto rather than sending a bad value to Groq.
inline int indexOf(const String& code) {
  for (int i = 0; i < kCount; ++i) {
    if (code == kLanguages[i].code) return i;
  }
  return 0;
}

inline int step(int index, int dir) { return ((index + dir) % kCount + kCount) % kCount; }

// The multipart field for the request, or nothing for Auto.
inline String formField(const String& boundary, const String& code) {
  if (code.isEmpty()) return String();
  return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + code + "\r\n";
}

}  // namespace lang
