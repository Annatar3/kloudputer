// Kloudputer — Up/Down recall of what you sent before, like Claude Code's prompt history. The newest prompts
// are also kept in /kloudputer/history.txt, one per line, so they survive a restart.
// A line starting with /pair holds a pairing code, so it is never kept.
#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <vector>
#ifdef ARDUINO
#include <SD.h>
#endif
#include "brand.h"

namespace prompts {

class History {
 public:
  static constexpr size_t kMax = 30;

  // True when the line joined the history (it wasn't empty, a pairing command, or a repeat of the last one).
  bool add(const String& line) {
    stop();
    if (line.isEmpty() || line.startsWith("/pair")) return false;
    if (!items_.empty() && items_.back() == line) return false;
    if (items_.size() == kMax) items_.erase(items_.begin());
    items_.push_back(line);
    return true;
  }

  size_t size() const { return items_.size(); }

  // Newest last, one per line.
  String serialize() const {
    String out;
    for (const String& item : items_) out += item + "\n";
    return out;
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

constexpr const char* kPath = DATA_DIR "/history.txt";
constexpr size_t kMaxFileBytes = 64 * 1024;  // past this the file is rewritten with just what is remembered

// Reads the saved prompts, oldest first, keeping the newest History::kMax. Returns the file's size.
inline size_t load(History& h) {
  File f = SD.open(kPath, FILE_READ);
  if (!f) return 0;
  const size_t size = f.size();
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    h.add(line);
  }
  f.close();
  h.stop();
  return size;
}

inline bool rewrite(const History& h) {
  SD.mkdir(DATA_DIR);
  SD.remove(kPath);
  File f = SD.open(kPath, FILE_WRITE);
  if (!f) return false;
  const String text = h.serialize();
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length()) == text.length();
  f.close();
  return ok;
}

// Adds one prompt to the end of the file. `fileBytes` is the caller's running size, so the file is trimmed back to
// what the history holds once it has grown past the limit.
inline bool append(const History& h, const String& line, size_t& fileBytes) {
  if (fileBytes + line.length() + 1 > kMaxFileBytes) {
    if (!rewrite(h)) return false;
    fileBytes = h.serialize().length();
    return true;
  }
  SD.mkdir(DATA_DIR);
  File f = SD.open(kPath, FILE_APPEND);
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.length()) == line.length() &&
                  f.write(static_cast<uint8_t>('\n')) == 1;
  f.close();
  if (ok) fileBytes += line.length() + 1;
  return ok;
}

}  // namespace prompts
