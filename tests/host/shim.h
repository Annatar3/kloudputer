// Host stand-ins for the Arduino String / SD File API surface tts.h uses, with Arduino's semantics
// (out-of-range indexes return 0 / -1, substring clamps, lastIndexOf(from >= len) is -1).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  explicit String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  unsigned length() const { return s_.size(); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  const char* c_str() const { return s_.c_str(); }
  bool isEmpty() const { return s_.empty(); }
  void reserve(unsigned) {}
  String substring(unsigned l) const { return substring(l, length()); }
  String substring(unsigned l, unsigned r) const {
    if (l > r) std::swap(l, r);
    if (l >= length()) return String();
    if (r > length()) r = length();
    return String(s_.substr(l, r - l));
  }
  int indexOf(char c, unsigned from = 0) const {
    if (from >= length()) return -1;
    auto p = s_.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const String& t, unsigned from = 0) const {
    if (from >= length()) return -1;
    auto p = s_.find(t.s_, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const char* t, unsigned from = 0) const { return indexOf(String(t), from); }
  int lastIndexOf(char c) const { return length() ? lastIndexOf(c, length() - 1) : -1; }
  int lastIndexOf(char c, unsigned from) const {
    if (from >= length()) return -1;
    auto p = s_.rfind(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) ++a;
    while (b > a && isspace((unsigned char)s_[b - 1])) --b;
    s_ = s_.substr(a, b - a);
  }
  void remove(unsigned i) { if (i < s_.size()) s_.erase(i); }
  bool concat(const char* b, unsigned n) { s_.append(b, n); return true; }
  void remove(unsigned i, unsigned n) {
    if (i >= length()) return;
    s_.erase(i, std::min<unsigned>(n, length() - i));
  }
  void replace(const String& f, const String& r) {
    if (f.isEmpty()) return;
    std::string out;
    size_t i = 0, p;
    while ((p = s_.find(f.s_, i)) != std::string::npos) { out += s_.substr(i, p - i) + r.s_; i = p + f.s_.size(); }
    s_ = out + s_.substr(i);
  }
  bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0 && s_.size() >= p.s_.size(); }
  bool endsWith(const String& p) const {
    return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
  }
  long toInt() const { return atol(s_.c_str()); }
  void setCharAt(unsigned i, char c) {
    if (i < s_.size()) s_[i] = c;
  }
  void toLowerCase() {
    for (auto& ch : s_) ch = tolower(static_cast<unsigned char>(ch));
  }
  size_t write(uint8_t c) { s_ += static_cast<char>(c); return 1; }
  size_t write(const uint8_t* b, size_t n) { s_.append(reinterpret_cast<const char*>(b), n); return n; }
  void toUpperCase() {
    for (auto& ch : s_) ch = toupper(static_cast<unsigned char>(ch));
  }
  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* o) { s_ += o; return *this; }
  String& operator+=(char c) { s_ += c; return *this; }
  friend String operator+(String a, const String& b) { return a += b; }
  friend String operator+(String a, const char* b) { return a += b; }
  friend String operator+(const char* a, const String& b) { return String(a) += b; }
  friend bool operator==(const String& a, const String& b) { return a.s_ == b.s_; }
  friend bool operator==(const String& a, const char* b) { return a.s_ == b; }
  friend bool operator!=(const String& a, const char* b) { return a.s_ != b; }
  friend bool operator!=(const String& a, const String& b) { return a.s_ != b.s_; }
  friend bool operator<(const String& a, const String& b) { return a.s_ < b.s_; }
  const std::string& str() const { return s_; }

 private:
  std::string s_;
};

struct {
  void printf(const char*, ...) {}
} Serial;

inline uint32_t millis() { return 0; }

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

// Reads see a snapshot taken at open; writes ("w"/"a") go straight into SD's copy of the file.
struct File {
  std::shared_ptr<std::vector<uint8_t>> data;
  std::vector<uint8_t>* sink = nullptr;
  size_t pos = 0;
  bool dir = false;
  std::vector<std::string> entries;  // a directory's children, full paths, in creation order
  size_t nextEntry = 0;
  std::vector<bool> entryIsDir;
  explicit operator bool() const { return data || sink || dir; }
  size_t size() const { return data ? data->size() : sink ? sink->size() : 0; }
  size_t read(uint8_t* b, size_t n) {
    if (!data) return 0;
    n = std::min(n, size() - std::min(pos, size()));
    memcpy(b, data->data() + pos, n);
    pos += n;
    return n;
  }
  int read() {
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
  }
  bool seek(uint32_t p) { pos = p; return true; }
  int available() { return data ? static_cast<int>(size() - std::min(pos, size())) : 0; }
  String readStringUntil(char end) {
    String out;
    for (int c = read(); c >= 0 && c != end; c = read()) out += static_cast<char>(c);
    return out;
  }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) {
    if (!sink) return 0;
    sink->insert(sink->end(), b, b + n);
    return n;
  }
  bool isDirectory() const { return dir; }
  String getNextFileName(bool* isDir) {
    if (nextEntry >= entries.size()) return String();
    if (isDir) *isDir = entryIsDir[nextEntry];
    return String(entries[nextEntry++]);
  }
  void close() {}
};

