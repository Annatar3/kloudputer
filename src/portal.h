// Kloudputer — setup from a phone. The Cardputer opens its own WPA2 network, whose password is only
// on its screen, and answers every DNS name and web request with the setup page. Phones then pop
// their "sign in to network" sheet by themselves. It needs no app and works next to any network,
// guest Wi-Fi included. Everything the phone asks for goes into a log (L on the setup screen).
#pragma once
#include <ArduinoJson.h>
#include <AsyncUDP.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <algorithm>
#include <array>
#include <vector>
#include "captive.h"
#include "config.h"
#include "setup_form.h"
#include "setup_screen.h"

namespace portal {
constexpr uint32_t kTimeoutMs = 10 * 60 * 1000;

enum class End : uint8_t { Saved, Cancelled, TimedOut, Failed };
enum class Key : uint8_t { None, Cancel, Log, Other };

struct Outcome {
  End end = End::Cancelled;
  phone::Form form;  // what was saved; its Wi-Fi is tried before it replaces the saved network
};

struct Setup {
  phone::Current now;
  std::vector<phone::Choice> models;
  Key (*readKey)();
};

// Networks for the page's select, strongest first, one entry per name.
inline String scanJson() {
  struct Net {
    String ssid;
    int rssi;
    bool open;
  };
  std::vector<Net> nets;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty() || std::any_of(nets.begin(), nets.end(), [&ssid](const Net& x) { return x.ssid == ssid; })) continue;
    nets.push_back({ssid, static_cast<int>(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (size_t i = 0; i < nets.size() && i < 20; ++i) {
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["open"] = nets[i].open;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// POST /save. Reading the body through the raw hook caps it at phone::kMaxBody; WebServer's own form
// parsing would first allocate whatever Content-Length the client claims.
class SaveHandler : public RequestHandler {
 public:
  SaveHandler(const Setup& setup, Outcome& out) : setup_(setup), out_(out) {}

  bool canHandle(HTTPMethod method, const String& uri) override { return method == HTTP_POST && uri == "/save"; }
  bool canHandle(WebServer&, HTTPMethod method, const String& uri) override { return canHandle(method, uri); }
  bool canRaw(const String& uri) override { return uri == "/save"; }
  bool canRaw(WebServer&, const String& uri) override { return canRaw(uri); }

  void raw(WebServer&, const String&, HTTPRaw& raw) override {
    if (raw.status == RAW_START) body_.clear();
    else if (raw.status == RAW_WRITE) body_.add(raw.buf, raw.currentSize);
  }

  bool handle(WebServer& server, HTTPMethod, const String&) override {
    const phone::Form form = phone::parseForm(body_.text);
    String error = body_.oversized ? String("That was more than 4 KB, so nothing was saved.")
                                   : phone::validate(form, setup_.now.hasAnthropic, setup_.models, setup_.now.agents);
    if (error.isEmpty() && !savePhone(form.anthropicKey, form.groqKey, form.model, form.tz, form.geminiKey)) {
      error = "The Cardputer couldn't store that. Try again.";
    }
    if (error.isEmpty() && (form.agentName.length() || !form.removeAgents.empty())) {
      std::vector<AgentEntry> agents = loadAgents();
      editAgents(agents, form.removeAgents, {form.agentName, form.agentCode, form.agentRelay});
      if (!saveAgents(agents)) error = "The Cardputer couldn't store the agents. Try again.";
    }
    body_.clear();
    server.sendHeader("Cache-Control", "no-store");
    if (error.length()) {
      server.send(200, "text/html", phone::page(setup_.now, setup_.models, form, error));
      return true;
    }
    out_.end = End::Saved;
    out_.form = form;
    server.send(200, "text/html", phone::savedPage());
    return true;
  }

 private:
  const Setup& setup_;
  Outcome& out_;
  phone::Body body_;
};

// What the AsyncUDP task hands the main loop about one DNS question; only the loop touches the log.
struct DnsSeen {
  char name[64];
  uint16_t type;
};

// Answers DNS on port 53 for the setup network (captive::dnsReply), and queues each question for the log.
class Dns {
 public:
  bool start() {
    seen_ = xQueueCreate(16, sizeof(DnsSeen));
    if (!seen_ || !udp_.listen(53)) return false;
    udp_.onPacket([this](AsyncUDPPacket& packet) {
      std::vector<uint8_t> reply;
      captive::DnsQuestion asked;
      if (!captive::dnsReply(packet.data(), packet.length(), phone::kApIp, reply, asked)) return;
      packet.write(reply.data(), reply.size());
      DnsSeen seen{};
      strncpy(seen.name, asked.name.c_str(), sizeof seen.name - 1);
      seen.type = asked.type;
      xQueueSend(seen_, &seen, 0);
    });
    return true;
  }

  bool next(DnsSeen& seen) { return seen_ && xQueueReceive(seen_, &seen, 0) == pdTRUE; }

  void stop() {
    udp_.close();
    if (seen_) vQueueDelete(seen_);
    seen_ = nullptr;
  }

 private:
  AsyncUDP udp_;
  QueueHandle_t seen_ = nullptr;
};

// Phones on the setup network, polled: the log shows each join and leave with its signal.
class Stations {
 public:
  void poll(captive::EventLog& log, uint32_t ms) {
    wifi_sta_list_t list{};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return;
    for (int i = 0; i < list.num; ++i) {
      if (!known(list.sta[i].mac)) log.add(ms, captive::stationEvent(true, list.sta[i].mac, list.sta[i].rssi));
    }
    for (const auto& mac : macs_) {
      bool still = false;
      for (int i = 0; i < list.num; ++i) still = still || memcmp(mac.data(), list.sta[i].mac, 6) == 0;
      if (!still) log.add(ms, captive::stationEvent(false, mac.data(), 0));
    }
    macs_.clear();
    for (int i = 0; i < list.num; ++i) macs_.push_back(toArray(list.sta[i].mac));
    rssi_ = list.num ? list.sta[0].rssi : 0;
  }

  int count() const { return macs_.size(); }
  int rssi() const { return rssi_; }

 private:
  static std::array<uint8_t, 6> toArray(const uint8_t* mac) {
    std::array<uint8_t, 6> a;
    memcpy(a.data(), mac, 6);
    return a;
  }
  bool known(const uint8_t* mac) const {
    return std::any_of(macs_.begin(), macs_.end(), [mac](const std::array<uint8_t, 6>& m) { return memcmp(m.data(), mac, 6) == 0; });
  }
  std::vector<std::array<uint8_t, 6>> macs_;
  int rssi_ = 0;
};

// Runs setup until the phone saves, Del or Tab cancels, or kTimeoutMs passes; owns the screen meanwhile.
// L shows the log of what the phone asked for. Leaves Wi-Fi in plain station mode, disconnected: the
// caller reconnects with whatever was saved.
inline Outcome run(ui::Gfx& d, ui::Header& header, const Setup& setup) {
  Outcome out;
  ui::SetupScreen screen(header);
  screen.begin(d);
  header.setNote("");
  screen.busy("Looking for Wi-Fi networks...");
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  const String networks = scanJson();
  // Only the access point from here on: a station interface left up can pull the radio off the AP's
  // channel, which phones see as a network that accepts connections and then drops them.
  WiFi.mode(WIFI_AP);

  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  const String apName = phone::apName(mac);
  const String apPass = phone::apPassword(esp_random);
  const IPAddress ip(phone::kApIp);
  if (!WiFi.softAP(apName.c_str(), apPass.c_str()) ||
      !WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), IPAddress(), ip)) {
    WiFi.mode(WIFI_STA);
    out.end = End::Failed;
    return out;
  }
  // No DHCP option 114: Android only fetches an https:// captive portal API URL (NetworkMonitor drops
  // anything else), and an https one would need a certificate for 192.0.2.1 that no phone trusts.
  Dns dns;
  dns.start();
  captive::EventLog events;
  Stations stations;
  const uint32_t t0 = millis();

  WebServer server(80);
  // Step 2 shows the latest request, so a photo of the screen says where setup got stuck.
  int requests = 0;
  String lastRequest, lastDetail;
  const char* collect[] = {"User-Agent", "Accept"};
  server.collectHeaders(collect, 2);
  server.addMiddleware([&](WebServer& srv, Middleware::Callback next) {
    const uint32_t start = millis();
    const bool ok = next();
    ++requests;
    const String method = srv.method() == HTTP_POST ? "POST" : "GET";
    const captive::Client client = captive::classify(srv.header("User-Agent"));
    events.add(start - t0, captive::httpEvent(method, srv.hostHeader(), srv.uri(), srv.responseCode(), client,
                                              captive::wantsCapport(srv.header("Accept"))));
    lastRequest = method + " " + srv.uri().substring(0, 12) + " " + srv.responseCode();
    lastDetail = String(millis() - start) + "ms " + captive::clientName(client) + " " + stations.rssi() + "dBm";
    return ok;
  });
  // RFC 8908: whoever asks for the Captive Portal API by media type gets it, on any path.
  server.addMiddleware([&](WebServer& srv, Middleware::Callback next) {
    if (!captive::wantsCapport(srv.header("Accept"))) return next();
    srv.sendHeader("Cache-Control", "private");
    srv.send(200, "application/captive+json", captive::capportJson(phone::kPageUrl));
    return true;
  });
  server.on("/ping", [&server] { server.send(200, "text/plain", "pong"); });
  auto toPage = [&server] {
    server.sendHeader("Location", phone::kPageUrl, true);
    server.send(302, "text/plain", "");
  };
  // The connectivity checks of Android, iOS/macOS and Windows on whatever host they use; a redirect is
  // what Android's NetworkMonitor counts as a portal (any 200-399 but 204), and the others too.
  for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                            "/connecttest.txt", "/ncsi.txt", "/fwlink", "/redirect", "/canonical.html", "/success.txt"}) {
    server.on(probe, toPage);
  }
  server.on("/", HTTP_GET, [&] {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", phone::page(setup.now, setup.models, phone::Form(), ""));
  });
  server.on("/scan", HTTP_GET, [&] { server.send(200, "application/json", networks); });
  server.addHandler(new SaveHandler(setup, out));  // the server deletes its handlers
  server.onNotFound(toPage);
  server.begin();

  enum class View : uint8_t { None, Join, Open, Log };
  View shown = View::None;
  bool logOpen = false;
  uint32_t logShown = 0, lastPoll = 0;
  int requestsShown = -1, minutesShown = -1;
  while (out.end != End::Saved) {
    server.handleClient();
    const uint32_t elapsed = millis() - t0;
    for (DnsSeen seen; dns.next(seen);) events.add(elapsed, captive::dnsEvent({seen.name, seen.type}));
    if (elapsed - lastPoll >= 500) {
      lastPoll = elapsed;
      stations.poll(events, elapsed);
    }

    const Key key = setup.readKey();
    if (logOpen && key != Key::None) {
      logOpen = false;
    } else if (key == Key::Log) {
      logOpen = true;
    } else if (key == Key::Cancel) {
      out.end = End::Cancelled;
      break;
    }

    const View want = logOpen ? View::Log : stations.count() > 0 ? View::Open : View::Join;
    if (want == View::Log && (shown != View::Log || events.added() != logShown)) {
      screen.log(events.lines());
      logShown = events.added();
    } else if (want != shown && want == View::Join) {
      screen.join(phone::wifiQr(apName, apPass), apName, apPass);
    } else if (want != shown && want == View::Open) {
      screen.open(phone::kPageUrl, phone::kApHost);
      requestsShown = -1;
    }
    shown = want;
    if (shown == View::Open && requests != requestsShown) {
      requestsShown = requests;
      screen.status(requests ? String(requests) + " req, " + lastRequest : String("no requests yet"),
                    requests ? lastDetail : String());
    }

    const int left = (kTimeoutMs - std::min(elapsed, kTimeoutMs) + 59999) / 60000;
    if (left != minutesShown) {
      minutesShown = left;
      header.setNote(String(left) + " min left");
    }
    if (elapsed >= kTimeoutMs) {
      out.end = End::TimedOut;
      break;
    }
    delay(2);
  }
  if (out.end == End::Saved) {
    screen.busy("Saved. Leaving setup...");
    for (const uint32_t t1 = millis(); millis() - t1 < 1000;) {  // lets the "Saved" page finish sending
      server.handleClient();
      delay(5);
    }
  }
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  return out;
}
}  // namespace portal
