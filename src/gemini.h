// Kloudputer — voice through Google's Gemini API, used when the voice key is a Google AI key instead of a Groq
// one. Speech-to-text uploads the recording as base64 while reading it from the SD card; text-to-speech decodes
// the reply's base64 straight into a WAV file on the card.
#pragma once
#include <ArduinoJson.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include "certs.h"
#include "gemini_codec.h"
#include "tls_diag.h"

namespace gemini {
constexpr uint32_t kIdleMs = 20000;

// Connects and sends the request head. HTTP/1.0, so the reply isn't chunked and can be read straight off the socket.
inline bool begin(WiFiClientSecure& c, const String& key, const char* model, size_t bodyLength, String& error) {
  c.setCACert(kRootCAs);  // Google's API is under the same roots as the other two
  c.setTimeout(30000);
  if (!c.connect(kHost, 443)) {
    error = "can't reach Google";
    return false;
  }
  c.print(String("POST ") + path(model) + " HTTP/1.0\r\nHost: " + kHost + "\r\nx-goog-api-key: " + key +
          "\r\nContent-Type: application/json\r\nContent-Length: " + String(bodyLength) + "\r\nConnection: close\r\n\r\n");
  return true;
}

// The status code, with the headers consumed; 0 when nothing came back.
inline int status(WiFiClientSecure& c) {
  const String line = c.readStringUntil('\n');  // "HTTP/1.0 200 OK"
  const int code = line.length() >= 12 ? line.substring(9, 12).toInt() : 0;
  while (c.connected() || c.available()) {
    if (c.readStringUntil('\n').length() <= 1) break;
  }
  return code;
}

inline String failure(WiFiClientSecure& c, int code) {
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  deserializeJson(doc, c, DeserializationOption::Filter(filter));
  String message = doc["error"]["message"] | "no details";
  if (message.length() > 120) message = message.substring(0, 120) + "...";
  return code ? "Gemini HTTP " + String(code) + ": " + message : String("no answer from Google");
}

// The words in the WAV at `wavPath`. `language` is an ISO 639-1 code or empty.
inline bool transcribe(const String& key, const char* wavPath, const String& language, String& text, int& code,
                       String& error) {
  code = 0;
  File f = SD.open(wavPath, FILE_READ);
  if (!f) {
    error = "no recording on SD";
    return false;
  }
  const String prefix = sttPrefix(), suffix = sttSuffix(language);
  WiFiClientSecure c;
  if (!begin(c, key, kSttModel, prefix.length() + base64Length(f.size()) + suffix.length(), error)) {
    f.close();
    return false;
  }
  c.print(prefix);
  static uint8_t raw[1536];  // a multiple of 3, so only the last piece is padded
  static char encoded[2048];
  while (f.available()) {
    const size_t n = f.read(raw, sizeof raw);
    const size_t m = base64Encode(raw, n, encoded);
    if (c.write(reinterpret_cast<const uint8_t*>(encoded), m) != m) {
      f.close();
      c.stop();
      error = "upload interrupted";
      return false;
    }
  }
  f.close();
  c.print(suffix);

  code = status(c);
  if (code != 200) {
    error = failure(c, code);
    c.stop();
    return false;
  }
  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  JsonDocument doc;
  const DeserializationError de = deserializeJson(doc, c, DeserializationOption::Filter(filter));
  const String state = de ? tlsState(c) : String();
  c.stop();
  if (de) {
    error = String("Gemini's reply stopped early (") + de.c_str() + "; " + state + ")";
    return false;
  }
  text = "";
  for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) text += static_cast<const char*>(part["text"] | "");
  text.trim();
  return true;
}

// Speaks `text` into a WAV file at `wavPath`. `voice` indexes kVoices.
inline bool speak(const String& key, size_t voice, const String& text, const String& wavPath, uint32_t& bytes, int& code,
                  String& error) {
  bytes = 0;
  code = 0;
  const String body = ttsBody(text, voice);
  WiFiClientSecure c;
  if (!begin(c, key, kTtsModel, body.length(), error)) return false;
  c.print(body);
  code = status(c);
  if (code != 200) {
    error = failure(c, code);
    c.stop();
    return false;
  }
  SD.remove(wavPath);
  File out = SD.open(wavPath, FILE_WRITE);
  if (!out) {
    c.stop();
    error = "can't write to SD";
    return false;
  }
  AudioScanner scan;
  static uint8_t in[1024], decoded[1024];
  size_t held = 0;
  bool wroteAny = false, sdOk = true;
  const auto flush = [&] {
    if (!held) return;
    if (!wroteAny && (held < 4 || memcmp(decoded, "RIFF", 4) != 0)) {  // bare samples: give them a WAV header
      uint8_t header[44];
      pcmHeader(header);
      sdOk = sdOk && out.write(header, sizeof header) == sizeof header;
    }
    wroteAny = true;
    sdOk = sdOk && out.write(decoded, held) == held;
    bytes += held;
    held = 0;
  };
  uint32_t lastByte = millis();
  while (!scan.done() && sdOk && (c.connected() || c.available()) && millis() - lastByte < kIdleMs) {
    const int n = c.available() ? c.read(in, sizeof in) : 0;
    if (n <= 0) {
      delay(2);
      continue;
    }
    lastByte = millis();
    for (int i = 0; i < n; ++i) {
      if (held == sizeof decoded) flush();
      if (scan.feed(static_cast<char>(in[i]), decoded[held])) ++held;
    }
  }
  flush();
  out.close();
  c.stop();
  if (!sdOk) error = "SD card write failed";
  else if (!scan.started()) error = "Gemini sent no audio";
  else if (!scan.done()) error = "reply cut off";
  return error.isEmpty();
}
}  // namespace gemini
