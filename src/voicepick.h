// Kloudputer — which voice provider to use. With both a Groq and a Gemini key, one is preferred (the Voice via
// menu row) and the other steps in when the preferred one answers with an error such as a rate limit.
#pragma once
#include <cstdint>

namespace voicepick {

enum class Provider : uint8_t { Groq, Gemini };
constexpr uint32_t kRestMs = 5 * 60 * 1000;  // how long a rate-limited provider is skipped

inline const char* name(Provider p) { return p == Provider::Groq ? "Groq" : "Gemini"; }
inline Provider other(Provider p) { return p == Provider::Groq ? Provider::Gemini : Provider::Groq; }

struct Plan {
  bool any = false;         // there is a key at all
  Provider first = Provider::Groq;
  bool hasSecond = false;   // the other provider has a key too
  Provider second = Provider::Gemini;
};

// `preferredResting`: the preferred provider said "slow down" a moment ago, so the other one goes first.
inline Plan plan(bool haveGroq, bool haveGemini, Provider preferred, bool preferredResting) {
  Plan p;
  p.any = haveGroq || haveGemini;
  if (!p.any) return p;
  const auto has = [&](Provider x) { return x == Provider::Groq ? haveGroq : haveGemini; };
  if (!has(preferred)) {
    p.first = other(preferred);
    return p;
  }
  p.first = preferred;
  p.second = other(preferred);
  p.hasSecond = has(p.second);
  if (p.hasSecond && preferredResting) {
    p.first = other(preferred);
    p.second = preferred;
  }
  return p;
}

// A provider that answered with an error (rate limit, quota, rejected key, server trouble) is worth trying the
// other one for. No answer at all is usually the network, which both share.
inline bool worthFallback(int status) { return status != 0 && status != 200; }
inline bool rateLimited(int status) { return status == 429; }

// Remembers a "slow down" for kRestMs. Works across the millis() wrap.
class Rest {
 public:
  void start(uint32_t now) {
    since_ = now;
    on_ = true;
  }
  bool resting(uint32_t now) {
    if (on_ && now - since_ >= kRestMs) on_ = false;
    return on_;
  }

 private:
  uint32_t since_ = 0;
  bool on_ = false;
};

}  // namespace voicepick
