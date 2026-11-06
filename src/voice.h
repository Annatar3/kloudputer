// Kloudputer — push-to-talk voice: record 16 kHz mono WAV to the SD card, transcribe with Groq Whisper.
//
// Capture runs on our own always-on I2S driver (audio_adv.h); its DMA ring keeps filling while we
// write each 100 ms piece to SD, so audio never accumulates in RAM and can't fight TLS for heap.
#pragma once
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include "audio_adv.h"
#include "brand.h"
#include "certs.h"

namespace voice {
constexpr uint32_t kRate = 16000;
constexpr size_t kChunk = 1600;  // 100 ms per buffer
constexpr const char* kPath = DATA_DIR "/rec.wav";

inline int16_t g_stereo[kChunk * 2];  // one 100 ms read: interleaved L/R
inline int16_t g_mono[kChunk];

inline void writeWavHeader(File& f, uint32_t samples) {
  const uint32_t dataBytes = samples * 2;
  uint8_t h[44];
  auto put32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  auto put16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);         // fmt chunk size
  put16(20, 1);          // PCM
  put16(22, 1);          // mono
  put32(24, kRate);
  put32(28, kRate * 2);  // byte rate
  put16(32, 2);          // block align
  put16(34, 16);         // bits per sample
  memcpy(h + 36, "data", 4);
  put32(40, dataBytes);
  f.seek(0);
  f.write(h, sizeof(h));
}

struct Recording {
  bool ok = false;
  uint32_t ms = 0;
  uint32_t samples = 0;
  float rms = 0;        // of the mono mix, around the mean so a DC offset doesn't count as sound
  float rmsL = 0;       // per I2S slot: tells us which side the codec actually talks on
  float rmsR = 0;
  int peakToPeak = 0;
  int mean = 0;          // DC level of the mix: a constant non-zero value means "data, but no signal"
  uint32_t nonZero = 0;  // frames with any non-zero bit: 0 means the codec never drives the data line
  String codecRegs;      // ES8311 chip ID + key registers right after setup
  adv_audio::PinActivity bck, ws, din;  // diagnose only
  int lineTestNz = -1;                  // diagnose only: % non-zero while the codec is told to send zeros
  String error;
};

// Streaming mean/variance for one signal.
struct LevelMeter {
  int64_t sum = 0;
  uint64_t sumSq = 0;
  uint32_t n = 0;
  void add(int v) {
    sum += v;
    sumSq += static_cast<uint64_t>(v * v);
    ++n;
  }
  float rms() const {
    if (n == 0) return 0;
    const double mean = static_cast<double>(sum) / n;
    const double var = static_cast<double>(sumSq) / n - mean * mean;
    return var > 0 ? sqrt(var) : 0;
  }
};

