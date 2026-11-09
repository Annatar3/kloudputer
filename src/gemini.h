// Kloudputer — voice through Google Gemini's REST API: speech-to-text and text-to-speech.
// voice.h and tts.h use this instead of Groq when the voice key is a Google AI key (starts with "AIza").
// Nothing here is held whole in RAM: the recording is base64-encoded off the SD card as it uploads, and
// the spoken reply is base64-decoded straight into a WAV file on the SD card.
#pragma once
#include <ArduinoJson.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include "brand.h"
#include "certs.h"

namespace gemini {
constexpr const char* kHost = "generativelanguage.googleapis.com";
constexpr const char* kSttPath = "/v1beta/models/gemini-2.5-flash:generateContent";
constexpr const char* kTtsPath = "/v1beta/models/gemini-2.5-flash-preview-tts:generateContent";
constexpr uint32_t kRate = 24000;  // Gemini speech is 16-bit mono PCM at 24 kHz
constexpr uint32_t kIdleMs = 15000;
// Same order as tts::kVoices (autumn, diana, hannah, austin, daniel, troy): three lighter, three deeper voices.
constexpr const char* kVoices[] = {"Kore", "Leda", "Aoede", "Charon", "Orus", "Fenrir"};

inline bool isKey(const String& key) { return key.startsWith("AIza"); }

// The reply body with HTTP chunked framing hidden: Google may chunk even an HTTP/1.0 reply.
struct Body {
  WiFiClientSecure& c;
  bool chunked = false;
  long left = 0;
  bool done = false;

  int next() {  // one byte, or -1 at the end or after a stall
    if (done) return -1;
    if (chunked && left == 0) {
      String line;
      for (int i = 0; i < 3 && line.isEmpty(); ++i) {  // the size line, after the CRLF that ends a chunk
        line = c.readStringUntil('\n');
        line.trim();
      }
      left = strtol(line.c_str(), nullptr, 16);
      if (left <= 0) {
        done = true;
        return -1;
      }
    }
    const uint32_t t0 = millis();
    while (!c.available()) {
      if (!c.connected() || millis() - t0 > kIdleMs) {
        done = true;
        return -1;
      }
      delay(1);
    }
    if (chunked) --left;
    return c.read();
  }
};

// Sends one POST (the body comes from `sendBody`, which returns false if the upload broke), then reads the
// status line and headers. Returns the HTTP status, or 0 with `err` set when there was none.
template <typename F>
inline int post(WiFiClientSecure& c, const char* path, const String& key, size_t length, F sendBody, bool& chunked,
                String& err) {
  c.setCACert(kRootCAs);
  c.setTimeout(30000);  // ms in Arduino 3.x
  if (!c.connect(kHost, 443)) {
    err = String("can't reach ") + kHost;
    return 0;
  }
  c.print(String("POST ") + path + " HTTP/1.0\r\nHost: " + kHost + "\r\nx-goog-api-key: " + key +
          "\r\nContent-Type: application/json\r\nContent-Length: " + String(length) + "\r\nConnection: close\r\n\r\n");
  if (!sendBody()) {
    c.stop();
    err = "upload interrupted";
    return 0;
  }
  const String statusLine = c.readStringUntil('\n');  // "HTTP/1.0 200 OK"
  const int code = statusLine.length() >= 12 ? statusLine.substring(9, 12).toInt() : 0;
  chunked = false;
  while (c.connected() || c.available()) {
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
    h.toLowerCase();
    if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") > 0) chunked = true;
  }
  if (code == 0) {
    c.stop();
    err = "no reply from Gemini";
  }
  return code;
}

inline String errorText(Body& b, int code) {
  String s;
  for (int ch; s.length() < 2048 && (ch = b.next()) >= 0;) s += static_cast<char>(ch);
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  deserializeJson(doc, s, DeserializationOption::Filter(filter));
  if (code == 429) return "Gemini quota used up, try later";
  return "Gemini HTTP " + String(code) + ": " + String(doc["error"]["message"] | "no details");
}

// The words in the WAV at `path`. False with `err` set on any failure.
inline bool transcribe(const String& key, const String& path, String& text, String& err) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    err = "no recording on SD";
    return false;
  }
  const size_t size = f.size();
  const String pre =
      R"({"contents":[{"parts":[{"text":"Transcribe this audio. Reply with only the words spoken, nothing else. If there is no speech, reply with nothing."},{"inlineData":{"mimeType":"audio/wav","data":")";
  const String tail = "\"}}]}]}";
  WiFiClientSecure c;
  bool chunked = false;
  static uint8_t in[2046];  // a multiple of 3, so every piece but the last encodes without padding
  static uint8_t out[2048 + 4 + 2048 / 2];
  const int code = post(
      c, kSttPath, key, pre.length() + (size + 2) / 3 * 4 + tail.length(),
      [&] {
        c.print(pre);
        while (f.available()) {
          const size_t n = f.read(in, sizeof(in));
          size_t len = 0;
          mbedtls_base64_encode(out, sizeof(out), &len, in, n);
          if (c.write(out, len) != len) return false;
        }
        c.print(tail);
        return true;
      },
      chunked, err);
  f.close();
  if (code == 0) return false;

  Body b{c, chunked};
  if (code != 200) {
    err = errorText(b, code);
    c.stop();
    return false;
  }
  String s;
  for (int ch; s.length() < 4096 && (ch = b.next()) >= 0;) s += static_cast<char>(ch);
  c.stop();
  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) {
    err = "bad Gemini reply";
    return false;
  }
  text = String(doc["candidates"][0]["content"]["parts"][0]["text"] | "");
  text.trim();
  return true;
}

