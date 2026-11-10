// Kloudputer — spoken answers: Groq Orpheus TTS to WAV files on the SD card, then playback.
//
// Replies stream straight to SD (a 200-character answer at 48 kHz is ~1 MB, far more than our heap),
// and play back from SD through audio_adv's always-on I2S clock, resampled to its 16 kHz.
#pragma once
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "audio.h"
#include "brand.h"
#include "certs.h"
#include "gemini.h"
#include "ui.h"

namespace tts {
constexpr const char* kVoices[] = {"autumn", "diana", "hannah", "austin", "daniel", "troy"};
// Orpheus speaks at 24 kHz. Asked for 16 kHz, Groq sent 24 kHz audio labelled 16 kHz (a woman's voice
// measured at 110 Hz, played 1.5x slow and deep), so ask for 24 kHz and resample on the device.
constexpr uint32_t kGroqRate = 24000;
constexpr int kMaxChunk = 190;            // Groq caps input at 200 characters
constexpr size_t kMaxChunks = 3;          // the whole answer is on screen; only its start is spoken
constexpr uint32_t kIdleMs = 15000;
constexpr size_t kOutFrames = 512;
// Groq's speech is mastered for normal speakers, so the Cardputer's tiny one plays it faintly (M5's own
// driver multiplies by up to 16 on this board). Playback multiplies by kBoost and a peak limiter pulls
// the gain back on loud syllables so they never clip: instant attack, ~60 ms release at 16 kHz.
constexpr float kBoost = 4.0f;
constexpr float kCeiling = 32000.0f;
constexpr float kRelease = 0.999f;

inline int16_t g_io[1024];               // 2 KB staging buffer between SD and network / WAV data
inline int16_t g_out[2 * kOutFrames];    // resampled L/R frames for one I2S write

inline String clipPath(size_t i) { return DATA_DIR "/say" + String(i) + ".wav"; }

inline const char* findVoice(const String& name) {
  for (const char* v : kVoices) {
    if (name == v) return v;
  }
  return nullptr;
}

// Last sentence end that fits; else a comma past mid-chunk; else the last space; else a hard cut.
inline int splitPoint(const String& s, int maxChunk = kMaxChunk) {
  int cut = 0;
  for (int i = 0; i < maxChunk; ++i) {
    if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && s[i + 1] == ' ') cut = i + 1;
  }
  if (cut) return cut;
  const int comma = s.lastIndexOf(',', maxChunk - 1);
  if (comma > maxChunk / 2) return comma + 1;
  const int space = s.lastIndexOf(' ', maxChunk);
  return space > 0 ? space : maxChunk;
}

inline bool hasWords(const String& s) {
  for (unsigned i = 0; i < s.length(); ++i) {
    if (isalnum(static_cast<unsigned char>(s[i]))) return true;
  }
  return false;
}

// Speakable pieces of an answer: markdown noise out, each line a sentence, then whole sentences
// packed greedily into at most maxChunks pieces of at most maxChunk characters. Groq needs the defaults; a provider without the
// 200-character cap passes one long piece.
inline std::vector<String> chunks(const String& answer, int maxChunk = kMaxChunk, size_t maxChunks = kMaxChunks) {
  const String text = ui::asciify(answer);
  String flat;
  int start = 0;
  while (start < static_cast<int>(text.length())) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    start = nl + 1;
    line.trim();
    if (line.startsWith("- ") || line.startsWith("> ")) line.remove(0, 2);
    for (const char* noise : {"*", "#", "`"}) line.replace(noise, "");
    while (line.indexOf("  ") >= 0) line.replace("  ", " ");
    line.trim();
    if (!hasWords(line)) continue;  // blank, or a rule like "---"
    if (!strchr(".!?,;:", line[line.length() - 1])) line += '.';
    if (flat.length()) flat += ' ';
    flat += line;
  }
  std::vector<String> out;
  while (flat.length() && out.size() < maxChunks) {
    const int cut = static_cast<int>(flat.length()) > maxChunk ? splitPoint(flat, maxChunk) : flat.length();
    String piece = flat.substring(0, cut);
    piece.trim();
    if (piece.length()) out.push_back(piece);
    flat = flat.substring(cut);
    flat.trim();
  }
  return out;
}

struct Synth {
  bool ok = false;
  int status = 0;  // HTTP status, 0 when Groq never answered
  uint32_t bytes = 0;
  uint32_t ms = 0;
  String error;
};

