// Kloudputer — Claude Messages API client (raw HTTPS; there's no Anthropic SDK for Arduino).
// Non-streamed on purpose: one request, one JSON reply, parsed straight off the socket with a filter
// so only the text we need ever lands in RAM.
#pragma once
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "certs.h"

struct ChatMsg {
  bool user;
  String text;
};

struct ChatResult {
  bool ok = false;
  String text;
  String error;
  int inTok = 0;
  int outTok = 0;
  uint32_t ms = 0;
};

inline const char* kSystemPrompt =
    "You are Claude, talking through Kloudputer, an unofficial pocket client on an M5Stack Cardputer ADV with a tiny "
    "40-column, 10-line text screen and a small keyboard. Reply concisely in plain ASCII text. "
    "No markdown, no tables, no emoji. Use short paragraphs; for lists use '-' at line start.";

// Appended in voice mode: the reply is also read aloud, and Groq's TTS takes 200 characters per call.
inline const char* kSpokenPrompt =
    " Your replies are also spoken aloud by a tiny speaker, so answer in one to three short "
    "conversational sentences, under 200 characters when possible, with no markdown, lists, code, "
    "emoji, URLs or symbols. Sound warm and lively, like chatting with a friend.";

// These models can decline on safety-classifier grounds; with fallbacks:"default" the API re-runs a
// declined request on a suitable model server-side instead of returning the refusal. Haiku 5.5 has no
// server-side fallback, so it's not listed.
inline bool wantsFallback(const String& model) {
  return model == "claude-opus-5-5" || model == "claude-opus-5" || model == "claude-sonnet-5-5" ||
         model == "claude-fable-5-1";
}

inline ChatResult anthropicChat(const String& apiKey, const String& model,
                                const std::vector<ChatMsg>& history, bool spoken) {
  ChatResult r;
  const uint32_t t0 = millis();

  String body;
  {
    JsonDocument req;
    req["model"] = model;
    req["max_tokens"] = 1024;
    req["system"] = spoken ? String(kSystemPrompt) + kSpokenPrompt : String(kSystemPrompt);
    req["output_config"]["effort"] = "low";  // snappy chat; Haiku 5.5 defaults to medium
    if (wantsFallback(model)) req["fallbacks"] = "default";
    JsonArray msgs = req["messages"].to<JsonArray>();
    for (const auto& m : history) {
      JsonObject o = msgs.add<JsonObject>();
      o["role"] = m.user ? "user" : "assistant";
      o["content"] = m.text;
    }
    serializeJson(req, body);
  }

  WiFiClientSecure client;
  client.setCACert(kRootCAs);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed straight off the stream
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  if (!http.begin(client, "https://api.anthropic.com/v1/messages")) {
    r.error = "could not start HTTPS";
    return r;
  }
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", apiKey);
  http.addHeader("anthropic-version", "2023-06-01");
  if (wantsFallback(model)) http.addHeader("anthropic-beta", "server-side-fallback-2026-07-01");

  const int code = http.POST(body);
  body = String();  // free the request before parsing the reply
  if (code <= 0) {
    r.error = "network: " + HTTPClient::errorToString(code);
    http.end();
    r.ms = millis() - t0;
    return r;
  }

  JsonDocument filter;
  filter["content"][0]["type"] = true;
  filter["content"][0]["text"] = true;
  filter["stop_reason"] = true;
  filter["usage"]["input_tokens"] = true;
  filter["usage"]["output_tokens"] = true;
  filter["error"]["message"] = true;

  JsonDocument resp;
  const DeserializationError de =
      deserializeJson(resp, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  r.ms = millis() - t0;

  if (code != 200) {
    const char* msg = resp["error"]["message"] | "no details";
    r.error = "HTTP " + String(code) + ": " + msg;
    return r;
  }
  if (de) {
    r.error = String("bad reply: ") + de.c_str();
    return r;
  }
  for (JsonObject block : resp["content"].as<JsonArray>()) {
    if (strcmp(block["type"] | "", "text") == 0) r.text += static_cast<const char*>(block["text"] | "");
  }
  r.inTok = resp["usage"]["input_tokens"] | 0;
  r.outTok = resp["usage"]["output_tokens"] | 0;
  const char* stop = resp["stop_reason"] | "";
  if (strcmp(stop, "refusal") == 0 && r.text.length() == 0) r.text = "(Claude declined this one.)";
  if (strcmp(stop, "max_tokens") == 0) r.text += " [cut off]";
  r.ok = true;
  return r;
}
