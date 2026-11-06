// Kloudputer — the connection to an agent's relay (ntfy): a stream of the agent's down topic, and
// publishing to its up topic.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <deque>
#include <memory>
#include "agent_proto.h"
#include "certs.h"

namespace agent {
// One connection at a time, since a TLS session takes ~35 KB of a tight heap: publishing closes the
// stream, and the stream then reopens from the last message id, which ntfy's cache fills in.
class Relay {
 public:
  enum class State : uint8_t { Off, Connecting, Streaming, Waiting };

  // `since` is a unix time: the first stream shows only messages from then on.
  void begin(const RelayUrl& url, const String& topicDown, uint32_t since) {
    url_ = url;
    topic_ = topicDown;
    since_ = since;
    lastId_ = "";
    attempts_ = 0;
    state_ = State::Connecting;
  }

  void stop() {
    client_.reset();
    state_ = State::Off;
  }

  // Closes the stream so something else can use TLS (speech, voice input); the next poll reopens it.
  void pause() {
    client_.reset();
    if (state_ == State::Streaming) state_ = State::Connecting;
  }

  State state() const { return state_; }
  const String& error() const { return error_; }

  // Call often. True with the next message body when one has arrived.
  bool poll(String& body) {
    if (state_ == State::Waiting && static_cast<int32_t>(millis() - retryAt_) >= 0) state_ = State::Connecting;
    if (state_ == State::Connecting && openStream()) {
      state_ = State::Streaming;
      attempts_ = 0;
      lastByte_ = millis();
      line_ = "";
    }
    if (state_ == State::Streaming) {
      uint8_t buf[256];
      for (int n; client_->available() > 0 && (n = client_->read(buf, sizeof buf)) > 0;) {
        lastByte_ = millis();
        feed(buf, n);
      }
      // ntfy sends a keepalive every 45 s; two minutes of silence means the stream died quietly.
      if (!client_->connected() || millis() - lastByte_ > 120000) {
        client_.reset();
        state_ = State::Connecting;
      }
    }
    if (inbox_.empty()) return false;
    body = inbox_.front();
    inbox_.pop_front();
    return true;
  }

  // Sends one sealed message to the up topic. The stream reopens on the next poll.
  bool publish(const String& topicUp, const String& body) {
    client_.reset();
    int code = 0;
    uint32_t wait = 0;
    const bool sent = request(publishRequest(url_, topicUp, body), code, wait);
    client_.reset();
    if (state_ != State::Off && state_ != State::Waiting) state_ = State::Connecting;
    if (sent && code == 200) return true;
    if (code == 429) fail("the relay asks to slow down", wait);
    else if (!sent) fail(error_, 0);
    else error_ = "the relay said HTTP " + String(code);
    return false;
  }

 private:
  static constexpr size_t kMaxLine = 6000;  // a 4,096-character body plus ntfy's fields
  static constexpr size_t kMaxInbox = 8;

  // Splits the stream into lines; each message event joins the inbox, and its id is where a reopened
  // stream resumes.
  void feed(const uint8_t* bytes, int n) {
    for (int i = 0; i < n; ++i) {
      if (bytes[i] != '\n') {
        if (line_.length() < kMaxLine) line_ += static_cast<char>(bytes[i]);
        continue;
      }
      RelayEvent e;
      if (line_.length() < kMaxLine && parseRelayLine(line_, e) && e.event == "message" && e.message.length()) {
        lastId_ = e.id;
        if (inbox_.size() < kMaxInbox) inbox_.push_back(e.message);
      }
      line_ = "";
    }
  }

  bool openStream() {
    int code = 0;
    uint32_t wait = 0;
    const String since = lastId_.length() ? lastId_ : String(since_);
    if (!request(streamRequest(url_, topic_, since), code, wait)) {
      fail(error_, 0);
      return false;
    }
    if (code == 200) return true;
    client_.reset();
    if (code == 429) fail("the relay asks to slow down", wait);
    else fail("the relay said HTTP " + String(code), 0);
    return false;
  }

  // Connects, sends `head`, and reads the response's status and headers; the body stays on client_.
  bool request(const String& head, int& code, uint32_t& retryAfterS) {
    if (WiFi.status() != WL_CONNECTED) {
      error_ = "offline";
      return false;
    }
    if (url_.tls) {
      auto* tls = new WiFiClientSecure();
      tls->setCACert(kRelayRootCAs);
      client_.reset(tls);
    } else {
      client_.reset(new WiFiClient());
    }
    if (!client_->connect(url_.host.c_str(), url_.port)) {
      error_ = "can't reach " + url_.host;
      client_.reset();
      return false;
    }
    client_->print(head);
    retryAfterS = 30;
    const uint32_t t0 = millis();
    String line;
    while (millis() - t0 < 15000) {
      if (!client_->available()) {
        if (!client_->connected()) break;
        delay(5);
        continue;
      }
      const int c = client_->read();
      if (c != '\n') {
        if (c != '\r' && line.length() < 512) line += static_cast<char>(c);
        continue;
      }
      if (!code) code = statusCode(line);
      else if (line.isEmpty()) return code != 0;
      retryAfterS = retryAfter(line, retryAfterS);
      line = "";
    }
    error_ = code ? "the relay stopped mid-reply" : "no answer from " + url_.host;
    client_.reset();
    return false;
  }

  void fail(const String& why, uint32_t waitS) {
    error_ = why;
    retryAt_ = millis() + (waitS ? waitS * 1000 : backoffMs(attempts_));
    ++attempts_;
    state_ = State::Waiting;
  }

  std::unique_ptr<NetworkClient> client_;
  RelayUrl url_;
  String topic_;
  String lastId_;
  uint32_t since_ = 0;
  String line_;
  std::deque<String> inbox_;
  uint32_t lastByte_ = 0, retryAt_ = 0;
  int attempts_ = 0;
  State state_ = State::Off;
  String error_;
};
}  // namespace agent
