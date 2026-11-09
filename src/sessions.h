// Kloudputer — saved conversations on the SD card, one append-only JSON Lines file per chat:
//   /kloudputer/sessions/<id>.jsonl
//   {"v":1,"title":"...","model":"claude-...","created":<unix s, 0 if the clock wasn't set>}
//   {"u":"<your prompt>"}
//   {"a":"<Claude's reply>"}
// A turn is written only once its reply has arrived, so power loss costs at most the line being
// written. Readers skip lines that don't parse and keys they don't know (later versions may add some).
#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <vector>
#ifdef ARDUINO
#include <SD.h>
#endif
#include "brand.h"
#include "text.h"

namespace sessions {
constexpr const char* kDir = DATA_DIR "/sessions";
constexpr const char* kExt = ".jsonl";
// Replies are capped at 1024 tokens, which fits easily once escaped; a longer line reads as damaged.
constexpr size_t kMaxLine = 16384;
constexpr size_t kMaxHeader = 1024;
constexpr size_t kTitleChars = 32;
constexpr size_t kResumeLines = 40;  // a resume rebuilds about the last 20 turns

struct Meta {
  String id;  // the file name; ids sort in the order the chats began
  String title;
  String model;
  uint32_t created = 0;  // unix seconds; 0 when the clock wasn't set
};

inline String pathOf(const String& id) { return String(kDir) + "/" + id + kExt; }

// The first prompt as one line of ASCII, cut at a word boundary near kTitleChars.
inline String titleFrom(const String& prompt) {
  const String text = ui::asciify(prompt);
  String flat;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i] == '\n' ? ' ' : text[i];
    if (c == ' ' && (flat.isEmpty() || flat[flat.length() - 1] == ' ')) continue;
    flat += c;
  }
  flat.trim();
  if (flat.length() <= kTitleChars) return flat;
  int cut = flat.lastIndexOf(' ', kTitleChars);
  if (cut < static_cast<int>(kTitleChars) / 2) cut = kTitleChars;
  String out = flat.substring(0, cut);
  out.trim();
  return out + "...";
}

// "20261106-143205", always UTC, so ids keep their order whatever time zone the screen shows.
inline String stampOf(time_t t) {
  tm u;
  gmtime_r(&t, &u);
  char buf[16];
  strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &u);
  return buf;
}

// A new id that sorts after `newest`: the clock's stamp when it does, else `newest` plus a counter
// ("-001", "-002"...) for chats begun before the clock was set or within the same second.
inline String nextId(const String& stamp, const String& newest) {
  if (stamp.length() && (newest.isEmpty() || newest < stamp)) return stamp;
  if (newest.isEmpty()) return "00000000-000000";
  const int dash = newest.lastIndexOf('-');
  const String tail = newest.substring(dash + 1);
  bool counter = dash > 0 && tail.length() == 3;
  for (size_t i = 0; counter && i < 3; ++i) counter = isdigit(static_cast<unsigned char>(tail[i]));
  if (!counter || tail == "999") return newest + "-001";
  char next[4];
  snprintf(next, sizeof next, "%03d", static_cast<int>(tail.toInt()) + 1);
  return newest.substring(0, dash + 1) + next;
}

// "today 14:32", "Nov 6 14:32", or "-" for a chat begun before the clock was set. Local time;
// `now` is 0 while the clock isn't set, and then nothing reads as today.
inline String when(uint32_t created, time_t now) {
  if (!created) return "-";
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const time_t t = created;
  tm c, n;
  localtime_r(&t, &c);
  char buf[24];
  if (now && localtime_r(&now, &n) && c.tm_year == n.tm_year && c.tm_yday == n.tm_yday) {
    snprintf(buf, sizeof buf, "%02d:%02d", c.tm_hour, c.tm_min);
  } else {
    snprintf(buf, sizeof buf, "%s %d", kMonths[c.tm_mon], c.tm_mday);
  }
  return buf;
}