struct SDFake {
  std::map<std::string, std::vector<uint8_t>> files;
  std::set<std::string> dirs{"/"};
  std::map<std::string, uint64_t> born;  // creation order, which a real FAT directory lists in
  uint64_t clock = 0;
  bool readOnly = false;

  static std::string parent(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : p.substr(0, slash);
  }
  void reset() {
    files.clear();
    dirs = {"/"};
    born.clear();
    readOnly = false;
  }
  bool exists(const String& p) const { return files.count(p.str()) || dirs.count(p.str()); }
  bool mkdir(const String& p) {
    if (readOnly || exists(p) || !dirs.count(parent(p.str()))) return false;
    dirs.insert(p.str());
    born[p.str()] = ++clock;
    return true;
  }
  bool remove(const String& p) { return !readOnly && files.erase(p.str()) > 0; }
  // Moves a file or a folder with everything in it, as FAT's rename does.
  bool rename(const String& from, const String& to) {
    const std::string f = from.str(), t = to.str();
    if (readOnly || !exists(from) || exists(to) || !dirs.count(parent(t))) return false;
    auto moved = [&](const std::string& p) { return p == f || p.rfind(f + "/", 0) == 0 ? t + p.substr(f.size()) : p; };
    std::map<std::string, std::vector<uint8_t>> nf;
    for (auto& kv : files) nf[moved(kv.first)] = std::move(kv.second);
    files = std::move(nf);
    std::set<std::string> nd;
    for (const auto& d : dirs) nd.insert(moved(d));
    dirs = std::move(nd);
    std::map<std::string, uint64_t> nb;
    for (const auto& kv : born) nb[moved(kv.first)] = kv.second;
    born = std::move(nb);
    return true;
  }
  File open(const String& p, const char* mode = FILE_READ) {
    File f;
    const std::string path = p.str();
    if (dirs.count(path)) {
      std::vector<std::pair<uint64_t, std::string>> kids;
      for (const auto& kv : files) if (parent(kv.first) == path) kids.push_back({born[kv.first], kv.first});
      for (const auto& d : dirs) if (d != path && parent(d) == path) kids.push_back({born[d], d});
      std::sort(kids.begin(), kids.end());
      f.dir = true;
      for (const auto& k : kids) {
        f.entries.push_back(k.second);
        f.entryIsDir.push_back(dirs.count(k.second) > 0);
      }
      return f;
    }
    if (mode[0] == 'r') {
      auto it = files.find(path);
      if (it != files.end()) f.data = std::make_shared<std::vector<uint8_t>>(it->second);
      return f;
    }
    if (readOnly || !dirs.count(parent(path))) return f;
    if (!files.count(path)) born[path] = ++clock;
    std::vector<uint8_t>& v = files[path];
    if (mode[0] == 'w') v.clear();
    f.sink = &v;
    return f;
  }
} SD;

namespace audio {
constexpr uint32_t kRate = 16000;
inline std::vector<int16_t> spoken;  // every frame written, interleaved L/R
inline std::vector<size_t> writes;   // frames per write
inline int unmutes = 0, mutes = 0, drains = 0;
inline std::vector<char> events;     // order of u(nmute), m(ute), d(rain), e(ndPlayback)
inline bool write(const int16_t* f, size_t bytes) {
  spoken.insert(spoken.end(), f, f + bytes / 2);
  writes.push_back(bytes / 4);
  return true;
}
inline void unmute() { ++unmutes; events.push_back('u'); }
inline void mute() { ++mutes; events.push_back('m'); }
inline void drain() { ++drains; events.push_back('d'); }
inline void endPlayback() { events.push_back('e'); }
inline bool isAdv() { return true; }
inline void reset() { spoken.clear(); writes.clear(); events.clear(); unmutes = mutes = drains = 0; }
}  // namespace audio

// NVS stand-in: namespace -> key -> value, kept for the whole test run like flash would be.
inline std::map<std::string, std::map<std::string, std::string>> g_nvs;
struct Preferences {
  std::string ns;
  bool readOnly = false;
  bool begin(const char* name, bool ro = false) {
    ns = name;
    readOnly = ro;
    return true;
  }
  void end() {}
  bool isKey(const char* key) { return g_nvs[ns].count(key) > 0; }
  String getString(const char* key, const String& fallback = String()) {
    auto it = g_nvs[ns].find(key);
    return it == g_nvs[ns].end() ? fallback : String(it->second);
  }
  bool remove(const char* key) { return !readOnly && g_nvs[ns].erase(key) > 0; }
  size_t putString(const char* key, const String& value) {
    if (readOnly) return 0;
    g_nvs[ns][key] = value.str();
    return value.length();
  }
};
