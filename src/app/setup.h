// Kloudputer — Running the phone setup page and applying what it saved.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

std::vector<phone::Choice> modelChoices() {
  std::vector<phone::Choice> out;
  for (const ModelOpt& m : kModels) out.push_back({m.id, modelName(m.id)});
  return out;
}

String currentSsid() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
  String ssid, pass;
  net::loadSaved(ssid, pass);
  return ssid.length() ? ssid : cfg.wifiSsid;
}

// During phone setup: L shows the log of what the phone asked for; Del, Tab or Esc cancels.
portal::Key setupKey() {
  const auto* st = nextKeys();
  if (!st) return portal::Key::None;
  if (typed(*st, 'l') || typed(*st, 'L')) return portal::Key::Log;
  if (st->backspace || st->del || st->tab || st->esc || typed(*st, '`')) return portal::Key::Cancel;
  return portal::Key::Other;
}

void reportKey(const char* name, int code) {
  view.add(String(name) + " key " + keycheck::verdict(code), keycheck::works(code) ? ui::GREEN : ui::ERR);
}

// Back from the phone. New settings apply without a reboot; a new Wi-Fi network must connect before
// it replaces the saved one; then the keys are tried against the APIs, which costs nothing.
void afterSetup(const portal::Outcome& o) {
  using portal::End;
  if (o.end == End::Saved) {
    view.add("Saved from your phone.", ui::GREEN);
    loadSettings();
  } else {
    view.add(o.end == End::TimedOut ? "Setup timed out after 10 minutes."
             : o.end == End::Failed ? "Couldn't start the setup Wi-Fi."
                                    : "Setup cancelled.",
             ui::SYS);
  }
  bool up = false;
  if (o.end == End::Saved && o.form.ssid.length()) {
    view.add("Joining " + o.form.ssid + "...", ui::SYS);
    up = net::connect(o.form.ssid, o.form.wifiPass);
    if (up) net::save(o.form.ssid, o.form.wifiPass);
    else view.add("Couldn't join " + o.form.ssid + " (wrong password?). Keeping the old network.", ui::ERR);
  }
  if (!up) up = connectKnown([](const String& ssid) { view.add("Joining " + ssid + "...", ui::SYS); });
  refreshHeader();
  if (!up) {
    view.add("Offline. /wifi picks a network; /setup runs setup again.", ui::ERR);
    return;
  }
  timeOk = net::syncTime(cfg.tz.c_str());
  if (o.end == End::Saved) {
    header.setNote("checking keys...", ui::SYS);
    if (cfg.anthropicKey.length()) reportKey("Anthropic", keycheck::anthropic(cfg.anthropicKey));
    if (gemini::isKey(cfg.groqKey)) reportKey("Gemini", keycheck::gemini(cfg.groqKey));
    else if (cfg.groqKey.length()) reportKey("Groq", keycheck::groq(cfg.groqKey));
  }
  refreshHeader();
}

// Phone setup takes the screen (its own Wi-Fi and page); the outcome is handled back in the chat.
Then setupFromPhone() {
  const RelayHold hold;
  phone::Current now = phone::currentFrom(currentSsid(), cfg.anthropicKey, cfg.groqKey, cfg.model, cfg.tz);
  for (const AgentEntry& a : loadAgents()) now.agents.push_back({a.name, a.relay});
  const portal::Outcome o = portal::run(M5Cardputer.Display, header, {now, modelChoices(), setupKey});
  return [o] { afterSetup(o); };
}
