// Kloudputer — Claude Messages API client (raw HTTPS; there's no Anthropic SDK for Arduino).
// Non-streamed on purpose: one request, one JSON reply, parsed straight off the socket with a filter
// so only the text we need ever lands in RAM.
#pragma once
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "certs.h"
#include "chat_request.h"
#include "tls_diag.h"

namespace {
ChatResult post(const String& apiKey, const String& model, const std::vector<ChatMsg>& history,
                const RequestOptions& opts, int& httpCode, String& apiMessage) {
  ChatResult r;
  const uint32_t t0 = millis();
  String body = requestBody(model, history, opts);

  WiFiClientSecure client;
  client.setCACert(kRootCAs);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed straight off the stream
  http.setConnectTimeout(15000);
  http.setTimeout(65000);  // the most this 16-bit setting holds; a web search can take a while
  if (!http.begin(client, "https://api.anthropic.com/v1/messages")) {
    r.error = "could not start HTTPS";
    return r;
  }
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", apiKey);
  http.addHeader("anthropic-version", "2023-06-01");
  if (wantsFallback(model)) http.addHeader("anthropic-beta", "server-side-fallback-2026-07-01");

  httpCode = http.POST(body);
  body = String();  // free the request before parsing the reply
  if (httpCode <= 0) {
    r.error = "network: " + HTTPClient::errorToString(httpCode);
    http.end();
    r.ms = millis() - t0;
    return r;
  }

  JsonDocument filter;
  replyFilter(filter);
  JsonDocument resp;
  const DeserializationError de =
      deserializeJson(resp, http.getStream(), DeserializationOption::Filter(filter));
  const String state = de ? tlsState(client) : String();  // read before the connection is torn down
  http.end();
  r.ms = millis() - t0;

  if (httpCode != 200) {
    apiMessage = resp["error"]["message"] | "no details";
    r.error = "HTTP " + String(httpCode) + ": " + apiMessage;
    return r;
  }
  if (de) {
    r.error = String("Claude's reply stopped early (") + de.c_str() + "; " + state + ")";
    return r;
  }
  readReply(resp, r);
  return r;
}
}  // namespace

inline ChatResult anthropicChat(const String& apiKey, const String& model, const std::vector<ChatMsg>& history,
                                const RequestOptions& opts) {
  int code = 0;
  String message;
  ChatResult r = post(apiKey, model, history, opts, code, message);
  if (!r.ok && opts.webSearch && searchRejected(code, message)) {
    RequestOptions plain = opts;
    plain.webSearch = false;
    r = post(apiKey, model, history, plain, code, message);
    r.searchBlocked = true;
  }
  return r;
}

// The title Haiku suggests for a first exchange, in `text`; not ok if it can't be had. Never worth an error on
// screen. The token counts come back too, since this call is billed like any other.
inline ChatResult claudeTitle(const String& apiKey, const String& prompt, const String& reply) {
  int code = 0;
  String message;
  return post(apiKey, kTitleModel, titleMessages(prompt, reply), titleOptions(), code, message);
}
