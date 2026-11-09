// Kloudputer — Up/Down recall of what you sent before, like Claude Code's prompt history.
// RAM only. A line starting with /pair holds a pairing code, so it is never kept.
#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <vector>

namespace prompts {

class History {
 public:
  static constexpr size_t kMax = 30;

  void add(const String& line) {
    stop();
    if (line.isEmpty() || line.startsWith("/pair")) return;
    if (!items_.empty() && items_.back() == line) return;
    if (items_.size() == kMax) items_.erase(items_.begin());
    items_.push_back(line);
  }

  // Up: the next older prompt. The first Up keeps what was being typed so Down can bring it back.
  bool older(const String& typing, String& out) {
    if (items_.empty()) return false;
    if (!browsing_) {
      draft_ = typing;
      pos_ = items_.size();
      browsing_ = true;
    }
    if (pos_ == 0) return false;
    out = items_[--pos_];
    return true;
  }

  // Down: the next newer prompt, then the draft, then nothing more.
  bool newer(String& out) {
    if (!browsing_) return false;
    if (++pos_ >= items_.size()) {
      out = draft_;
      stop();
      return true;
    }
    out = items_[pos_];
    return true;
  }

  // Typing over a recalled prompt makes it a new draft.
  void stop() {
    browsing_ = false;
    draft_ = String();
  }

 private:
  std::vector<String> items_;
  String draft_;
  size_t pos_ = 0;
  bool browsing_ = false;
};

}  // namespace prompts
