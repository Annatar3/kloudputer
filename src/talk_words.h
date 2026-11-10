// Kloudputer — the words that end talk mode when they are all you say.
#pragma once
#include <cctype>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace talk {

// True when the whole transcript is a stop phrase, whatever its case or punctuation: "Stop.", "goodbye!".
// "stop the deploy" is a request for Claude, not a stop.
inline bool saysStop(const String& transcript) {
  String said;
  for (size_t i = 0; i < transcript.length(); ++i) {
    const char c = transcript[i];
    if (isalpha(static_cast<unsigned char>(c))) said += static_cast<char>(tolower(c));
    else if ((c == ' ' || c == '\'') && said.length() && said[said.length() - 1] != ' ') said += c == '\'' ? '\'' : ' ';
  }
  said.trim();
  static const char* const kStops[] = {"stop", "stop listening", "stop talking", "goodbye", "good bye", "bye",
                                       "bye bye", "that's all", "thats all", "exit", "end talk mode"};
  for (const char* stop : kStops) {
    if (said == stop) return true;
  }
  return false;
}

}  // namespace talk
