// Kloudputer — the Cardputer's side of the agent protocol (connector/PROTOCOL.md, v1): pairing codes, keys,
// the encrypted envelope, plaintext messages, replay checks, report reassembly, and reading the ntfy relay.
// No radio here, so the host tests run all of it against the protocol's test vector.
#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#include "agent_crypto.h"
#include "brand.h"
#include "text.h"

namespace agent {
constexpr const char* kSalt = "claudeputer-v1";  // fixed by PROTOCOL.md: part of every key, not the product name
constexpr const char* kDefaultRelay = "https://ntfy.sh";
constexpr const char* kUserAgent = brand::kProduct;
constexpr size_t kMaxPlain = 2800;
constexpr size_t kMaxAsk = 2000;
constexpr size_t kMaxBody = 4096;

// ---------- base64url, no padding ----------
constexpr const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline String b64urlEncode(const uint8_t* data, size_t n) {
  String out;
  out.reserve((n * 4 + 2) / 3);
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = data[i] << 16 | (i + 1 < n ? data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
    const size_t chars = n - i >= 3 ? 4 : n - i + 1;
    for (size_t k = 0; k < chars; ++k) out += kB64[(v >> (18 - 6 * k)) & 63];
  }
  return out;
}

// Strict: only the base64url alphabet, no padding, and no stray bits in the last character.
inline bool b64urlDecode(const String& s, std::vector<uint8_t>& out) {
  out.clear();
  if (s.length() % 4 == 1) return false;
  out.reserve(s.length() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    const char* at = strchr(kB64, s[i]);
    if (!at || !s[i]) return false;
    acc = acc << 6 | static_cast<uint32_t>(at - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((acc >> bits) & 0xFF);
    }
  }
  return (acc & ((1u << bits) - 1)) == 0;
}

// ---------- pairing and keys ----------
// "cp1-" + base64url of the connector's 32-byte secret.
inline bool parsePairingCode(const String& code, uint8_t secret[32]) {
  std::vector<uint8_t> raw;
  if (code.length() != 47 || !code.startsWith("cp1-") || !b64urlDecode(code.substring(4), raw) || raw.size() != 32) {
    return false;
  }
  memcpy(secret, raw.data(), 32);
  return true;
}

// RFC 5869 on HMAC-SHA256.
inline bool hkdf(const uint8_t* ikm, size_t ikmLen, const char* salt, const char* info, uint8_t* out, size_t len) {
  uint8_t prk[32];
  if (len > 255 * 32 || !agentcrypto::hmacSha256(reinterpret_cast<const uint8_t*>(salt), strlen(salt), ikm, ikmLen, prk)) {
    return false;
  }
  std::vector<uint8_t> block;
  uint8_t t[32];
  size_t done = 0;
  for (uint8_t counter = 1; done < len; ++counter) {
    block.assign(t, t + (counter > 1 ? 32 : 0));
    block.insert(block.end(), info, info + strlen(info));
    block.push_back(counter);
    if (!agentcrypto::hmacSha256(prk, 32, block.data(), block.size(), t)) return false;
    const size_t take = std::min<size_t>(32, len - done);
    memcpy(out + done, t, take);
    done += take;
  }
  return true;
}

struct Keys {
  String topicUp;  // Cardputer -> connector
  String topicDown;
  uint8_t keyUp[32];
  uint8_t keyDown[32];
};

inline bool deriveKeys(const uint8_t secret[32], Keys& k) {
  uint8_t up[16], down[16];
  if (!hkdf(secret, 32, kSalt, "topic-up", up, 16) || !hkdf(secret, 32, kSalt, "topic-down", down, 16) ||
      !hkdf(secret, 32, kSalt, "key-up", k.keyUp, 32) || !hkdf(secret, 32, kSalt, "key-down", k.keyDown, 32)) {
    return false;
  }
  k.topicUp = "cp-" + b64urlEncode(up, 16);
  k.topicDown = "cp-" + b64urlEncode(down, 16);
  return true;
}

// ---------- envelope: base64url(nonce[12] || ciphertext || tag[16]), AAD = the topic ----------
inline String seal(const uint8_t key[32], const String& topic, const String& plain, const uint8_t nonce[12]) {
  if (plain.length() > kMaxPlain) return String();
  std::vector<uint8_t> box(12 + plain.length() + 16);
  memcpy(box.data(), nonce, 12);
  if (!agentcrypto::gcmSeal(key, nonce, reinterpret_cast<const uint8_t*>(topic.c_str()), topic.length(),
                            reinterpret_cast<const uint8_t*>(plain.c_str()), plain.length(), box.data() + 12,
                            box.data() + 12 + plain.length())) {
    return String();
  }
  return b64urlEncode(box.data(), box.size());
}

inline String seal(const uint8_t key[32], const String& topic, const String& plain) {
  uint8_t nonce[12];
  agentcrypto::randomBytes(nonce, 12);
  return seal(key, topic, plain, nonce);
}

// False for anything that isn't an intact message under this key and topic.
inline bool open(const uint8_t key[32], const String& topic, const String& body, String& plain) {
  std::vector<uint8_t> box;
  if (body.length() > kMaxBody || !b64urlDecode(body, box) || box.size() < 12 + 16) return false;
  const size_t len = box.size() - 12 - 16;
  std::vector<char> out(len + 1, 0);
  if (!agentcrypto::gcmOpen(key, box.data(), reinterpret_cast<const uint8_t*>(topic.c_str()), topic.length(),
                            box.data() + 12, len, box.data() + 12 + len, reinterpret_cast<uint8_t*>(out.data()))) {
    return false;
  }
  if (memchr(out.data(), 0, len)) return false;
  plain = String(out.data());
  return true;
}

// ---------- plaintext ----------
enum class Kind : uint8_t { Unknown, Ask, Cancel, Approve, Ping, Hello, Ack, Step, Approval, Part, Done, Error };

struct KindName {
  Kind kind;
  const char* name;
};
constexpr KindName kKinds[] = {{Kind::Ask, "ask"},     {Kind::Cancel, "cancel"}, {Kind::Approve, "approve"},
                               {Kind::Ping, "ping"},   {Kind::Hello, "hello"},   {Kind::Ack, "ack"},
                               {Kind::Step, "step"},   {Kind::Approval, "approval"}, {Kind::Part, "part"},
                               {Kind::Done, "done"},   {Kind::Error, "error"}};

inline const char* kindName(Kind k) {
  for (const KindName& e : kKinds) {
    if (e.kind == k) return e.name;
  }
  return "";
}

struct Message {
  Kind kind = Kind::Unknown;
  String job;  // 8 hex characters; empty for hello and ping
  uint32_t n = 0;
  uint32_t ts = 0;
  String text;     // ask, step, part, error
  String name;     // hello
  String version;  // hello
  bool busy = false;
  int i = 0, of = 0;  // part
  bool ok = false;    // done
  double secs = 0, cost = 0;
  String req, tool, detail;  // approval (phase B)
};

inline String toJson(const JsonDocument& doc) {
  std::vector<char> buf(measureJson(doc) + 1, 0);
  serializeJson(doc, buf.data(), buf.size());
  return String(buf.data());
}

// An up message, with the fields in the order the protocol's test vector has them.
inline String buildUp(Kind kind, const String& job, uint32_t n, uint32_t ts, const String& text = String()) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["k"] = kindName(kind);
  doc["j"] = job.c_str();
  doc["n"] = n;
  doc["ts"] = ts;
  if (kind == Kind::Ask) doc["text"] = text.c_str();
  return toJson(doc);
}

