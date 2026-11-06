// Kloudputer — WiFi, saved networks (NVS), and clock sync (TLS certificate checks need real time).
#pragma once
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

namespace net {
inline bool connect(const String& ssid, const String& pass, uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// configTime() would reset the time zone to UTC, so the zone goes in with the servers.
inline bool syncTime(const char* tz, uint32_t timeoutMs = 10000) {
  configTzTime(tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < timeoutMs) delay(200);
  return time(nullptr) >= 1700000000;
}

inline void loadSaved(String& ssid, String& pass) {
  Preferences p;
  if (!p.begin("cputer", true)) return;
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
}

inline void save(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("cputer", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

}  // namespace net
