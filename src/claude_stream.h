// Kloudputer — reading a streamed Messages API reply (server-sent events) one character at a time, so the text
// can be shown while Claude is still writing it. Lines are handled whole when they are short, which every
// event we care about is. A long line (a web search's results arrive as one line of 20 KB or more) is skipped
// without being stored.
#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include "chat_request.h"

namespace claude {

class StreamReader {
 public:
  enum class Did : uint8_t { Nothing, Text, Searching, Done, Failed };

  explicit StreamReader(ChatResult& result) : r_(result) {}

  // One character of the reply body in; what it completed, if anything. After Text, the reply so far is in the
  // result's text.
  Did feed(char ch) {
    if (ch != '\n') {
      if (len_ < sizeof line_ - 1) line_[len_++] = ch;
      else overflow_ = true;
      return Did::Nothing;
    }
    line_[len_] = 0;
    const Did did = overflow_ ? skipped() : handle();
    len_ = 0;
    overflow_ = false;
    return did;
  }

  bool done() const { return done_; }
  bool failed() const { return failed_; }

  // Call when the stream has ended, however it ended. Applies the stop reason, and decides what a reply that
  // was cut off is worth: text that already arrived is kept and marked, nothing at all is an error.
  void finish(const String& whyCut = String()) {
    if (failed_) return;
    if (!done_) {
      if (r_.text.isEmpty()) {
        r_.error = "Claude's reply stopped before any text" + (whyCut.length() ? " (" + whyCut + ")" : String());
        return;
      }
      r_.text += " [connection lost]";
    }
    if (lostText_) r_.text += " [part missing]";
    if (stop_ == "refusal" && r_.text.isEmpty()) r_.text = "(Claude declined this one.)";
    if (stop_ == "max_tokens") r_.text += " [cut off]";
    if (stop_ == "pause_turn") r_.text += " [search paused]";
    r_.ok = true;
  }

 private:
  Did handle() {
    if (strncmp(line_, "data:", 5) != 0) return Did::Nothing;  // "event:" lines repeat what the data says
    JsonDocument doc;
    if (deserializeJson(doc, line_ + 5)) return Did::Nothing;
    const char* type = doc["type"] | "";
    if (strcmp(type, "content_block_delta") == 0) {
      if (strcmp(doc["delta"]["type"] | "", "text_delta") != 0) return Did::Nothing;
      r_.text += static_cast<const char*>(doc["delta"]["text"] | "");
      return Did::Text;
    }
    if (strcmp(type, "content_block_start") == 0) {
      return strcmp(doc["content_block"]["type"] | "", "server_tool_use") == 0 ? Did::Searching : Did::Nothing;
    }
    if (strcmp(type, "message_start") == 0) {
      r_.inTok = doc["message"]["usage"]["input_tokens"] | 0;
      return Did::Nothing;
    }
    if (strcmp(type, "message_delta") == 0) {
      stop_ = static_cast<const char*>(doc["delta"]["stop_reason"] | "");
      r_.outTok = doc["usage"]["output_tokens"] | r_.outTok;
      r_.inTok = doc["usage"]["input_tokens"] | r_.inTok;  // the final count includes what a search added
      r_.searches = doc["usage"]["server_tool_use"]["web_search_requests"] | 0;
      return Did::Nothing;
    }
    if (strcmp(type, "message_stop") == 0) {
      done_ = true;
      return Did::Done;
    }
    if (strcmp(type, "error") == 0) {
      r_.error = String("Claude: ") + static_cast<const char*>(doc["error"]["message"] | "the stream reported an error");
      failed_ = true;
      return Did::Failed;
    }
    return Did::Nothing;
  }

  // A line too long to keep. None of ours should be, but say so if text was in one.
  Did skipped() {
    if (strstr(line_, "\"text_delta\"")) lostText_ = true;
    return Did::Nothing;
  }

  ChatResult& r_;
  char line_[1536];
  size_t len_ = 0;
  bool overflow_ = false, done_ = false, failed_ = false, lostText_ = false;
  String stop_;
};

}  // namespace claude
