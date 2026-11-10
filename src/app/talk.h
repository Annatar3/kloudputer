// Kloudputer — Talk mode: a hands-free conversation. Listen, transcribe, ask Claude, speak the answer, listen again.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

// Lets go of whatever started or interrupted a turn, so a held key isn't read as "stop" by the next one.
void waitForRelease() {
  const uint32_t t0 = millis();
  do {
    delay(10);
    M5Cardputer.update();
    M5Cardputer.Keyboard.updateKeyList();
  } while ((M5Cardputer.BtnA.isPressed() || M5Cardputer.Keyboard.isPressed()) && millis() - t0 < 1500);
  M5Cardputer.Keyboard.updateKeysState();
}

// Started by two taps on G0, or /talk. A key while Claude is speaking skips the rest of that answer; a key while
// it is listening, the word "stop", or 45 s without speech ends talk mode.
void talkMode() {
  const RelayHold hold;  // an agent's messages wait until talk mode is over
  if (!haveVoiceKey() || cfg.anthropicKey.isEmpty()) {
    view.add("Talk mode needs a Claude key and a voice key. Type /setup to add them.", ui::ERR);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    view.add("Offline. Type /wifi to connect.", ui::ERR);
    return;
  }
  talking = true;
  view.add("Talk mode: just speak. Any key, or saying \"stop\", ends it.", ui::GREEN);
  beep();
  waitForRelease();
  String why = "Talk mode off.";
  while (true) {
    view.setInput("", "(listening) ");
    header.setNote("LISTENING", ui::GREEN);
    pokeScreen();
    const voice::Listening heard = voice::listen(stopRequested, [](bool speaking) {
      header.setNote(speaking ? "HEARING YOU" : "LISTENING", speaking ? ui::ERR : ui::GREEN);
    });
    Serial.printf("[talk] heard=%d %ums peak=%.0f room=%.0f err=%s heap=%u\n", static_cast<int>(heard.what), heard.ms,
                  heard.peak, heard.background, heard.error.c_str(), ESP.getFreeHeap());
    talkRoom = heard.background;
    if (heard.what == voice::Heard::Speech) talkPeak = heard.peak;
    if (heard.what == voice::Heard::Stopped) break;
    if (heard.what == voice::Heard::Failed) {
      why = "Talk mode off: " + heard.error;
      break;
    }
    if (heard.what == voice::Heard::Quiet) {
      why = "Talk mode off: nothing heard for a while.";
      break;
    }
    header.setNote("transcribing...", ui::SYS);
    view.setInput("", "... ");
    const voice::Transcript t = transcribeRecording();
    if (!t.ok) {  // stop rather than keep spending requests on something that is failing
      why = "Talk mode off. Voice: " + t.error;
      break;
    }
    if (t.text.isEmpty()) continue;  // a noise, not words
    if (talk::saysStop(t.text)) break;
    sendPrompt(t.text);  // shows it, asks Claude and, in talk mode, always speaks the answer
    waitForRelease();
  }
  talking = false;
  view.add(why, why == "Talk mode off." ? ui::DIM : ui::SYS);
  waitForRelease();
  refreshHeader();
  lastHeaderMs = millis();
  view.setInput(input);
}
