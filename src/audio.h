// Kloudputer — the microphone and speaker, whichever Cardputer this is. The ADV has a codec chip that records
// and plays at once and never stops (audio_adv.h). The original has a microphone and an amplifier that share a
// pin and take turns (audio_classic.h). The rest of the firmware talks to this and doesn't care which.
#pragma once
#include "audio_adv.h"
#include "audio_classic.h"

namespace audio {
constexpr uint32_t kRate = 16000;
static_assert(adv_audio::kRate == kRate && classic_audio::kRate == kRate, "both boards record and play at 16 kHz");

inline bool g_classic = false;  // set once at boot, from what the hardware answers to

inline bool isAdv() { return !g_classic; }

// Capture. Frames are interleaved L/R 16-bit at kRate.
inline bool start(String& err) { return g_classic ? classic_audio::start(err) : adv_audio::start(err); }
inline size_t read(int16_t* buf, size_t bytes, uint32_t timeoutMs) {
  return g_classic ? classic_audio::read(buf, bytes, timeoutMs) : adv_audio::read(buf, bytes, timeoutMs);
}
// Drops what was captured before now. The original's microphone only starts when asked, so it has nothing old.
inline void flush(int16_t* buf, size_t bytes) {
  if (!g_classic) adv_audio::flush(buf, bytes);
}
// Call when a recording is over. The ADV keeps listening; the original has to let go of the shared pin.
inline void endCapture() {
  if (g_classic) classic_audio::stopCapture();
}

// Playback: unmute, write frames, drain, mute, endPlayback.
inline void unmute() {
  if (!g_classic) adv_audio::unmute();
}
inline bool write(const int16_t* frames, size_t bytes) {
  return g_classic ? classic_audio::write(frames, bytes) : adv_audio::write(frames, bytes);
}
inline void drain() { g_classic ? classic_audio::drain() : adv_audio::drain(); }
inline void mute() { g_classic ? classic_audio::mute() : adv_audio::mute(); }
inline void endPlayback() {
  if (g_classic) classic_audio::release();
}
inline void setVolume(int level) { g_classic ? classic_audio::setVolume(level) : adv_audio::setVolume(level); }
}  // namespace audio
