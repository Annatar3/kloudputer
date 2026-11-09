// Kloudputer — Push-to-talk and the mic test.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

// "L 0 R 0 mix 0 dc 0 nz 0%": per-slot levels, DC level, and how much of the data line was ever non-zero.
String micSummary(const voice::Recording& rec) {
  const uint32_t pct = rec.samples ? rec.nonZero * 100 / rec.samples : 0;
  return "L " + String(rec.rmsL, 0) + " R " + String(rec.rmsR, 0) + " mix " + String(rec.rms, 0) + " dc " +
         String(rec.mean) + " nz " + String(pct) + "%";
}

// 3 s recording with no button involved, then levels + codec registers: tells a dead mic from a
// dead button, and a silent codec from one that's talking.
void micTest() {
  view.add("Mic test: talk for 3 seconds...", ui::SYS);
  const uint32_t t0 = millis();
  const voice::Recording rec = voice::record(
      [t0] { return millis() - t0 < 3000; },
      [](uint32_t ms) { header.setNote("MIC TEST " + String(ms / 1000.0, 1) + "s", ui::ERR); }, 30000, true);
  Serial.printf("[mic] ok=%d %ums nz=%u dc=%d rms=%.0f L=%.0f R=%.0f p2p=%d bck=%d%%/%d ws=%d%%/%d din=%d%%/%d "
                "zeros-test=%d%% regs=%s err=%s\n",
                rec.ok, rec.ms, rec.nonZero, rec.mean, rec.rms, rec.rmsL, rec.rmsR, rec.peakToPeak, rec.bck.highPct,
                rec.bck.toggles, rec.ws.highPct, rec.ws.toggles, rec.din.highPct, rec.din.toggles, rec.lineTestNz,
                rec.codecRegs.c_str(), rec.error.c_str());
  refreshHeader();
  if (!rec.ok || rec.samples == 0) {
    view.add("Mic: " + (rec.ok ? String("I2S delivered no data") : rec.error), ui::ERR);
    return;
  }
  view.add(String(rec.ms / 1000.0, 1) + "s  " + micSummary(rec) + " p2p " + String(rec.peakToPeak),
           rec.rms >= kSilentRms ? ui::GREEN : ui::ERR);
  // Pin activity: BCK/WS should toggle a lot at ~50% high; a stuck pin means no clock at the codec.
  view.add("hi% bck " + String(rec.bck.highPct) + " ws " + String(rec.ws.highPct) + " din " +
               String(rec.din.highPct) + "  zeros-test nz " + String(rec.lineTestNz) + "%",
           ui::DIM);
  view.add("toggles bck " + String(rec.bck.toggles) + " ws " + String(rec.ws.toggles) + " din " +
               String(rec.din.toggles),
           ui::DIM);
  view.add("codec FD FE 00 01 02 09 0A 0D 0E 14 17:", ui::DIM);
  view.add(rec.codecRegs, ui::DIM);
  view.add(rec.rms >= kSilentRms ? "Mic works - G0 voice should too." : "Mic silent. Send me these numbers.",
           rec.rms >= kSilentRms ? ui::GREEN : ui::SYS);
}

// Hold G0 to talk into `v`'s input line, `buf`; the transcript can be fixed there before Enter.
void doVoice(ui::ChatView& v, String& buf) {
  const RelayHold hold;
  if (!haveVoiceKey()) {
    v.add("Voice needs a Groq or Gemini key. Type /setup to add one.", ui::ERR);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    v.add("Offline. Type /wifi to connect.", ui::ERR);
    return;
  }
  v.setInput("", "(recording - release G0) ");
  pokeScreen();
  const voice::Recording rec = voice::record(
      [] {
        M5Cardputer.update();
        return M5Cardputer.BtnA.isPressed();
      },
      [](uint32_t ms) { header.setNote("REC " + String(ms / 1000.0, 1) + "s", ui::ERR); });
  Serial.printf("[voice] rec ok=%d %ums samples=%u rms=%.0f L=%.0f R=%.0f p2p=%d err=%s\n", rec.ok, rec.ms,
                rec.samples, rec.rms, rec.rmsL, rec.rmsR, rec.peakToPeak, rec.error.c_str());
  if (!rec.ok || rec.ms < 400) {
    v.add(rec.ok ? "Too short - hold G0 while you talk." : "Voice: " + rec.error, rec.ok ? ui::SYS : ui::ERR);
    refreshHeader();
    v.setInput(buf);
    return;
  }
  // Whisper invents text ("Thank you.") for silence, so never send it a silent clip.
  if (rec.rms < kSilentRms) {
    v.add("Didn't hear anything: " + micSummary(rec), ui::SYS);
    v.add("Try /mic for a full mic test.", ui::DIM);
    refreshHeader();
    v.setInput(buf);
    return;
  }
  header.setNote("level " + String(rec.rms, 0) + " - transcribing...", ui::SYS);
  v.setInput("", "... ");
  const voicepick::Plan plan = voicePlan();
  voice::Transcript t = voice::transcribe(voiceKey(plan.first), lang::kLanguages[dictation].code);
  Serial.printf("[voice] stt %s ok=%d http=%d %ums text=%s err=%s heap=%u\n", voicepick::name(plan.first), t.ok, t.status,
                t.ms, t.text.c_str(), t.error.c_str(), ESP.getFreeHeap());
  if (!t.ok && plan.hasSecond && voicepick::worthFallback(t.status)) {
    voiceRefused(plan.first, t.status);
    header.setNote(String(voicepick::name(plan.second)) + " instead...", ui::SYS);
    const String firstError = t.error;
    t = voice::transcribe(voiceKey(plan.second), lang::kLanguages[dictation].code);
    Serial.printf("[voice] stt %s ok=%d http=%d %ums err=%s\n", voicepick::name(plan.second), t.ok, t.status, t.ms,
                  t.error.c_str());
    if (!t.ok) t.error = firstError + "; then " + t.error;
  }
  lastHeaderMs = millis();
  if (!t.ok) {
    v.add("Voice: " + t.error, ui::ERR);
    refreshHeader();
    v.setInput(buf);
    return;
  }
  if (t.text.isEmpty()) {
    v.add("Didn't catch that - try again.", ui::SYS);
    refreshHeader();
    v.setInput(buf);
    return;
  }
  if (buf.length()) buf += ' ';
  buf += t.text;
  v.setInput(buf);
  pokeScreen();
  header.setNote("heard " + String(rec.ms / 1000.0, 1) + "s L" + String(rec.rmsL, 0) + " R" +
                     String(rec.rmsR, 0) + " - Enter",
                 ui::GREEN);
}
