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
#include "claude_stream.h"
#include <functional>
#include "tls_diag.h"

namespace {
ChatResult post(const String& apiKey, const String& model, const std::vector<ChatMsg>& history,
                const RequestOptions& opts, int& httpCode, String& apiMessage) {
  ChatResult r;
  const uint32_t t0 = millis();
  String body = requestBody(model, history, opts);

  TlsClient client;
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
  CountedReader reader{http.getStream()};
  const DeserializationError de = deserializeJson(resp, reader, DeserializationOption::Filter(filter));
  const String state = de ? readFailure(client, reader) : String();
  Serial.printf("[chat] body %u bytes in %lu ms tls=%d\n", static_cast<unsigned>(reader.bytes), millis() - reader.startedMs,
                client.lastReadError);
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

// What the reply is doing, for the screen: text so far is in the result that comes with it.
using ChatProgress = std::function<void(claude::StreamReader::Did, const ChatResult&)>;

namespace {
constexpr uint32_t kStreamIdleMs = 90000;  // a web search can keep the stream quiet for a while

// Like post(), but streamed: `progress` hears about each piece of text as it arrives. A reply that is cut off
// after some text keeps that text.
ChatResult postStream(const String& apiKey, const String& model, const std::vector<ChatMsg>& history,
                      const RequestOptions& opts, int& httpCode, String& apiMessage, const ChatProgress& progress) {
  ChatResult r;
  const uint32_t t0 = millis();
  RequestOptions streamed = opts;
  streamed.stream = true;
  String body = requestBody(model, history, streamed);

  TlsClient client;
  client.setCACert(kRootCAs);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding: the events come straight off the socket until it closes
  http.setConnectTimeout(15000);
  http.setTimeout(65000);
  if (!http.begin(client, "https://api.anthropic.com/v1/messages")) {
    r.error = "could not start HTTPS";
    return r;
  }
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", apiKey);
  http.addHeader("anthropic-version", "2023-06-01");
  if (wantsFallback(model)) http.addHeader("anthropic-beta", "server-side-fallback-2026-07-01");

  httpCode = http.POST(body);
  body = String();
  if (httpCode <= 0) {
    r.error = "network: " + HTTPClient::errorToString(httpCode);
    http.end();
    r.ms = millis() - t0;
    return r;
  }
  if (httpCode != 200) {  // an error arrives as plain JSON, not as a stream
    JsonDocument filter;
    filter["error"]["message"] = true;
    JsonDocument resp;
    deserializeJson(resp, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    apiMessage = resp["error"]["message"] | "no details";
    r.error = "HTTP " + String(httpCode) + ": " + apiMessage;
    r.ms = millis() - t0;
    return r;
  }

  claude::StreamReader reader(r);
  uint8_t buf[256];
  size_t bytes = 0;
  const uint32_t started = millis();
  uint32_t lastByte = started;
  while (!reader.done() && !reader.failed()) {
    const int waiting = client.available();
    if (waiting <= 0) {
      if (!client.connected() || millis() - lastByte > kStreamIdleMs) break;
      delay(5);
      continue;
    }
    const int n = client.read(buf, std::min<int>(waiting, sizeof buf));
    if (n <= 0) break;
    lastByte = millis();
    bytes += n;
    for (int i = 0; i < n; ++i) {
      const claude::StreamReader::Did did = reader.feed(static_cast<char>(buf[i]));
      if (did != claude::StreamReader::Did::Nothing && progress) progress(did, r);
    }
  }
  const String whyCut = reader.done() || reader.failed() ? String() : readFailure(client, bytes, started);
  Serial.printf("[chat] stream %u bytes in %lu ms done=%d tls=%d\n", static_cast<unsigned>(bytes), millis() - started,
                reader.done(), client.lastReadError);
  http.end();
  reader.finish(whyCut);
  r.ms = millis() - t0;
  return r;
}
}  // namespace

inline ChatResult anthropicChat(const String& apiKey, const String& model, const std::vector<ChatMsg>& history,
                                const RequestOptions& opts, const ChatProgress& progress = nullptr) {
  int code = 0;
  String message;
  ChatResult r = postStream(apiKey, model, history, opts, code, message, progress);
  if (!r.ok && opts.webSearch && searchRejected(code, message)) {
    RequestOptions plain = opts;
    plain.webSearch = false;
    r = postStream(apiKey, model, history, plain, code, message, progress);
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