inline void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  auto put32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  auto put16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);  // PCM
  put16(22, 1);  // mono
  put32(24, kRate);
  put32(28, kRate * 2);
  put16(32, 2);
  put16(34, 16);
  memcpy(h + 36, "data", 4);
  put32(40, dataBytes);
  f.seek(0);
  f.write(h, sizeof(h));
}

inline int b64Value(int ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+' || ch == '-') return 62;  // '-' and '_' are the URL-safe forms
  if (ch == '/' || ch == '_') return 63;
  return -1;
}

// Speaks `text` into a 24 kHz mono WAV at `path`. `voice` indexes kVoices. `bytes` is the audio size written.
inline bool speak(const String& key, size_t voice, const String& text, const String& path, uint32_t& bytes,
                  int& status, String& err) {
  String body;
  {
    JsonDocument req;
    req["contents"][0]["parts"][0]["text"] = text;
    req["generationConfig"]["responseModalities"][0] = "AUDIO";
    req["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] =
        kVoices[voice < sizeof(kVoices) / sizeof(kVoices[0]) ? voice : 0];
    serializeJson(req, body);
  }
  WiFiClientSecure c;
  bool chunked = false;
  status = post(c, kTtsPath, key, body.length(), [&] { return c.print(body) == body.length(); }, chunked, err);
  body = String();
  if (status == 0) return false;

  Body b{c, chunked};
  if (status != 200) {
    err = errorText(b, status);
    c.stop();
    return false;
  }
  SD.mkdir(DATA_DIR);
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    c.stop();
    err = "can't write to SD";
    return false;
  }
  uint8_t zero[44] = {0};
  f.write(zero, sizeof(zero));  // the real header goes in once the length is known

  // Find the "data" key, then base64-decode its string value until the closing quote.
  static const char kKey[] = "\"data\"";
  enum { FindKey, FindValue, Decode } state = FindKey;
  int matched = 0;
  uint32_t acc = 0;
  int have = 0;
  uint8_t pcm[384];
  size_t n = 0;
  bytes = 0;
  auto flush = [&] {
    if (n && f.write(pcm, n) != n) err = "SD write failed";
    bytes += n;
    n = 0;
  };
  for (int ch; err.isEmpty() && (ch = b.next()) >= 0;) {
    if (state == FindKey) {
      if (ch == kKey[matched]) {
        if (kKey[++matched] == 0) state = FindValue;
      } else {
        matched = ch == '"' ? 1 : 0;
      }
    } else if (state == FindValue) {
      if (ch == '"') state = Decode;  // the opening quote of the value
    } else {
      if (ch == '"') break;
      const int v = b64Value(ch);
      if (v < 0) continue;  // '=' padding
      acc = acc << 6 | v;
      if (++have == 4) {
        pcm[n++] = acc >> 16;
        pcm[n++] = acc >> 8;
        pcm[n++] = acc;
        acc = 0;
        have = 0;
        if (n > sizeof(pcm) - 3) flush();
      }
    }
  }
  if (have == 2) pcm[n++] = acc >> 4;
  if (have == 3) {
    pcm[n++] = acc >> 10;
    pcm[n++] = acc >> 2;
  }
  flush();
  c.stop();
  bytes &= ~1u;  // whole 16-bit samples only
  if (err.isEmpty() && bytes == 0) err = "Gemini sent no audio";
  if (err.isEmpty()) writeWavHeader(f, bytes);
  f.close();
  return err.isEmpty();
}
}  // namespace gemini
