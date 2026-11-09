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

// Synthesizes every chunk before playing any, so the answer plays without gaps mid-way. If a later
// chunk fails (say, a rate limit), the ones already made still play.
void speak(const String& text, ui::ChatView& v = view) {
  const RelayHold hold;
  if (cfg.groqKey.isEmpty() || WiFi.status() != WL_CONNECTED) {
    voiceError(cfg.groqKey.isEmpty() ? "needs a voice key (/setup)" : "offline", v);
    return;
  }
  const std::vector<String> parts =
      gemini::isKey(cfg.groqKey) ? tts::chunks(text, gemini::kMaxSpeakChars, 1) : tts::chunks(text);
  size_t ready = 0;
  while (ready < parts.size()) {
    header.setNote("voice " + String(ready + 1) + "/" + String(parts.size()) + "...", ui::SYS);
    const tts::Synth s = tts::synthesize(cfg.groqKey, voiceName, parts[ready], tts::clipPath(ready));
    Serial.printf("[tts] %u/%u ok=%d http=%d %luB %lums heap=%lu err=%s\n", ready + 1, parts.size(), s.ok,
                  s.status, s.bytes, s.ms, ESP.getFreeHeap(), s.error.c_str());
    if (!s.ok) {
      voiceError(s.error, v);
      break;
    }
    ++ready;
    if (stopRequested()) return;
  }
  if (!ready) return;
  header.setNote("SPEAKING - any key stops", ui::GREEN);
  const tts::Playback p = tts::play(ready, stopRequested);
  Serial.printf("[tts] play end=%d err=%s\n", static_cast<int>(p.end), p.error.c_str());
  if (p.end == tts::PlayEnd::Failed) voiceError(p.error, v);
}