// False for anything that isn't a v1 message of a known kind with its common fields.
inline bool parse(const String& plain, Message& m) {
  JsonDocument doc;
  m = Message();
  if (deserializeJson(doc, plain.c_str(), plain.length()) || !doc.is<JsonObject>() || doc["v"] != 1 ||
      !doc["k"].is<const char*>() || !doc["j"].is<const char*>() || !doc["n"].is<uint32_t>() ||
      !doc["ts"].is<uint32_t>()) {
    return false;
  }
  for (const KindName& e : kKinds) {
    if (strcmp(e.name, doc["k"].as<const char*>()) == 0) m.kind = e.kind;
  }
  if (m.kind == Kind::Unknown) return false;
  m.job = doc["j"].as<const char*>();
  m.n = doc["n"];
  m.ts = doc["ts"];
  m.text = doc["text"] | "";
  m.name = doc["name"] | "";
  m.version = doc["version"] | "";
  m.busy = doc["busy"] | false;
  m.i = doc["i"] | 0;
  m.of = doc["of"] | 0;
  m.ok = doc["ok"] | false;
  m.secs = doc["secs"] | 0.0;
  m.cost = doc["cost"] | 0.0;
  m.req = doc["req"] | "";
  m.tool = doc["tool"] | "";
  m.detail = doc["detail"] | "";
  return true;
}