// The conversation window sent to Claude: at most maxMsgs messages and maxChars characters, oldest
// dropped first, always starting on one of your turns. Used live and when resuming.
template <typename Msg>
void trimHistory(std::vector<Msg>& h, size_t maxMsgs, size_t maxChars) {
  auto chars = [&h] {
    size_t n = 0;
    for (const auto& m : h) n += m.text.length();
    return n;
  };
  while (h.size() > maxMsgs || (h.size() > 1 && chars() > maxChars)) h.erase(h.begin());
  while (!h.empty() && !h.front().user) h.erase(h.begin());
}

// Reads '\n'-separated lines through a small chunk buffer into the caller's line buffer, so a file is
// never in memory whole. A line longer than `cap` comes back with tooLong set and its bytes skipped.
class LineReader {
 public:
  struct Line {
    const char* text = nullptr;
    size_t len = 0;
    bool tooLong = false;
    uint32_t start = 0;  // file offset of its first byte
  };

  LineReader(File& f, char* buf, size_t cap, uint32_t offset = 0) : f_(f), buf_(buf), cap_(cap), offset_(offset) {}

  bool next(Line& line) {
    line = Line{buf_, 0, false, offset_};
    bool any = false;
    while (true) {
      if (pos_ == fill_) {
        fill_ = f_.read(chunk_, sizeof chunk_);
        pos_ = 0;
        if (fill_ == 0) break;
      }
      const char c = chunk_[pos_++];
      ++offset_;
      any = true;
      if (c == '\n') break;
      if (line.len < cap_) buf_[line.len++] = c;
      else line.tooLong = true;
    }
    return any;
  }

 private:
  File& f_;
  char* buf_;
  size_t cap_;
  uint32_t offset_;
  uint8_t chunk_[256];
  size_t fill_ = 0, pos_ = 0;
};

inline void readHeader(const LineReader::Line& line, Meta& m) {
  JsonDocument doc;
  if (line.tooLong || deserializeJson(doc, line.text, line.len)) return;
  m.title = doc["title"] | "";
  m.model = doc["model"] | "";
  m.created = doc["created"] | 0u;
}

// Every session id on the card. Skips folders, other files, and the "._" files macOS leaves behind.
template <typename F>
void forEachId(F f) {
  if (!SD.exists(kDir)) return;
  File dir = SD.open(kDir);
  if (!dir || !dir.isDirectory()) return;
  const size_t ext = strlen(kExt);
  bool isDir = false;
  for (String p = dir.getNextFileName(&isDir); p.length(); p = dir.getNextFileName(&isDir)) {
    const String name = p.substring(p.lastIndexOf('/') + 1);
    if (isDir || name.startsWith(".") || !name.endsWith(kExt) || name.length() == ext) continue;
    f(name.substring(0, name.length() - ext));
  }
  dir.close();
}

inline size_t count() {
  size_t n = 0;
  forEachId([&n](const String&) { ++n; });
  return n;
}

inline String newestId() {
  String newest;
  forEachId([&newest](const String& id) {
    if (newest < id) newest = id;
  });
  return newest;
}

struct Listing {
  std::vector<Meta> items;  // newest first
  size_t total = 0;         // every session on the card
};

// The newest `max` sessions with their headers. Only each file's first line is read.
inline Listing list(size_t max) {
  Listing out;
  std::vector<String> ids;  // newest first, at most `max`
  forEachId([&](const String& id) {
    ++out.total;
    if (max == 0 || (ids.size() == max && !(ids.back() < id))) return;
    ids.insert(std::find_if(ids.begin(), ids.end(), [&id](const String& o) { return o < id; }), id);
    if (ids.size() > max) ids.pop_back();
  });
  std::unique_ptr<char[]> buf(new (std::nothrow) char[kMaxHeader]);
  for (const String& id : ids) {
    Meta m;
    m.id = id;
    File f = SD.open(pathOf(id), FILE_READ);
    if (f && buf) {
      LineReader r(f, buf.get(), kMaxHeader);
      LineReader::Line line;
      if (r.next(line)) readHeader(line, m);
    }
    if (f) f.close();
    out.items.push_back(m);
  }
  return out;
}

