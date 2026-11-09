// Kloudputer — Joining a network: the scan list, the saved network, and switching to a new one.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

bool wifiSetup() {
  view.add("Scanning for WiFi...", ui::SYS);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  const int n = WiFi.scanNetworks();
  if (n <= 0) {
    view.add("No networks found. Type /wifi to retry.", ui::ERR);
    return false;
  }
  const int shown = std::min(n, 9);
  for (int i = 0; i < shown; ++i) {
    const bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    view.add(String(i + 1) + ") " + WiFi.SSID(i) + "  " + WiFi.RSSI(i) + "dBm" + (open ? " open" : ""),
             ui::DIM);
  }
  const String pick = readLine("network # > ");
  const int idx = pick.toInt() - 1;
  if (idx < 0 || idx >= shown) {
    WiFi.scanDelete();
    view.add("Cancelled. Type /wifi to try again.", ui::SYS);
    return false;
  }
  const String ssid = WiFi.SSID(idx);
  const bool open = WiFi.encryptionType(idx) == WIFI_AUTH_OPEN;
  WiFi.scanDelete();
  const String pass = open ? String() : readLine("password > ", true);
  view.add("Connecting to " + ssid + "...", ui::SYS);
  if (!net::connect(ssid, pass)) {
    view.add("Could not connect (wrong password?). /wifi to retry.", ui::ERR);
    return false;
  }
  net::save(ssid, pass);
  view.add("Connected. Saved for next time.", ui::SYS);
  return true;
}

// The saved network is replaced only once a new one connects; cancelling or a failed join goes back to it.
void changeWifi() {
  const RelayHold hold;
  if (wifiSetup()) {
    timeOk = net::syncTime(cfg.tz.c_str());
  } else {
    String ssid, pass;
    net::loadSaved(ssid, pass);
    if (ssid.length()) {
      view.add("Reconnecting to " + ssid + "...", ui::SYS);
      if (net::connect(ssid, pass) && !timeOk) timeOk = net::syncTime(cfg.tz.c_str());
    }
  }
  refreshHeader();
}

// Boot progress on the splash; once the chat has taken over (the Wi-Fi picker), as a chat line.
void bootStep(const String& text, int done) {
  if (view.shown()) view.add(text, ui::SYS);
  else ui::splashStep(M5Cardputer.Display, text, done, kBootSteps);
}

// The network saved on the Cardputer (phone setup, the picker) first, then the SD card's.
bool connectKnown(void (*trying)(const String& ssid)) {
  String ssid, pass;
  net::loadSaved(ssid, pass);
  if (ssid.length()) {
    trying(ssid);
    if (net::connect(ssid, pass)) return true;
  }
  if (cfg.wifiSsid.length() && cfg.wifiSsid != ssid) {
    trying(cfg.wifiSsid);
    if (net::connect(cfg.wifiSsid, cfg.wifiPass)) return true;
  }
  return false;
}

// Known networks connect behind the splash; only the interactive picker needs the chat.
void bringUpNetwork() {
  bool up = connectKnown([](const String& ssid) { bootStep("Wi-Fi: " + ssid + "...", 2); });
  if (!up) {
    refreshHeader();
    view.show();
    up = wifiSetup();
  }
  if (!up) return;
  bootStep("syncing clock...", 3);
  timeOk = net::syncTime(cfg.tz.c_str());
  if (!timeOk) view.add("Clock sync failed; HTTPS may fail. /wifi to retry.", ui::ERR);
}