inline String newJobId() {
  uint8_t r[4];
  agentcrypto::randomBytes(r, 4);
  char hex[9];
  snprintf(hex, sizeof hex, "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
  return hex;
}

// ---------- replay checks ----------
// Drops a message whose ts is more than 15 minutes old or 2 minutes ahead, or whose (j, n) was seen. A
// seen pair is forgotten once its ts leaves the window, since a replay of it fails the window anyway.
class ReplayGuard {
 public:
  static constexpr int64_t kPast = 15 * 60, kAhead = 2 * 60;
  static constexpr size_t kMax = 256;

  bool accept(const String& job, uint32_t n, uint32_t ts, uint32_t now) {
    if (now == 0 || static_cast<int64_t>(now) - ts > kPast || static_cast<int64_t>(ts) - now > kAhead) return false;
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(),
                               [now](const Seen& s) { return static_cast<int64_t>(now) - s.ts > kPast; }),
                seen_.end());
    if (std::any_of(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.n == n && s.job == job; })) return false;
    seen_.push_back({job, n, ts});
    if (seen_.size() > kMax) seen_.pop_front();
    return true;
  }

 private:
  struct Seen {
    String job;
    uint32_t n;
    uint32_t ts;
  };
  std::deque<Seen> seen_;
};

// ---------- the report ----------
// Puts a report back together from its parts in any order, however often each arrives. The protocol
// doesn't say whether i counts from 0 or 1 (the Node connector uses 1), so either is accepted. Only the
// first kKeep parts are kept: the screen holds its last 300 lines (about 11 KB) anyway, and a report
// shares the heap with a TLS session.
class Report {
 public:
  static constexpr int kMaxParts = 64;
  static constexpr int kKeep = 8;

  void reset() {
    of_ = 0;
    parts_.clear();
  }

  // False when the part doesn't fit the report started so far.
  bool add(int i, int of, const String& text) {
    if (of < 1 || of > kMaxParts || i < 0 || i > of || (of_ && of != of_)) return false;
    of_ = of;
    if (i <= kKeep) parts_[i] = text;
    return true;
  }

  // The kept parts are all here, numbered from 0 or from 1.
  bool complete() const { return base() >= 0; }

  String text() const {
    String out;
    const int b = base();
    for (int i = b; b >= 0 && i < b + wanted(); ++i) {
      const auto part = parts_.find(i);
      if (part != parts_.end()) out += part->second;
    }
    if (b >= 0 && of_ > kKeep) out += "\n[The report was " + String(of_) + " parts; the Cardputer shows the first " + String(kKeep) + ".]";
    return out;
  }

  int have() const { return parts_.size(); }
  int of() const { return of_; }

 private:
  int wanted() const { return std::min(of_, kKeep); }

  int base() const {
    if (!of_) return -1;
    for (int b : {0, 1}) {
      if (b == 1 && parts_.count(0)) continue;
      bool all = true;
      for (int i = b; all && i < b + wanted(); ++i) all = parts_.count(i) > 0;
      if (all) return b;
    }
    return -1;
  }

  int of_ = 0;
  std::map<int, String> parts_;
};

// ---------- the relay (ntfy) ----------
struct RelayUrl {
  bool tls = true;
  String host;
  uint16_t port = 443;
  String path;  // base path, no trailing slash
};

inline bool printableUrl(const String& s) {
  for (size_t i = 0; i < s.length(); ++i) {
    const uint8_t c = s[i];
    if (c <= ' ' || c >= 0x7F) return false;
  }
  return true;
}

