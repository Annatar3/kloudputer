// Kloudputer — why a reply stopped early, in words for the error line: what TLS last complained about (often
// memory), and how much heap was left.
#pragma once
#include <WiFiClientSecure.h>

// Call before the client is stopped. "tls: <reason>, heap 41k, block 18k"; "tls: none" when TLS saw no error,
// which points at the server or the network closing the connection.
inline String tlsState(WiFiClientSecure& c) {
  char why[96] = "";
  const int code = c.lastError(why, sizeof why);
  String reason = code ? String(why) : String("none");
  reason.trim();
  if (reason.length() > 60) reason = reason.substring(0, 60);
  return "tls: " + reason + ", heap " + String(ESP.getFreeHeap() / 1024) + "k, block " + String(ESP.getMaxAllocHeap() / 1024) + "k";
}
