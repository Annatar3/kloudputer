// Kloudputer — tries API keys for free: listing a provider's models costs nothing but needs a valid key.
#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "certs.h"

namespace keycheck {
// The HTTP status of an authenticated GET, or a negative HTTPClient error. The body is never read.
inline int get(const char* url, const char* header, const String& value, const char* header2 = nullptr,
               const char* value2 = nullptr) {
  WiFiClientSecure tls;
  tls.setCACert(kRootCAs);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader(header, value);
  if (header2) http.addHeader(header2, value2);
  const int code = http.GET();
  http.end();
  return code;
}

inline int anthropic(const String& key) {
  return get("https://api.anthropic.com/v1/models", "x-api-key", key, "anthropic-version", "2023-06-01");
}

inline int groq(const String& key) { return get("https://api.groq.com/openai/v1/models", "Authorization", "Bearer " + key); }

inline int gemini(const String& key) {
  return get("https://generativelanguage.googleapis.com/v1beta/models?pageSize=1", "x-goog-api-key", key);
}

inline bool works(int code) { return code == 200; }

// "works", "rejected (401)", "couldn't be checked (read Timeout)"...
inline String verdict(int code) {
  if (works(code)) return "works";
  if (code == 401 || code == 403) return "rejected (" + String(code) + ")";
  if (code < 0) return "couldn't be checked (" + HTTPClient::errorToString(code) + ")";
  return "got HTTP " + String(code);
}
}  // namespace keycheck
