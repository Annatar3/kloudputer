// Kloudputer — The slash commands' own screens: help, voice, volume, info.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

void showHelp() {
  view.add("Type or hold G0 to talk. Enter sends.", ui::DIM);
  view.add("Tap G0 twice (or /talk): talk mode", ui::DIM);
  view.add("Tab    menu: sessions, voice, model", ui::DIM);
  view.add("Fn+; Fn+.  earlier prompts", ui::DIM);
  view.add("Fn+, Fn+/  scroll   Fn+`  clear", ui::DIM);
  view.add("Menus: ; . move  , / change  Del back", ui::DIM);
  view.add("/new    start a fresh conversation", ui::DIM);
  view.add("/sessions  saved chats (x deletes)", ui::DIM);
  view.add("/setup  Wi-Fi + keys from a phone", ui::DIM);
  view.add("/pair NAME CODE  pair an agent", ui::DIM);
  view.add("/remember X  or say: remember that X", ui::DIM);
  view.add("/notes  list    /forget N|all", ui::DIM);
  view.add("/model  Haiku, Sonnet, Opus or Fable", ui::DIM);
  view.add("/wifi   pick a different network", ui::DIM);
  view.add("/info   device + connection details", ui::DIM);
  view.add("/voice  spoken answers on / off", ui::DIM);
  view.add("/voice N  autumn diana hannah", ui::DIM);
  view.add("          austin daniel troy", ui::DIM);
  view.add("/vol N  speaker volume 1-10", ui::DIM);
  view.add("/say T  speak text T (voice test)", ui::DIM);
  view.add("Any key or G0 stops speech.", ui::DIM);
  view.add("/mic    3 s mic test (no button)", ui::DIM);
  view.add("/codec  audio chip registers", ui::DIM);
}

void newChat() {
  history.clear();
  spent.chat = 0;
  session = sessions::Meta();
  view.clear();
  view.add("New conversation.", ui::GREEN);
}

// /voice toggles spoken answers; /voice <name> picks a voice, turns them on and plays a sample.
void voiceCommand(const String& arg) {
  const char* pick = tts::findVoice(arg);
  if (arg.length() && arg != "on" && arg != "off" && !pick) {
    view.add("Usage: /voice [on|off|name]", ui::ERR);
    view.add("Voices: autumn diana hannah austin daniel troy", ui::DIM);
    return;
  }
  const bool on = pick || (arg.isEmpty() ? !voiceReplies : arg == "on");
  if (on && !haveVoiceKey()) {
    view.add("Voice needs a Groq or Gemini key. Type /setup to add one.", ui::ERR);
    return;
  }
  voiceReplies = on;
  if (pick) voiceName = pick;
  Preferences p;
  p.begin("cputer", false);
  p.putBool("voice", on);
  p.putString("vname", voiceName);
  p.end();
  view.add(String("Spoken answers: ") + (on ? "on" : "off") + ", voice " + voiceName + " (saved)", ui::SYS);
  if (pick) {
    String name = pick;
    name.setCharAt(0, toupper(name[0]));
    speak("Hi, I'm " + name + ". This is how I sound on your Cardputer.");
  }
  refreshHeader();
}

void applyVolume(int v) {
  volume = v;
  adv_audio::setVolume(v);
  Preferences p;
  p.begin("cputer", false);
  p.putUChar("vol", v);
  p.end();
}

void volCommand(const String& arg) {
  if (arg.isEmpty()) {
    view.add("Volume " + String(volume) + "/10. /vol 1-10 to change.", ui::SYS);
    return;
  }
  const int v = arg.toInt();
  if (v < 1 || v > 10) {
    view.add("Volume is 1-10", ui::ERR);
    return;
  }
  applyVolume(v);
  view.add("Volume " + String(v) + "/10 (saved)", ui::SYS);
}

void sayCommand(const String& text) {
  if (text.isEmpty() || text.length() > 200) {
    view.add("Usage: /say <text, up to 200 chars>", ui::ERR);
    return;
  }
  speak(text);
  refreshHeader();
}

// A crash or watchdog shows up here after the device comes back.
String resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "low power (brownout)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other (" + String(static_cast<int>(esp_reset_reason())) + ")";
  }
}

void showInfo() {
  view.add(String(brand::kProduct) + " " + kVersion + ", " + brand::kTagline + ".", ui::DIM);
  view.add(brand::kDisclaimer, ui::DIM);
  view.add("board " + String(boardId) +
               (boardId == static_cast<int>(m5::board_t::board_M5CardputerADV) ? " (ADV)" : " (not ADV)") +
               "  kbd " + (kb && kb->ok() ? "ok" : "FAIL") + "  codec " + (adv_audio::codecPresent() ? "ok" : "MISSING"),
           ui::DIM);
  view.add("spent ~" + usage::money(spent.chat) + " chat  " + usage::money(usage::spentToday(spent, todayStamp())) + " today  " +
               usage::money(spent.total) + " total",
           ui::DIM);
  view.add("heap " + String(ESP.getFreeHeap()) + "  max block " + String(ESP.getMaxAllocHeap()), ui::DIM);
  view.add("last restart: " + resetReason(), ui::DIM);
  view.add("loop stack: " + String(uxTaskGetStackHighWaterMark(nullptr)) + " of " + String(kLoopStack) +
               " bytes never used", ui::DIM);
  const int battery = M5.Power.getBatteryLevel();
  view.add(battery < 0 ? "battery: no reading on this board (" + String(battery) + ")"
                       : "battery " + String(battery) + "%  " + String(M5.Power.getBatteryVoltage()) + " mV",
           ui::DIM);
  view.add("sd " + String(sdOk ? "ok" : "missing") + "  cfg " + (cfg.source.length() ? cfg.source : "none"),
           ui::DIM);
  view.add("wifi " +
               (WiFi.status() == WL_CONNECTED ? WiFi.SSID() + " " + WiFi.localIP().toString() : String("offline")) +
               "  clock " + (timeOk ? "ok" : "unsynced"),
           ui::DIM);
  view.add("model " + cfg.model + "  tz " + cfg.tz, ui::DIM);
  view.add("claude key " + (cfg.anthropicKey.length() ? cfg.anthropicFrom : String("MISSING")) + "  voice " +
               (!haveVoiceKey() ? String("-")
                                : (cfg.groqKey.length() ? "Groq (" + cfg.groqFrom + ") " : String()) +
                                      (cfg.geminiKey.length() ? "Gemini (" + cfg.geminiFrom + ") " : String()) +
                                      voicepick::name(voicePlan().first) + " first"),
           ui::DIM);
  view.add(String("voice ") + (voiceReplies ? "on" : "off") + " " + voiceName + "  vol " + String(volume) + "/10",
           ui::DIM);
  if (talkRoom >= 0) {
    view.add("talk mode heard: room " + String(talkRoom, 0) + ", voice " + (talkPeak >= 0 ? String(talkPeak, 0) : String("-")) +
                 " (starts at " + String(static_cast<int>(vad::Tuning().minLevel)) + " or 3x the room)",
             ui::DIM);
  }
  view.add("agents " + String(static_cast<int>(loadAgents().size())) + " paired  crypto " + (cryptoOk ? "ok" : "FAIL"),
           cryptoOk ? ui::DIM : ui::ERR);
}