// "https://ntfy.sh", "http://10.0.0.5:8080/ntfy". Plain http only when the URL says so.
inline bool parseRelay(const String& url, RelayUrl& r) {
  r = RelayUrl();
  String rest;
  if (url.startsWith("https://")) {
    rest = url.substring(8);
  } else if (url.startsWith("http://")) {
    rest = url.substring(7);
    r.tls = false;
    r.port = 80;
  } else {
    return false;
  }
  if (url.length() > 128 || !printableUrl(url)) return false;
  int slash = rest.indexOf('/');
  if (slash < 0) slash = rest.length();
  String host = rest.substring(0, slash);
  r.path = rest.substring(slash);
  while (r.path.length() && r.path[r.path.length() - 1] == '/') r.path.remove(r.path.length() - 1, 1);
  const int colon = host.indexOf(':');
  if (colon >= 0) {
    const String port = host.substring(colon + 1);
    const long p = port.toInt();
    if (port.isEmpty() || p < 1 || p > 65535 || String(static_cast<int>(p)) != port) return false;
    r.port = p;
    host = host.substring(0, colon);
  }
  for (size_t i = 0; i < host.length(); ++i) {
    if (!isalnum(static_cast<unsigned char>(host[i])) && host[i] != '.' && host[i] != '-') return false;
  }
  if (host.isEmpty() || r.path.indexOf('?') >= 0 || r.path.indexOf('#') >= 0) return false;
  r.host = host;
  return true;
}

struct RelayEvent {
  String event;  // open, keepalive, message, poll_request
  String id;
  String message;
};

// One line of ntfy's JSON stream.
inline bool parseRelayLine(const String& line, RelayEvent& e) {
  JsonDocument doc;
  e = RelayEvent();
  if (deserializeJson(doc, line.c_str(), line.length()) || !doc["event"].is<const char*>()) return false;
  e.event = doc["event"].as<const char*>();
  e.id = doc["id"] | "";
  e.message = doc["message"] | "";
  return true;
}

inline String streamRequest(const RelayUrl& r, const String& topic, const String& since) {
  return "GET " + r.path + "/" + topic + "/json?since=" + since + " HTTP/1.0\r\nHost: " + r.host +
         "\r\nUser-Agent: " + kUserAgent + "\r\n\r\n";
}

inline String publishRequest(const RelayUrl& r, const String& topic, const String& body) {
  return "POST " + r.path + "/" + topic + " HTTP/1.0\r\nHost: " + r.host +
         "\r\nUser-Agent: " + kUserAgent + "\r\nX-Firebase: no\r\nContent-Type: text/plain\r\nContent-Length: " +
         String(static_cast<unsigned>(body.length())) + "\r\n\r\n" + body;
}

// "HTTP/1.1 429 Too Many Requests" -> 429; 0 when it isn't a status line.
inline int statusCode(const String& line) {
  if (!line.startsWith("HTTP/")) return 0;
  const int sp = line.indexOf(' ');
  return sp < 0 ? 0 : static_cast<int>(line.substring(sp + 1, sp + 4).toInt());
}

// Seconds from a "Retry-After: 30" header line; `fallback` when it's absent or a date.
inline uint32_t retryAfter(const String& header, uint32_t fallback) {
  String lower = header;
  for (size_t i = 0; i < lower.length(); ++i) lower.setCharAt(i, tolower(lower[i]));
  if (!lower.startsWith("retry-after:")) return fallback;
  String v = header.substring(12);
  v.trim();
  const long s = v.toInt();
  return s > 0 && String(static_cast<int>(s)) == v ? std::min<long>(s, 3600) : fallback;
}

// Reconnect waits: 1, 2, 4 ... seconds, at most a minute.
inline uint32_t backoffMs(int attempt) { return std::min<uint32_t>(60000, 1000u << std::min(attempt, 6)); }

// ---------- the protocol's test vector, run on whatever crypto is underneath ----------
constexpr const char* kVectorBody =
    "oKGio6SlpqeoqaqrryCiHMBGtQuISoGsA7NUwazBvByIVhFX1I-qq7TCuw1HdIzhCvja_zfG5CjEIn-oxQxZSeOv9TeDzE9lVlGiTtDAtamaB_"
    "IxJX6lkDci-Jnku0SdWvmuruLnES2klg";
constexpr const char* kVectorPlain =
    "{\"v\":1,\"k\":\"ask\",\"j\":\"0badc0de\",\"n\":1,\"ts\":1762444800,\"text\":\"is cms-api up?\"}";

// True when this device derives the vector's keys and opens and seals its message bit for bit.
inline bool selfTest() {
  uint8_t secret[32];
  Keys k;
  String plain;
  const uint8_t nonce[12] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab};
  return parsePairingCode("cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", secret) && deriveKeys(secret, k) &&
         k.topicUp == "cp-lQakgFwxcIsQkFbCO6dVXw" && open(k.keyUp, k.topicUp, kVectorBody, plain) &&
         plain == kVectorPlain && seal(k.keyUp, k.topicUp, kVectorPlain, nonce) == kVectorBody;
}
}  // namespace agent
