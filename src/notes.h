// Kloudputer — things you asked Claude to remember, kept in /kloudputer/notes.txt, one per line.
// They ride along in the system prompt of every chat. Plain text so the card can be edited on a laptop.
#pragma once
#include <vector>
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#endif
#include "brand.h"

namespace notes {
constexpr const char* kPath = DATA_DIR "/notes.txt";
constexpr size_t kMaxNotes = 20;
constexpr size_t kMaxChars = 150;

enum class Add { Added, Empty, Duplicate, Full };

class Notes {
 public:
  Add add(String text) {
    text = tidy(text);
    if (text.isEmpty()) return Add::Empty;
    for (const String& n : items_) {
      if (n == text) return Add::Duplicate;
    }
    if (items_.size() >= kMaxNotes) return Add::Full;
    items_.push_back(text);
    return Add::Added;
  }

  // 1-based, as the screen numbers them.
  bool remove(size_t number) {
    if (number == 0 || number > items_.size()) return false;
    items_.erase(items_.begin() + (number - 1));
    return true;
  }

  void clear() { items_.clear(); }
  const std::vector<String>& items() const { return items_; }

  String joined() const {
    String out;
    for (const String& n : items_) out += "- " + n + "\n";
    out.trim();
    return out;
  }

  // One note per line; blank lines and #comments are skipped, so a laptop-edited file loads too.
  void parse(const String& file) {
    items_.clear();
    int from = 0;
    while (from <= static_cast<int>(file.length()) && items_.size() < kMaxNotes) {
      int nl = file.indexOf('\n', from);
      if (nl < 0) nl = file.length();
      String line = tidy(file.substring(from, nl));
      from = nl + 1;
      if (line.isEmpty() || line[0] == '#') continue;
      items_.push_back(line);
    }
  }

  String serialize() const {
    String out;
    for (const String& n : items_) out += n + "\n";
    return out;
  }

 private:
  // One line, single spaces, capped.
  static String tidy(String s) {
    String out;
    bool space = false;
    for (size_t i = 0; i < s.length(); ++i) {
      const char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
        space = out.length() > 0;
        continue;
      }
      if (space) out += ' ';
      space = false;
      out += c;
    }
    return out.length() > kMaxChars ? out.substring(0, kMaxChars) : out;
  }

  std::vector<String> items_;
};

// "remember that my server is atlas" (spoken or typed) or "/remember my server is atlas" -> the note. A bare
// "remember ..." is left for Claude, since it is just as likely a question.
inline bool rememberText(const String& line, String& note) {
  String lower = line;
  lower.toLowerCase();
  static const char* const kLeads[] = {"remember that ", "/remember "};
  for (const char* lead : kLeads) {
    if (lower.startsWith(lead)) {
      note = line.substring(strlen(lead));
      return true;
    }
  }
  return false;
}

inline Notes load() {
  Notes n;
  File f = SD.open(kPath, FILE_READ);
  if (!f) return n;
  String all;
  while (f.available() && all.length() < 8192) all += static_cast<char>(f.read());
  f.close();
  n.parse(all);
  return n;
}

// Written beside the real file and swapped in, so a power cut leaves the old list rather than half a new one.
inline bool save(const Notes& n) {
  const String tmp = String(kPath) + ".tmp";
  SD.mkdir(DATA_DIR);
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  const String text = n.serialize();
  const bool wrote = f.write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length()) == text.length();
  f.close();
  if (!wrote) {
    SD.remove(tmp);
    return false;
  }
  SD.remove(kPath);
  return SD.rename(tmp, kPath);
}
}  // namespace notes