// Reads a session to resume it: `meta` from its header, then its last complete turns (about 20),
// oldest first, through onTurn(prompt, reply). Damaged lines, unknown keys and a prompt whose reply
// never got saved are skipped. False if the file can't be read.
template <typename F>
bool load(const String& id, Meta& meta, F onTurn) {
  meta = Meta();
  meta.id = id;
  File f = SD.open(pathOf(id), FILE_READ);
  if (!f) return false;
  std::unique_ptr<char[]> buf(new (std::nothrow) char[kMaxLine]);
  if (!buf) {
    f.close();
    return false;
  }
  LineReader::Line line;
  // Pass 1 notes where the last kResumeLines lines start, keeping no text; pass 2 parses only those.
  std::vector<uint32_t> starts;
  LineReader scan(f, nullptr, 0);
  while (scan.next(line)) {
    if (starts.size() == kResumeLines) starts.erase(starts.begin());
    starts.push_back(line.start);
  }
  f.seek(0);
  LineReader head(f, buf.get(), kMaxHeader);
  if (head.next(line)) readHeader(line, meta);
  if (!starts.empty()) {
    f.seek(starts.front());
    LineReader body(f, buf.get(), kMaxLine, starts.front());
    String prompt;
    bool pending = false;
    while (body.next(line)) {
      JsonDocument doc;
      if (line.tooLong || deserializeJson(doc, line.text, line.len)) continue;
      if (doc["u"].is<const char*>()) {
        prompt = doc["u"].as<const char*>();
        pending = true;
      } else if (doc["a"].is<const char*>() && pending) {
        onTurn(prompt, String(doc["a"].as<const char*>()));
        pending = false;
      }
    }
  }
  f.close();
  return true;
}

inline bool writeLine(File& f, const JsonDocument& doc) {
  const size_t n = measureJson(doc);
  return serializeJson(doc, f) == n && f.write(static_cast<uint8_t>('\n')) == 1;
}

inline bool writeLine(File& f, const char* key, const String& text) {
  JsonDocument doc;
  doc[key] = text.c_str();
  return writeLine(f, doc);
}

// A file cut off mid-line (power loss) would glue the next line onto the damaged one.
inline bool endsWithNewline(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return true;
  bool newline = true;
  if (const size_t size = f.size()) {
    f.seek(size - 1);
    newline = f.read() == '\n';
  }
  f.close();
  return newline;
}