// One POST to Groq's speech endpoint, streaming the WAV reply to `path` in 2 KB pieces.
inline Synth request(const String& groqKey, const char* voice, const String& text, const String& path, bool askRate) {
  Synth s;
  const uint32_t t0 = millis();
  String body;
  {
    JsonDocument req;
    req["model"] = "canopylabs/orpheus-v1-english";
    req["voice"] = voice;
    req["input"] = text;
    req["response_format"] = "wav";
    if (askRate) req["sample_rate"] = kGroqRate;
    serializeJson(req, body);
  }
  WiFiClientSecure c;
  c.setCACert(kRootCAs);
  c.setTimeout(30000);  // ms in Arduino 3.x
  if (!c.connect("api.groq.com", 443)) {
    s.error = "can't reach api.groq.com";
    return s;
  }
  // HTTP/1.0 so the reply isn't chunked and the WAV bytes come straight off the socket.
  c.print("POST /openai/v1/audio/speech HTTP/1.0\r\nHost: api.groq.com\r\nAuthorization: Bearer " + groqKey +
          "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
          "\r\nConnection: close\r\n\r\n");
  c.print(body);

  const String statusLine = c.readStringUntil('\n');  // "HTTP/1.1 200 OK"
  s.status = statusLine.length() >= 12 ? statusLine.substring(9, 12).toInt() : 0;
  uint32_t length = UINT32_MAX;  // no Content-Length: read until the server closes
  long retryAfter = 0;
  while (c.connected() || c.available()) {
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
    h.toLowerCase();
    if (h.startsWith("content-length:")) length = h.substring(15).toInt();
    else if (h.startsWith("retry-after:")) retryAfter = h.substring(12).toInt();
  }

  if (s.status != 200) {
    JsonDocument filter;
    filter["error"]["message"] = true;
    JsonDocument doc;
    deserializeJson(doc, c, DeserializationOption::Filter(filter));
    c.stop();
    s.ms = millis() - t0;
    if (s.status == 0) s.error = "no reply from Groq";
    else if (s.status == 429) s.error = retryAfter > 0 ? "Groq rate limit, retry in " + String(retryAfter) + "s" : "Groq rate limit, try later";
    else s.error = "Groq HTTP " + String(s.status) + ": " + String(doc["error"]["message"] | "no details");
    return s;
  }

  SD.mkdir(DATA_DIR);
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    c.stop();
    s.error = "can't write to SD";
    return s;
  }
  uint8_t* buf = reinterpret_cast<uint8_t*>(g_io);
  uint32_t lastData = millis();
  while (s.bytes < length) {
    const int avail = c.available();
    if (avail > 0) {
      const int n = c.read(buf, std::min<int>(avail, sizeof(g_io)));
      if (n > 0) {
        if (f.write(buf, n) != static_cast<size_t>(n)) {
          s.error = "SD write failed";
          break;
        }
        s.bytes += n;
        lastData = millis();
        continue;
      }
    }
    if (!c.connected()) break;
    if (millis() - lastData > kIdleMs) {
      s.error = "Groq stalled mid-reply";
      break;
    }
    delay(1);
  }
  f.close();
  c.stop();
  s.ms = millis() - t0;
  if (s.error.isEmpty() && length != UINT32_MAX && s.bytes < length) s.error = "reply cut off";
  s.ok = s.error.isEmpty();
  return s;
}

// The sample_rate field is undocumented for Orpheus: once Groq rejects it, stop sending it.
inline Synth synthesize(const String& groqKey, const char* voice, const String& text, const String& path) {
  if (gemini::isKey(groqKey)) {  // the voice key is a Google one
    Synth g;
    const uint32_t t0 = millis();
    size_t index = 0;
    for (size_t i = 0; i < sizeof kVoices / sizeof kVoices[0]; ++i) {
      if (strcmp(voice, kVoices[i]) == 0) index = i;
    }
    g.ok = gemini::speak(groqKey, index, text, path, g.bytes, g.status, g.error);
    g.ms = millis() - t0;
    return g;
  }
  static bool askRate = true;
  Synth s = request(groqKey, voice, text, path, askRate);
  if (askRate && s.status == 400 && s.error.indexOf("sample_rate") >= 0) {
    askRate = false;
    s = request(groqKey, voice, text, path, false);
  }
  return s;
}

struct WavFormat {
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0, dataOffset = 0, dataBytes = 0;
};

inline uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
inline uint32_t le32(const uint8_t* p) { return le16(p) | static_cast<uint32_t>(le16(p + 2)) << 16; }

// Walks the RIFF chunks. A streamed WAV may carry a placeholder data size (0, 0xFFFFFFFF, or past
// the end of the file): the audio then runs to the end of the file.
inline bool parseWav(File& f, WavFormat& w, String& err) {
  w = {};
  const uint32_t size = f.size();
  uint8_t h[16];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    err = "reply is not a WAV file";
    return false;
  }
  bool haveFmt = false;
  for (uint32_t pos = 12; pos + 8 <= size;) {
    f.seek(pos);
    if (f.read(h, 8) != 8) break;
    const uint32_t len = le32(h + 4);
    pos += 8;
    if (!memcmp(h, "fmt ", 4)) {
      if (len < 16 || f.read(h, 16) != 16) {
        err = "bad WAV fmt chunk";
        return false;
      }
      w.format = le16(h);
      w.channels = le16(h + 2);
      w.rate = le32(h + 4);
      w.bits = le16(h + 14);
      haveFmt = true;
    } else if (!memcmp(h, "data", 4)) {
      w.dataOffset = pos;
      w.dataBytes = len == 0 || len > size - pos ? size - pos : len;
      break;
    }
    if (len > size - pos) break;
    pos += len + (len & 1);
  }
  if (!haveFmt || !w.dataOffset) {
    err = "WAV has no fmt or data chunk";
    return false;
  }
  if (w.format != 1 || w.bits != 16 || w.channels < 1 || w.channels > 2) {
    err = "unsupported WAV: format " + String(w.format) + ", " + String(w.bits) + "-bit, " +
          String(w.channels) + " ch";
    return false;
  }
  if (w.rate < 8000 || w.rate > 96000) {
    err = "unsupported WAV rate " + String(w.rate);
    return false;
  }
  return true;
}

