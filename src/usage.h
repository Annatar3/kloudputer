// Kloudputer — what the chats cost, estimated from the token counts each reply reports and the list prices
// below. It is an estimate: the Anthropic Console has the real bill.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace usage {

struct Price {
  const char* model;
  float in, out;  // dollars per million tokens
};

// The models the menu offers. A model set by hand in the config file has no price here and counts as unknown.
inline constexpr Price kPrices[] = {
    {"claude-haiku-5-5", 0.1f, 0.5f},
    {"claude-sonnet-5-5", 2, 10},
    {"claude-opus-5-5", 4, 20},
    {"claude-fable-5-1", 10, 50},
};
constexpr uint32_t kSearchMicros = 10000;  // web search is billed at $10 per 1,000 searches

inline const Price* priceOf(const String& model) {
  for (const Price& p : kPrices) {
    if (model == p.model) return &p;
  }
  return nullptr;
}

inline String dollars(float v) {
  char buf[16];
  if (v == std::floor(v)) snprintf(buf, sizeof buf, "$%d", static_cast<int>(v));
  else snprintf(buf, sizeof buf, "$%.1f", v);
  return buf;
}

// "$2/$10": per million tokens in and out, for the model menus.
inline String label(const String& model) {
  const Price* p = priceOf(model);
  return p ? dollars(p->in) + "/" + dollars(p->out) : String("?");
}

// One reply's cost in millionths of a dollar. `known` is false when the model has no price, and then only the
// searches are counted.
inline uint32_t costMicros(const String& model, int inTok, int outTok, int searches, bool* known = nullptr) {
  const Price* p = priceOf(model);
  if (known) *known = p != nullptr;
  double micros = static_cast<double>(searches > 0 ? searches : 0) * kSearchMicros;
  if (p) micros += (inTok > 0 ? inTok : 0) * static_cast<double>(p->in) + (outTok > 0 ? outTok : 0) * static_cast<double>(p->out);
  return micros >= 4294967295.0 ? 4294967295u : static_cast<uint32_t>(std::llround(micros));
}

// "$0", "$0.0004", "$0.12", "$12.30": four decimals under a cent, where chat costs usually are.
inline String money(uint32_t micros) {
  char buf[20];
  if (micros == 0) return "$0";
  if (micros < 10000) snprintf(buf, sizeof buf, "$%.4f", micros / 1e6);
  else snprintf(buf, sizeof buf, "$%.2f", micros / 1e6);
  return buf;
}

// Running totals. The day and the all-time total are saved; the chat total lives with the open chat.
struct Tally {
  uint32_t chat = 0;
  uint32_t day = 0;
  uint32_t total = 0;
  uint32_t date = 0;  // yyyymmdd of `day`, 0 until the clock has been set once
};

inline uint32_t plus(uint32_t a, uint32_t b) { return a > 4294967295u - b ? 4294967295u : a + b; }

// `today` is yyyymmdd in local time, or 0 while the clock isn't set; spending then stays in the day it was on.
inline void add(Tally& t, uint32_t micros, uint32_t today) {
  if (today && today != t.date) {
    t.date = today;
    t.day = 0;
  }
  t.chat = plus(t.chat, micros);
  t.day = plus(t.day, micros);
  t.total = plus(t.total, micros);
}

// What "today" shows: nothing spent yet if the saved day is an earlier one.
inline uint32_t spentToday(const Tally& t, uint32_t today) { return today && today != t.date ? 0 : t.day; }

}  // namespace usage
