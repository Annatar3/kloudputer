// Kloudputer — the radio-free half of the setup network: answering DNS, telling which program on the
// phone is asking over HTTP, and the setup log. Pure, so the host tests run all of it.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "text.h"

namespace captive {
constexpr uint32_t kDnsTtl = 10;  // seconds; short, so a phone that just left setup asks real DNS again

struct DnsQuestion {
  String name;  // lowercase, dotted
  uint16_t type = 0;
};

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(v >> 8);
  out.push_back(v & 0xFF);
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  put16(out, v >> 16);
  put16(out, v & 0xFFFF);
}

// The reply to one DNS packet, with the same semantics as the core's DNSServer in captive mode: A and
// ANY get `ip`, other types NOERROR with no answer (and an SOA, which lets the phone cache that), and a
// query that isn't exactly one question a bare NXDOMAIN header. An EDNS OPT record is ignored, as the
// core does. False means stay silent: a response, a truncated or malformed packet, a compressed name.
inline bool dnsReply(const uint8_t* q, size_t len, const uint8_t ip[4], std::vector<uint8_t>& out, DnsQuestion& asked) {
  out.clear();
  asked = DnsQuestion();
  if (len < 17 || (q[2] & 0x80)) return false;
  const uint8_t opcode = (q[2] >> 3) & 0x0F;
  const uint16_t qd = q[4] << 8 | q[5], an = q[6] << 8 | q[7], ns = q[8] << 8 | q[9];
  const uint8_t flags = 0x80 | (q[2] & 0x01);  // response, keeping the "recursion desired" bit
  if (opcode != 0 || qd != 1 || an != 0 || ns != 0) {
    out = {q[0], q[1], flags, 0x80 | 3, 0, 0, 0, 0, 0, 0, 0, 0};
    return true;
  }
  size_t pos = 12;
  String name;
  while (true) {
    if (pos >= len) return false;
    const uint8_t label = q[pos];
    if (label == 0) break;
    if ((label & 0xC0) || pos + 1 + label > len || pos + 1 + label - 12 > 254) return false;
    if (name.length()) name += '.';
    for (size_t i = 1; i <= label; ++i) name += static_cast<char>(tolower(q[pos + i]));
    pos += 1 + label;
  }
  const size_t questionEnd = pos + 1 + 4;
  if (questionEnd > len) return false;
  asked.name = name;
  asked.type = q[pos + 1] << 8 | q[pos + 2];
  const bool address = asked.type == 1 || asked.type == 255;

  out = {q[0], q[1], flags, 0x80, 0, 1, 0, static_cast<uint8_t>(address), 0, static_cast<uint8_t>(!address), 0, 0};
  out.insert(out.end(), q + 12, q + questionEnd);
  out.push_back(0xC0);  // a pointer to the question's name at offset 12
  out.push_back(12);
  if (address) {
    put16(out, 1);  // A
    put16(out, 1);  // IN
    put32(out, kDnsTtl);
    put16(out, 4);
    out.insert(out.end(), ip, ip + 4);
  } else {
    put16(out, 6);  // SOA
    put16(out, 1);
    put32(out, kDnsTtl);
    put16(out, 22);
    out.push_back(0);  // primary server: the root name
    out.push_back(0);  // contact: the root name
    const uint32_t soa[] = {1, 3600, 600, 86400, kDnsTtl};  // serial, refresh, retry, expire, minimum
    for (uint32_t v : soa) put32(out, v);
  }
  return true;
}

inline String dnsTypeName(uint16_t type) {
  switch (type) {
    case 1: return "A";
    case 2: return "NS";
    case 5: return "CNAME";
    case 6: return "SOA";
    case 12: return "PTR";
    case 15: return "MX";
    case 16: return "TXT";
    case 28: return "AAAA";
    case 64: return "SVCB";
    case 65: return "HTTPS";
    case 255: return "ANY";
    default: return "TYPE" + String(static_cast<int>(type));
  }
}

enum class Client : uint8_t { Chrome, Webview, AndroidProbe, AndroidSystem, Samsung, Ios, Other };

// Which program sent an HTTP request, from its User-Agent.
inline Client classify(const String& ua) {
  if (ua.indexOf("SamsungBrowser") >= 0) return Client::Samsung;
  if (ua.indexOf("; wv)") >= 0) return Client::Webview;
  // Android's NetworkMonitor probes with a fixed 2017 desktop-Chrome User-Agent no phone browser sends.
  if (ua.indexOf("X11; Linux x86_64") >= 0 && ua.indexOf("Chrome/60.0.3112.32") >= 0) return Client::AndroidProbe;
  if (ua.indexOf("Dalvik/") >= 0) return Client::AndroidSystem;
  if (ua.indexOf("CaptiveNetworkSupport") >= 0 || ua.indexOf("iPhone") >= 0 || ua.indexOf("iPad") >= 0) {
    return Client::Ios;
  }
  if (ua.indexOf("Chrome/") >= 0) return Client::Chrome;
  return Client::Other;
}

inline const char* clientName(Client c) {
  switch (c) {
    case Client::Chrome: return "chrome";
    case Client::Webview: return "webview";
    case Client::AndroidProbe: return "android-probe";
    case Client::AndroidSystem: return "android-system";
    case Client::Samsung: return "samsung";
    case Client::Ios: return "ios";
    default: return "other";
  }
}

// RFC 8908: a client asking for the Captive Portal API sends this media type in Accept.
inline bool wantsCapport(const String& accept) {
  String lower = accept;
  for (size_t i = 0; i < lower.length(); ++i) lower.setCharAt(i, tolower(lower[i]));
  return lower.indexOf("application/captive+json") >= 0;
}

inline String capportJson(const String& portalUrl) {
  return "{\"captive\":true,\"user-portal-url\":\"" + portalUrl + "\"}";
}

inline String dnsEvent(const DnsQuestion& q) { return "dns " + q.name + " " + dnsTypeName(q.type); }

inline String httpEvent(const String& method, const String& host, const String& uri, int code, Client client,
                        bool capport) {
  return method + " " + host + uri + " " + String(code) + " " + clientName(client) + (capport ? " capport" : "");
}

inline String stationEvent(bool joined, const uint8_t mac[6], int rssi) {
  char buf[40];
  if (joined) snprintf(buf, sizeof buf, "join %02x:%02x %ddBm", mac[4], mac[5], rssi);
  else snprintf(buf, sizeof buf, "leave %02x:%02x", mac[4], mac[5]);
  return buf;
}

// The last kSize events, oldest first, each stamped with seconds since setup began.
class EventLog {
 public:
  static constexpr size_t kSize = 40;

  void add(uint32_t ms, const String& text) {
    char stamp[12];
    snprintf(stamp, sizeof stamp, "%3lus ", static_cast<unsigned long>(ms / 1000));
    if (lines_.size() == kSize) lines_.pop_front();
    lines_.push_back(String(stamp) + text);
    ++added_;
  }

  const std::deque<String>& lines() const { return lines_; }
  uint32_t added() const { return added_; }  // grows with every event, so a screen knows to redraw

 private:
  std::deque<String> lines_;
  uint32_t added_ = 0;
};
}  // namespace captive