// Records while keepGoing() is true (capped at maxMs); onTick(elapsedMs) lets the UI show a timer.
// diagnose adds ~0.5 s of pin and codec-output checks before recording (for /mic).
template <class KeepGoing, class OnTick>
Recording record(KeepGoing keepGoing, OnTick onTick, uint32_t maxMs = 30000, bool diagnose = false) {
  Recording r;
  SD.mkdir(DATA_DIR);
  File f = SD.open(kPath, FILE_WRITE);
  if (!f) {
    r.error = "can't write to SD";
    return r;
  }
  const uint8_t placeholder[44] = {0};
  f.write(placeholder, sizeof(placeholder));

  if (!adv_audio::start(r.error)) {
    f.close();
    return r;
  }
  r.codecRegs = adv_audio::codecDump();
  if (diagnose) {
    r.bck = adv_audio::probePin(adv_audio::kBck);
    r.ws = adv_audio::probePin(adv_audio::kWs);
    r.din = adv_audio::probePin(adv_audio::kDin);
    r.lineTestNz = adv_audio::lineTestNonZeroPct(g_stereo, sizeof(g_stereo));
  }
  adv_audio::flush(g_stereo, sizeof(g_stereo));

  LevelMeter mono, left, right;
  int minV = 32767, maxV = -32768;
  const uint32_t t0 = millis();
  uint32_t lastTick = 0;
  while (keepGoing() && millis() - t0 < maxMs) {
    const size_t got = adv_audio::read(g_stereo, sizeof(g_stereo), 300) / 4;  // frames
    for (size_t i = 0; i < got; ++i) {
      const int l = g_stereo[2 * i];
      const int rr = g_stereo[2 * i + 1];
      const int m = (l + rr) / 2;
      g_mono[i] = static_cast<int16_t>(m);
      left.add(l);
      right.add(rr);
      mono.add(m);
      if (l != 0 || rr != 0) ++r.nonZero;
      if (m < minV) minV = m;
      if (m > maxV) maxV = m;
    }
    f.write(reinterpret_cast<const uint8_t*>(g_mono), got * 2);
    r.samples += got;
    if (millis() - lastTick > 200) {
      lastTick = millis();
      onTick(millis() - t0);
    }
  }
  writeWavHeader(f, r.samples);
  f.close();

  r.ms = r.samples * 1000 / kRate;
  r.ok = true;  // a quick tap can end before the first 100 ms piece: the caller sees ms == 0
  r.rms = mono.rms();
  r.rmsL = left.rms();
  r.rmsR = right.rms();
  r.peakToPeak = maxV - minV;
  r.mean = mono.n ? static_cast<int>(mono.sum / static_cast<int64_t>(mono.n)) : 0;
  return r;
}

struct Transcript {
  bool ok = false;
  String text;
  String error;
  uint32_t ms = 0;
};

// Uploads the recording as multipart/form-data, streaming the WAV from SD in 2 KB pieces.
inline Transcript transcribe(const String& groqKey) {
  Transcript t;
  const uint32_t t0 = millis();
  File f = SD.open(kPath, FILE_READ);
  if (!f) {
    t.error = "no recording on SD";
    return t;
  }
  const size_t fileSize = f.size();
  const String b = "kloudputer8f3a2c";
  const String pre = "--" + b + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-large-v3-turbo\r\n" +
                     "--" + b + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n" +
                     "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\n" +
                     "Content-Type: audio/wav\r\n\r\n";
  const String post = "\r\n--" + b + "--\r\n";

  WiFiClientSecure c;
  c.setCACert(kRootCAs);
  c.setTimeout(30000);  // ms in Arduino 3.x
  if (!c.connect("api.groq.com", 443)) {
    f.close();
    t.error = "can't reach api.groq.com";
    return t;
  }
  // HTTP/1.0 so the reply isn't chunked and can be parsed straight off the socket.
  c.print("POST /openai/v1/audio/transcriptions HTTP/1.0\r\nHost: api.groq.com\r\nAuthorization: Bearer " +
          groqKey + "\r\nContent-Type: multipart/form-data; boundary=" + b +
          "\r\nContent-Length: " + String(pre.length() + fileSize + post.length()) +
          "\r\nConnection: close\r\n\r\n");
  c.print(pre);
  uint8_t buf[2048];
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (c.write(buf, n) != n) {
      f.close();
      c.stop();
      t.error = "upload interrupted";
      return t;
    }
  }
  f.close();
  c.print(post);

  const String status = c.readStringUntil('\n');  // "HTTP/1.1 200 OK"
  const int code = status.length() >= 12 ? status.substring(9, 12).toInt() : 0;
  while (c.connected() || c.available()) {  // skip headers
    const String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;
  }
  JsonDocument filter;
  filter["text"] = true;
  filter["error"]["message"] = true;
  JsonDocument doc;
  const DeserializationError de = deserializeJson(doc, c, DeserializationOption::Filter(filter));
  c.stop();
  t.ms = millis() - t0;

  if (code != 200) {
    t.error = "Groq HTTP " + String(code) + ": " + String(doc["error"]["message"] | "no details");
    return t;
  }
  if (de) {
    t.error = String("bad Groq reply: ") + de.c_str();
    return t;
  }
  t.text = String(doc["text"] | "");
  t.text.trim();
  t.ok = true;
  return t;
}
}  // namespace voice
