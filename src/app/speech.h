// Kloudputer — Reading a reply aloud.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

void voiceError(const String& reason, ui::ChatView& v = view) { v.add("Voice reply: " + reason.substring(0, 110), ui::ERR); }

// Any key or G0. Reading the key press here consumes it, so it never lands in the input line.
bool stopRequested() {
  M5Cardputer.update();
  M5Cardputer.Keyboard.updateKeyList();
  const bool key = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
  return key || M5Cardputer.BtnA.isPressed();
}

// Makes the spoken clips for `text` with one provider. Returns how many are ready to play; `failed` holds the
// reply of the request that stopped it, if one did. -1 means a key press stopped it.
int synthesizeWith(voicepick::Provider who, const String& text, tts::Synth& failed) {
  const bool wide = who == voicepick::Provider::Gemini;  // no 200-character cap: one request for the whole reply
  const std::vector<String> parts = wide ? tts::chunks(text, gemini::kMaxSpeakChars, 1) : tts::chunks(text);
  int ready = 0;
  while (ready < static_cast<int>(parts.size())) {
    header.setNote("voice " + String(ready + 1) + "/" + String(parts.size()) + "...", ui::SYS);
    const tts::Synth s = tts::synthesize(voiceKey(who), voiceName, parts[ready], tts::clipPath(ready));
    Serial.printf("[tts] %s %d/%u ok=%d http=%d %luB %lums heap=%lu err=%s\n", voicepick::name(who), ready + 1,
                  parts.size(), s.ok, s.status, s.bytes, s.ms, ESP.getFreeHeap(), s.error.c_str());
    if (!s.ok) {
      failed = s;
      break;
    }
    ++ready;
    if (stopRequested()) return -1;
  }
  return ready;
}

// Synthesizes every chunk before playing any, so the answer plays without gaps mid-way. When the first provider
// answers with an error (say, a rate limit) and the other has a key, the whole reply is made again with that one.
// With no other provider, the chunks already made still play.
void speak(const String& text, ui::ChatView& v = view) {
  const RelayHold hold;
  if (!haveVoiceKey() || WiFi.status() != WL_CONNECTED) {
    voiceError(!haveVoiceKey() ? "needs a voice key (/setup)" : "offline", v);
    return;
  }
  const voicepick::Plan plan = voicePlan();
  tts::Synth failed;
  int ready = synthesizeWith(plan.first, text, failed);
  if (ready >= 0 && !failed.ok && failed.error.length() && plan.hasSecond && voicepick::worthFallback(failed.status)) {
    voiceRefused(plan.first, failed.status);
    const String firstError = failed.error;
    failed = tts::Synth();
    header.setNote(String(voicepick::name(plan.second)) + " instead...", ui::SYS);
    ready = synthesizeWith(plan.second, text, failed);
    if (failed.error.length()) failed.error = firstError + "; then " + failed.error;
  }
  if (ready < 0) return;
  if (failed.error.length()) voiceError(failed.error, v);
  if (!ready) return;
  header.setNote("SPEAKING - any key stops", ui::GREEN);
  const tts::Playback p = tts::play(ready, stopRequested);
  Serial.printf("[tts] play end=%d err=%s\n", static_cast<int>(p.end), p.error.c_str());
  if (p.end == tts::PlayEnd::Failed) voiceError(p.error, v);
}