// Audio EQ cookbook biquad, transposed direct form II.
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1 = 0, z2 = 0;

  float operator()(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

inline Biquad highPass(uint32_t rate, float hz, float q) {
  const float w = 2 * float(M_PI) * hz / rate, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
  return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
}

inline Biquad lowPass(uint32_t rate, float hz, float q) {
  const float w = 2 * float(M_PI) * hz / rate, c = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
  return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
}

// Mono stream from inRate to audio::kRate. Downsampling low-passes at 7 kHz first (4th-order
// Butterworth), so the 8-12 kHz part of a 24 kHz clip can't fold back into the speech band as grit,
// then box-averages. Upsampling holds.
struct Resampler {
  uint32_t inRate;
  Biquad aa1 = lowPass(inRate, 7000, 0.5412f), aa2 = lowPass(inRate, 7000, 1.3066f);
  uint32_t phase = 0;  // in output-rate units: an output sample is due each time it reaches inRate
  float sum = 0;       // input samples since the last output, averaged into it
  int32_t count = 0;

  template <class Emit>
  void push(int16_t s, Emit emit) {
    const float x = inRate > audio::kRate ? aa2(aa1(s)) : s;
    sum += x;
    ++count;
    for (phase += audio::kRate; phase >= inRate; phase -= inRate) {
      emit(count ? sum / count : x);
      sum = 0;
      count = 0;
    }
  }
};

struct Limiter {
  float env = 0;  // recent peak of the boosted signal

  int16_t operator()(float s) {
    const float x = s * kBoost;
    env = std::max(std::fabs(x), env * kRelease);
    return static_cast<int16_t>(env > kCeiling ? x * (kCeiling / env) : x);
  }
};

// The speaker can't play below ~400 Hz; left in, that bass only eats headroom before the limiter.
struct Voicing {
  Biquad lowCut = highPass(audio::kRate, 400, 0.7071f);
  Limiter limit;

  int16_t operator()(float s) { return limit(lowCut(s)); }
};

enum class PlayEnd : uint8_t { Finished, Stopped, Failed };

struct Playback {
  PlayEnd end = PlayEnd::Finished;
  String error;
};

// Polls stop() once per I2S write (every few tens of ms of audio).
template <class Stop>
Playback playClip(const String& path, Stop stop) {
  File f = SD.open(path, FILE_READ);
  if (!f) return {PlayEnd::Failed, "no audio on SD"};
  WavFormat w;
  String err;
  if (!parseWav(f, w, err)) return {PlayEnd::Failed, err};
  Serial.printf("[tts] %s: %lu Hz, %u ch, %lu bytes\n", path.c_str(), w.rate, w.channels, w.dataBytes);
  f.seek(w.dataOffset);
  const size_t frameBytes = 2 * w.channels;
  // Input per write, sized so its resampled output always fits g_out.
  const size_t inFrames =
      std::min<size_t>(sizeof(g_io) / frameBytes, (kOutFrames - 1) * w.rate / audio::kRate);
  Resampler rs{w.rate};
  Voicing voicing;
  size_t out = 0, played = 0;
  const uint32_t t0 = millis();
  auto emit = [&out, &voicing](float s) {
    g_out[2 * out] = g_out[2 * out + 1] = voicing(s);
    ++out;
  };
  for (uint32_t left = w.dataBytes - w.dataBytes % frameBytes; left;) {
    if (stop()) return {PlayEnd::Stopped, ""};
    const size_t want = std::min<size_t>(left, inFrames * frameBytes);
    const size_t got = f.read(reinterpret_cast<uint8_t*>(g_io), want);
    const size_t frames = got / frameBytes;
    for (size_t i = 0; i < frames; ++i) {
      rs.push(w.channels == 2 ? (g_io[2 * i] + g_io[2 * i + 1]) / 2 : g_io[i], emit);
    }
    if (out && !audio::write(g_out, out * 4)) return {PlayEnd::Failed, "speaker write failed"};
    played += out;
    out = 0;
    if (got < want) break;
    left -= got;
  }
  // The last few DMA buffers are still queued when the final write returns.
  Serial.printf("[tts] played %u frames in %lu ms\n", played, millis() - t0);
  return {};
}

// Plays say0..say{clips-1} back to back. stop() is any key or G0; it cuts the sound at once.
template <class Stop>
Playback play(size_t clips, Stop stop) {
  audio::unmute();
  Playback p;
  for (size_t i = 0; i < clips && p.end == PlayEnd::Finished; ++i) p = playClip(clipPath(i), stop);
  if (p.end == PlayEnd::Stopped) audio::mute();
  audio::drain();
  audio::mute();
  audio::endPlayback();
  return p;
}
}  // namespace tts
