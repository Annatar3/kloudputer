// Kloudputer — microphone and speaker on the original Cardputer (not the ADV): a PDM microphone and a plain I2S
// amplifier, with no codec chip. They share GPIO43 (the microphone's clock is the amplifier's word select), so
// only one runs at a time. This goes through M5Unified's own Mic and Speaker classes with the pins and settings
// M5Unified uses for this board, because that path is the vendor's and this file was written without the
// hardware to try it on.
#pragma once
#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

namespace classic_audio {
constexpr uint32_t kRate = 16000;
constexpr size_t kChunk = 1600;        // 100 ms of mono samples per capture buffer
constexpr size_t kPlaySamples = 1024;  // one speaker buffer: 512 stereo frames

// Two capture buffers take turns so nothing is lost between them; three playback buffers rotate because the
// speaker keeps pointing at one until it has played. All of them exist only while they are in use: this memory
// has to be free again for the next TLS session.
inline std::unique_ptr<int16_t[]> g_rec[2];
inline std::unique_ptr<int16_t[]> g_play[3];
inline int g_oldest = 0, g_submitted = 0, g_playAt = 0;
inline bool g_micOn = false, g_spkOn = false;
inline uint8_t g_volume = 255;

// Once at boot: the pins M5Unified gives this board when it is allowed to set them up itself.
inline void configure() {
  auto mic = M5.Mic.config();
  mic.pin_data_in = GPIO_NUM_46;
  mic.pin_ws = GPIO_NUM_43;  // the PDM clock
  mic.sample_rate = kRate;
  M5.Mic.config(mic);
  auto spk = M5.Speaker.config();
  spk.pin_bck = GPIO_NUM_41;
  spk.pin_ws = GPIO_NUM_43;
  spk.pin_data_out = GPIO_NUM_42;
  spk.magnification = 16;
  spk.i2s_port = I2S_NUM_1;
  M5.Speaker.config(spk);
}

inline void stopCapture() {
  if (g_micOn) M5.Mic.end();  // returns once the recorder has let go of the buffers
  g_micOn = false;
  g_submitted = 0;
  for (auto& b : g_rec) b.reset();
}

// Ends playback and gives the speaker's memory back.
inline void release() {
  if (g_spkOn) {
    M5.Speaker.stop();
    M5.Speaker.end();
  }
  g_spkOn = false;
  for (auto& b : g_play) b.reset();
}

inline bool start(String& err) {
  if (g_micOn) return true;
  release();
  for (auto& b : g_rec) b.reset(new (std::nothrow) int16_t[kChunk]);
  if (!g_rec[0] || !g_rec[1]) {
    stopCapture();
    err = "not enough memory for the microphone";
    return false;
  }
  if (!M5.Mic.begin()) {
    stopCapture();
    err = "the microphone did not start";
    return false;
  }
  g_micOn = true;
  g_oldest = 0;
  g_submitted = 0;
  return true;
}

// The next 100 ms, as interleaved L/R frames (the one microphone on both sides) like the ADV driver gives.
// Returns bytes written, 0 if the microphone gave nothing in time.
inline size_t read(int16_t* stereo, size_t bytes, uint32_t timeoutMs) {
  if (!g_micOn) return 0;
  while (g_submitted < 2) {
    if (!M5.Mic.record(g_rec[(g_oldest + g_submitted) % 2].get(), kChunk, kRate)) return 0;
    ++g_submitted;
  }
  const uint32_t t0 = millis();
  while (M5.Mic.isRecording() >= static_cast<size_t>(g_submitted)) {  // the oldest buffer is still filling
    if (millis() - t0 > timeoutMs + 200) return 0;
    delay(1);
  }
  const size_t frames = std::min(bytes / 4, kChunk);
  const int16_t* mono = g_rec[g_oldest].get();
  for (size_t i = 0; i < frames; ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
  g_oldest ^= 1;
  --g_submitted;
  return frames * 4;
}

inline bool beginPlayback() {
  if (g_spkOn) return true;
  stopCapture();
  for (auto& b : g_play) b.reset(new (std::nothrow) int16_t[kPlaySamples]);
  if (!g_play[0] || !g_play[1] || !g_play[2]) {
    release();
    return false;
  }
  M5.Speaker.setVolume(g_volume);
  g_spkOn = M5.Speaker.begin();
  if (!g_spkOn) release();
  g_playAt = 0;
  return g_spkOn;
}

// Queues interleaved L/R frames. Blocks while the speaker's queue is full.
inline bool write(const int16_t* frames, size_t bytes) {
  if (!beginPlayback()) return false;
  for (size_t samples = bytes / 2; samples;) {
    const size_t n = std::min(samples, kPlaySamples);
    const uint32_t t0 = millis();
    while (M5.Speaker.isPlaying(0) >= 2) {  // one playing and one waiting: the third buffer is the free one
      if (millis() - t0 > 2000) return false;
      delay(1);
    }
    int16_t* buf = g_play[g_playAt].get();
    g_playAt = (g_playAt + 1) % 3;
    memcpy(buf, frames, n * 2);
    if (!M5.Speaker.playRaw(buf, n, kRate, true, 1, 0, false)) return false;
    frames += n;
    samples -= n;
  }
  return true;
}

inline void drain() {
  const uint32_t t0 = millis();
  while (g_spkOn && M5.Speaker.isPlaying() && millis() - t0 < 5000) delay(2);
}

inline void mute() {
  if (g_spkOn) M5.Speaker.stop();
}

// Level 1-10, 4 dB a step like the ADV. This speaker's loudness goes with the square of its volume setting.
inline void setVolume(int level) {
  level = std::min(std::max(level, 1), 10);
  g_volume = static_cast<uint8_t>(lroundf(255.0f * powf(10.0f, -0.1f * (10 - level))));
  if (g_spkOn) M5.Speaker.setVolume(g_volume);
}
}  // namespace classic_audio