// Saves one finished turn, starting the file with its header if it's new. False if the card refused.
inline bool append(const Meta& m, const String& prompt, const String& reply) {
  const String path = pathOf(m.id);
  const bool fresh = !SD.exists(path);
  if (fresh) {
    SD.mkdir(DATA_DIR);
    SD.mkdir(kDir);
  }
  const bool repair = !fresh && !endsWithNewline(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  bool ok = !repair || f.write(static_cast<uint8_t>('\n')) == 1;
  if (fresh) {
    JsonDocument header;
    header["v"] = 1;
    header["title"] = m.title.c_str();
    header["model"] = m.model.c_str();
    header["created"] = m.created;
    ok = ok && writeLine(f, header);
  }
  ok = ok && writeLine(f, "u", prompt) && writeLine(f, "a", reply);
  f.close();
  return ok;
}

inline bool remove(const String& id) { return SD.remove(pathOf(id)); }

// A model-written title as one tidy line: first line only, no surrounding quotes or closing punctuation.
inline String cleanTitle(const String& raw) {
  String t = raw;
  const int nl = t.indexOf('\n');
  if (nl >= 0) t = t.substring(0, nl);
  t = titleFrom(t);
  auto edge = [](char c) { return c == '"' || c == '\'' || c == '`' || c == '*' || c == '#' || c == ' '; };
  while (t.length() && edge(t[0])) t.remove(0, 1);
  while (t.length() && (edge(t[t.length() - 1]) || t[t.length() - 1] == '.' || t[t.length() - 1] == '!')) {
    t.remove(t.length() - 1);
  }
  return t;
}

constexpr size_t kMaxRetitleBytes = 24 * 1024;

// Replaces the title in a session's first line. Meant for right after the first exchange, when the file
// is small; a bigger one is left alone. The new file is written beside the old and swapped in.
inline bool retitle(const String& id, const String& title) {
  const String path = pathOf(id);
  File in = SD.open(path, FILE_READ);
  if (!in) return false;
  if (in.size() > kMaxRetitleBytes) {
    in.close();
    return false;
  }
  String all;
  uint8_t chunk[256];
  for (size_t n = in.read(chunk, sizeof chunk); n > 0; n = in.read(chunk, sizeof chunk)) {
    all.concat(reinterpret_cast<const char*>(chunk), n);
  }
  in.close();
  const int nl = all.indexOf('\n');
  if (nl < 0 || static_cast<size_t>(nl) > kMaxHeader) return false;
  JsonDocument header;
  if (deserializeJson(header, all.c_str(), nl)) return false;
  header["title"] = title.c_str();
  String out;
  serializeJson(header, out);
  out += all.substring(nl);

  const String tmp = path + ".tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  const bool wrote = f.write(reinterpret_cast<const uint8_t*>(out.c_str()), out.length()) == out.length();
  f.close();
  if (!wrote) {
    SD.remove(tmp);
    return false;
  }
  SD.remove(path);
  return SD.rename(tmp, path);
}

// Every word of the query appears somewhere in the chat's prompts and replies, ignoring case. The file is read in
// small pieces and never held whole. The header line is skipped: the title has its own filter.
inline bool contains(const String& id, const String& query) {
  std::vector<String> words;
  String q = query;
  q.toLowerCase();
  for (int from = 0; from < static_cast<int>(q.length());) {
    int sp = q.indexOf(' ', from);
    if (sp < 0) sp = q.length();
    if (sp > from) words.push_back(q.substring(from, sp));
    from = sp + 1;
  }
  if (words.empty()) return true;
  size_t longest = 0;
  for (const String& w : words) longest = std::max<size_t>(longest, w.length());
  File f = SD.open(pathOf(id), FILE_READ);
  if (!f) return false;
  std::vector<bool> found(words.size(), false);
  size_t missing = words.size();
  String window;  // the tail of the last piece plus this one, so a word split across two pieces still matches
  bool header = true;
  uint8_t chunk[256];
  for (size_t n = f.read(chunk, sizeof chunk); n > 0 && missing; n = f.read(chunk, sizeof chunk)) {
    size_t i = 0;
    if (header) {
      while (i < n && chunk[i] != '\n') ++i;
      if (i == n) continue;
      header = false;
      ++i;
    }
    for (; i < n; ++i) window += static_cast<char>(tolower(chunk[i]));
    for (size_t w = 0; w < words.size(); ++w) {
      if (!found[w] && window.indexOf(words[w]) >= 0) {
        found[w] = true;
        --missing;
      }
    }
    if (window.length() >= longest) window = window.substring(window.length() - (longest - 1));
  }
  f.close();
  return missing == 0;
}

// Every word of the query appears in the title, ignoring case. An empty query matches everything.
inline bool matches(const String& title, const String& query) {
  String hay = title;
  hay.toLowerCase();
  String q = query;
  q.toLowerCase();
  int from = 0;
  while (from < static_cast<int>(q.length())) {
    int sp = q.indexOf(' ', from);
    if (sp < 0) sp = q.length();
    const String word = q.substring(from, sp);
    from = sp + 1;
    if (word.length() && hay.indexOf(word) < 0) return false;
  }
  return true;
}
}  // namespace sessions
