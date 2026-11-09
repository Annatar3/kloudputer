// Kloudputer — permission requests from an agent, waiting for y or n on the Cardputer.
// The connector denies anything unanswered after five minutes, so this side forgets it then too.
#pragma once
#include <deque>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace approvals {

constexpr uint32_t kTtlMs = 5 * 60 * 1000;
constexpr size_t kMaxWaiting = 8;

struct Pending {
  String req;
  String tool;
  String detail;
  uint32_t at;  // millis() when it arrived
};

class Queue {
 public:
  // False when it can't be kept (no request id, a repeat, or the queue is full); the caller then denies a full one.
  enum class Add { Kept, Invalid, Repeat, Full };

  Add add(const String& req, const String& tool, const String& detail, uint32_t now) {
    if (req.isEmpty()) return Add::Invalid;
    for (const Pending& p : items_) {
      if (p.req == req) return Add::Repeat;
    }
    if (items_.size() >= kMaxWaiting) return Add::Full;
    items_.push_back({req, tool, detail, now});
    return Add::Kept;
  }

  bool empty() const { return items_.empty(); }
  size_t size() const { return items_.size(); }
  const Pending& front() const { return items_.front(); }

  Pending pop() {
    Pending p = items_.front();
    items_.pop_front();
    return p;
  }

  // Drops what has waited past the limit; `now` may have wrapped, which unsigned subtraction handles.
  size_t expire(uint32_t now) {
    size_t gone = 0;
    while (!items_.empty() && now - items_.front().at >= kTtlMs) {
      items_.pop_front();
      ++gone;
    }
    return gone;
  }

  std::deque<Pending> takeAll() {
    std::deque<Pending> all;
    all.swap(items_);
    return all;
  }

  void clear() { items_.clear(); }

 private:
  std::deque<Pending> items_;
};

enum class Answer { None, Allow, Deny };

// The first y or n in what was just typed decides it; anything else is ignored, so a stray key never allows.
inline Answer answerFrom(const String& typed) {
  for (size_t i = 0; i < typed.length(); ++i) {
    const char c = typed[i];
    if (c == 'y' || c == 'Y') return Answer::Allow;
    if (c == 'n' || c == 'N') return Answer::Deny;
  }
  return Answer::None;
}

}  // namespace approvals
