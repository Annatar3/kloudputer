// Kloudputer — what goes to the Messages API and how the reply is read. No networking here, so the
// laptop tests can exercise it.
#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <vector>
#ifdef ARDUINO
#include <Arduino.h>
#endif

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
  int searches = 0;
  bool searchBlocked = false;  // the API refused web search, and this answer came from a retry without it
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

inline const char* kSearchPrompt =
    " You can search the web. Do it only when the answer depends on recent or changing facts, and say plainly "
    "when you used it.";

constexpr int kMaxSearches = 3;
constexpr size_t kMaxNotesChars = 1500;

// These models can decline on safety-classifier grounds; with fallbacks:"default" the API re-runs a
// declined request on a suitable model server-side instead of returning the refusal. Haiku 5.5 has no
// server-side fallback, so it's not listed.
inline bool wantsFallback(const String& model) {
  return model == "claude-opus-5-5" || model == "claude-opus-5" || model == "claude-sonnet-5-5" ||
         model == "claude-fable-5-1";
}

struct RequestOptions {
  bool spoken = false;
  bool webSearch = false;
  String notes;  // things the user asked Claude to remember, one per line
  String system;  // replaces the whole system prompt, for one-off jobs like titling a chat
  int maxTokens = 1024;
};

inline String systemPrompt(const RequestOptions& o) {
  if (o.system.length()) return o.system;
  String s = kSystemPrompt;
  if (o.spoken) s += kSpokenPrompt;
  if (o.webSearch) s += kSearchPrompt;
  if (o.notes.length()) {
    s += "\n\nThe user asked you to remember these notes. Use them when relevant and don't recite them:\n";
    s += o.notes.length() > kMaxNotesChars ? o.notes.substring(0, kMaxNotesChars) : o.notes;
  }
  return s;
}

inline String requestBody(const String& model, const std::vector<ChatMsg>& history, const RequestOptions& o) {
  JsonDocument req;
  req["model"] = model;
  req["max_tokens"] = o.maxTokens;
  req["system"] = systemPrompt(o);
  req["output_config"]["effort"] = "low";  // snappy chat; Haiku 5.5 defaults to medium
  if (wantsFallback(model)) req["fallbacks"] = "default";
  if (o.webSearch) {
    JsonObject tool = req["tools"].to<JsonArray>().add<JsonObject>();
    tool["type"] = "web_search_20250305";
    tool["name"] = "web_search";
    tool["max_uses"] = kMaxSearches;
  }
  JsonArray msgs = req["messages"].to<JsonArray>();
  for (const auto& m : history) {
    JsonObject msg = msgs.add<JsonObject>();
    msg["role"] = m.user ? "user" : "assistant";
    msg["content"] = m.text;
  }
  String body;
  serializeJson(req, body);
  return body;
}

// Keeps only what the reply reader uses, so a search result's page text never lands in RAM.
inline void replyFilter(JsonDocument& filter) {
  filter["content"][0]["type"] = true;
  filter["content"][0]["text"] = true;
  filter["stop_reason"] = true;
  filter["usage"]["input_tokens"] = true;
  filter["usage"]["output_tokens"] = true;
  filter["usage"]["server_tool_use"]["web_search_requests"] = true;
  filter["error"]["message"] = true;
}

// Whether a 400 is the API saying web search isn't available to this key, so the request can be retried
// without it.
inline bool searchRejected(int code, const String& message) {
  if (code != 400 && code != 403) return false;
  String m = message;
  m.toLowerCase();
  return m.indexOf("web_search") >= 0 || m.indexOf("web search") >= 0;
}

inline void readReply(JsonDocument& resp, ChatResult& r) {
  // With search on, the answer can arrive in several text blocks around the tool blocks.
  for (JsonObject block : resp["content"].as<JsonArray>()) {
    if (strcmp(block["type"] | "", "text") == 0) r.text += static_cast<const char*>(block["text"] | "");
  }
  r.inTok = resp["usage"]["input_tokens"] | 0;
  r.outTok = resp["usage"]["output_tokens"] | 0;
  r.searches = resp["usage"]["server_tool_use"]["web_search_requests"] | 0;
  const char* stop = resp["stop_reason"] | "";
  if (strcmp(stop, "refusal") == 0 && r.text.length() == 0) r.text = "(Claude declined this one.)";
  if (strcmp(stop, "max_tokens") == 0) r.text += " [cut off]";
  if (strcmp(stop, "pause_turn") == 0) r.text += " [search paused]";
  r.ok = true;
}

// A short name for a conversation, asked of the cheapest model after its first exchange.
inline const char* kTitleModel = "claude-haiku-5-5";

inline RequestOptions titleOptions() {
  RequestOptions o;
  o.system =
      "Give the conversation a title of three to six words. Plain ASCII, no quotes, no markdown, "
      "no punctuation at the end. Reply with the title only.";
  o.maxTokens = 24;
  return o;
}

inline std::vector<ChatMsg> titleMessages(const String& prompt, const String& reply) {
  const auto cut = [](const String& s) { return s.length() > 300 ? s.substring(0, 300) : s; };
  return {{true, "Prompt: " + cut(prompt) + "\nAnswer: " + cut(reply) + "\n\nTitle?"}};
}
