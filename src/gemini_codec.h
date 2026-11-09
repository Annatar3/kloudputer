// Kloudputer — the parts of Gemini voice that need no network: which key is whose, the request JSON, and
// base64 in both directions as a stream, so neither the recording nor the spoken reply is ever held in RAM.
#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace gemini {
constexpr const char* kHost = "generativelanguage.googleapis.com";
// "-latest" follows Google's current small model, so a retired version doesn't break voice input.
constexpr const char* kSttModel = "gemini-flash-lite-latest";
constexpr const char* kTtsModel = "gemini-3.8-flash-lite-tts";
// In the order of tts::kVoices (three lighter voices, then three deeper ones).
constexpr const char* kVoices[] = {"Kore", "Leda", "Aoede", "Charon", "Orus", "Fenrir"};
// Gemini has no 200-character cap, so a reply is spoken in one request: the same length Groq gets in three.
constexpr size_t kMaxSpeakChars = 570;

// Google AI Studio keys: "AIza..." for years, "AQ...." since 2026.
inline bool isKey(const String& key) { return key.startsWith("AIza") || key.startsWith("AQ."); }

inline String path(const char* model) { return String("/v1beta/models/") + model + ":generateContent"; }

// ---------- base64 out (the recording, as it uploads) ----------
inline size_t base64Length(size_t bytes) { return (bytes + 2) / 3 * 4; }

// Encodes `n` bytes into `out` (room for base64Length(n)). Feed multiples of 3 until the last piece, since each
// call pads its own tail.
inline size_t base64Encode(const uint8_t* in, size_t n, char* out) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t a = in[i], b = i + 1 < n ? in[i + 1] : 0, c = i + 2 < n ? in[i + 2] : 0;
    const uint32_t v = a << 16 | b << 8 | c;
    out[o++] = kAlphabet[v >> 18];
    out[o++] = kAlphabet[v >> 12 & 63];
    out[o++] = i + 1 < n ? kAlphabet[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  return o;
}

// ---------- base64 in (the spoken reply, as it downloads) ----------
class Base64Decoder {
 public:
  // One character in; true when it completed a byte, which is then in `out`. Padding and anything that isn't
  // base64 produce nothing.
  bool feed(char ch, uint8_t& out) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+' || ch == '-') v = 62;
    else if (ch == '/' || ch == '_') v = 63;
    else return false;
    acc_ = (acc_ << 6 | static_cast<uint32_t>(v)) & 0xFFFFFF;
    bits_ += 6;
    if (bits_ < 8) return false;
    bits_ -= 8;
    out = static_cast<uint8_t>(acc_ >> bits_);
    return true;
  }

 private:
  uint32_t acc_ = 0;
  int bits_ = 0;
};

// Reads a generateContent reply one character at a time and hands back the bytes of the first "data" string
// (the audio), decoded. Everything else in the JSON is skipped without being stored.
class AudioScanner {
 public:
  bool feed(char ch, uint8_t& out) {
    switch (state_) {
      case State::Outside:
        if (ch == '"') {
          state_ = State::InString;
          keyLen_ = 0;
          escaped_ = false;
        }
        return false;
      case State::InString:
        if (escaped_) {
          escaped_ = false;
          keyLen_ = sizeof key_;  // an escape can't be part of the key we want
        } else if (ch == '\\') {
          escaped_ = true;
        } else if (ch == '"') {
          state_ = keyLen_ == 4 && memcmp(key_, "data", 4) == 0 ? State::AfterKey : State::Outside;
        } else if (keyLen_ < sizeof key_) {
          key_[keyLen_++] = ch;
        }
        return false;
      case State::AfterKey:
        if (ch == ' ' || ch == ':' || ch == '\n' || ch == '\r' || ch == '\t') return false;
        state_ = ch == '"' ? State::InData : State::Outside;
        return false;
      case State::InData:
        if (ch == '"') {
          state_ = State::Done;
          return false;
        }
        return decoder_.feed(ch, out);
      case State::Done:
        return false;
    }
    return false;
  }

  bool started() const { return state_ == State::InData || state_ == State::Done; }
  bool done() const { return state_ == State::Done; }

 private:
  enum class State : uint8_t { Outside, InString, AfterKey, InData, Done };
  State state_ = State::Outside;
  char key_[5] = {};
  size_t keyLen_ = 0;
  bool escaped_ = false;
  Base64Decoder decoder_;
};

// A 44-byte header for 24 kHz mono 16-bit PCM, for a reply that arrives as bare samples. The sizes are left at
// zero, which the player reads as "to the end of the file".
inline void pcmHeader(uint8_t out[44]) {
  static const uint8_t kHeader[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                      1, 0, 1, 0, 0xC0, 0x5D, 0, 0, 0x80, 0xBB, 0, 0, 2, 0, 16, 0,
                                      'd', 'a', 't', 'a', 0, 0, 0, 0};
  memcpy(out, kHeader, sizeof kHeader);
}

// ---------- requests ----------
// Speech-to-text sends the WAV inline; these are the JSON before and after its base64.
inline String sttPrefix() { return "{\"contents\":[{\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\""; }

// `language` is an ISO 639-1 code, or empty to let the model tell.
inline String sttSuffix(const String& language) {
  JsonDocument doc;
  String ask = "Transcribe this recording word for word. Reply with only the words spoken, nothing else.";
  if (language.length()) ask += " The speaker's language code is " + language + ".";
  ask += " If nothing is said, reply with nothing.";
  doc.set(ask.c_str());
  String quoted;
  serializeJson(doc, quoted);
  return "\"}},{\"text\":" + quoted + "}]}]}";
}

// The text is sent bare: a style note such as "say warmly:" gets read out by this model.
inline String ttsBody(const String& text, size_t voice) {
  JsonDocument doc;
  doc["contents"][0]["parts"][0]["text"] = text.length() > kMaxSpeakChars ? text.substring(0, kMaxSpeakChars).c_str() : text.c_str();
  JsonObject config = doc["generationConfig"].to<JsonObject>();
  config["responseModalities"][0] = "AUDIO";
  config["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = kVoices[voice < sizeof kVoices / sizeof kVoices[0] ? voice : 0];
  String body;
  serializeJson(doc, body);
  return body;
}
}  // namespace gemini
