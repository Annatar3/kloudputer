// Renders Kloudputer screens from the real firmware UI code (src/theme.h, ui.h, menu.h) into
// 240x135 PPMs, and checks that every screen paints every pixel and that in-place redraws end up
// identical to a fresh draw of the same state.
#include "shim.h"
#include "menu.h"
#include "captive.h"
#include "setup_form.h"
#include "setup_screen.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using Sprite = lgfx::LGFX_Sprite;
constexpr uint16_t kUnpainted = 0xF81F;

static int failures = 0;

static void save(Sprite& c, const std::string& name) {
  FILE* f = fopen((name + ".ppm").c_str(), "wb");
  fprintf(f, "P6 240 135 255\n");
  for (int y = 0; y < 135; ++y)
    for (int x = 0; x < 240; ++x) {
      const uint16_t p = c.readPixel(x, y);
      const uint8_t px[3] = {uint8_t((p >> 11) << 3), uint8_t(((p >> 5) & 63) << 2), uint8_t((p & 31) << 3)};
      fwrite(px, 1, 3, f);
    }
  fclose(f);
}

static int countColor(Sprite& c, uint16_t col) {
  int n = 0;
  for (int y = 0; y < 135; ++y)
    for (int x = 0; x < 240; ++x) n += c.readPixel(x, y) == col;
  return n;
}

static int diff(Sprite& a, Sprite& b) {
  int n = 0;
  for (int y = 0; y < 135; ++y)
    for (int x = 0; x < 240; ++x) n += a.readPixel(x, y) != b.readPixel(x, y);
  return n;
}

static void check(bool ok, const std::string& what, int detail) {
  printf("%s  %s (%d)\n", ok ? "ok  " : "FAIL", what.c_str(), detail);
  if (!ok) ++failures;
}

// A fresh screen: unpainted pixels stay magenta so a missed region shows up.
struct Screen {
  Sprite gfx;
  ui::Header header;
  ui::ChatView chat{header};
  ui::Menu menu{header};
  Screen() {
    gfx.setColorDepth(16);
    gfx.createSprite(240, 135);
    gfx.fillScreen(kUnpainted);
    header.begin(gfx);
    chat.begin(gfx);
    menu.begin(gfx);
    header.setNote("Haiku");
    header.setIcons({true, 3, 70});
  }
  void shot(const std::string& name) {
    check(countColor(gfx, kUnpainted) == 0, name + ": every pixel painted", countColor(gfx, kUnpainted));
    save(gfx, name);
  }
};

// An illustrative Android join after the fix, built with the same helpers the portal logs with.
static captive::EventLog androidLog() {
  using captive::Client;
  captive::EventLog log;
  const uint8_t mac[6] = {0x2a, 0x11, 0x5c, 0x90, 0x4e, 0xa1};
  log.add(400, captive::stationEvent(true, mac, -36));
  log.add(1200, captive::dnsEvent({"connectivitycheck.gstatic.com", 1}));
  log.add(1200, captive::dnsEvent({"www.google.com", 1}));
  log.add(1300, captive::httpEvent("GET", "connectivitycheck.gstatic.com", "/generate_204", 302, Client::AndroidProbe, false));
  log.add(1400, captive::httpEvent("GET", "www.google.com", "/gen_204", 302, Client::AndroidProbe, false));
  log.add(1900, captive::dnsEvent({"mtalk.google.com", 1}));
  log.add(2000, captive::dnsEvent({"mtalk.google.com", 28}));
  log.add(2600, captive::httpEvent("GET", "connectivitycheck.gstatic.com", "/generate_204", 302, Client::Webview, false));
  log.add(2700, captive::httpEvent("GET", "192.0.2.1", "/", 200, Client::Webview, false));
  log.add(2900, captive::httpEvent("GET", "192.0.2.1", "/scan", 200, Client::Webview, false));
  log.add(3100, captive::dnsEvent({"clients4.google.com", 65}));
  return log;
}

static void conversation(ui::ChatView& v) {
  v.add("Type or hold G0 to talk. Tab: menu.", ui::DIM);
  v.addUser("what's a good name for a cat?");
  v.addClaude("How about Miso? It\xE2\x80\x99s short, warm and easy to call across the room. Pepper and Biscuit "
              "are good too.");
  v.addUser("make it nerdier");
}

static const char* kLong =
    "Here's a quick plan for the weekend trip:\n\n"
    "1. Friday evening: pack light, check the weather and charge everything.\n"
    "2. Saturday: take the 8:15 train, drop bags at the hostel, walk the old town and find lunch near "
    "the river.\n"
    "3. Sunday: museum in the morning, then the hill trail for the sunset view.\n\n"
    "Budget about 120 for food and tickets. Want me to turn this into a checklist?";

static std::vector<ui::MenuItem> mainItems(const char* model = "Haiku 5.5") {
  return {{"New chat", ""},
          {"Sessions", "12 saved"},
          {"Agents", "2 paired"},
          {"Voice", "on - diana"},
          {"Voice via", "< Groq first >", "Left/right: who goes first   Del back"},
          {"Dictation", "< Auto >", "Left/right: language   Del back"},
          {"Web search", "< on >", "Left/right: on or off   Del back"},
          {"Screen sleep", "< 1 min >", "Left/right: dim then off   Del back"},
          {"Volume", "< 8 >", "Left / right change it   Del back"},
          {"Model", model},
          {"Usage", "$0.04 today", "Enter: estimated cost   Del back"},
          {"Notes", "3 saved", "Enter lists them   Del back"},
          {"Wi-Fi", "HomeNet", "Enter scans for networks   Del back"},
          {"Setup from phone", "", "Enter: Wi-Fi and keys from a phone"},
          {"Device info", ""},
          {"Keys/help", ""}};
}

// Where the Volume row is, so the checks keep pointing at it when rows are added above.
static int volumeRow() {
  const auto items = mainItems();
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].label == "Volume") return static_cast<int>(i);
  }
  return 0;
}

static std::vector<ui::MenuItem> voiceItems() {
  const char* names[] = {"Autumn", "Diana", "Hannah", "Austin", "Daniel", "Troy"};
  const char* kinds[] = {"female", "female", "female", "male", "male", "male"};
  std::vector<ui::MenuItem> items{{"Spoken answers", "on", "Enter turns them on / off   Del back"}};
  for (int i = 0; i < 6; ++i)
    items.push_back({names[i], String(kinds[i]) + (i == 1 ? "  *" : ""), "Enter: hear it and keep it   Del back"});
  return items;
}

static std::vector<ui::MenuItem> modelItems() {
  return {{"Haiku 5.5", "$0.1/$0.5  *", "fast, cheapest  ($/1M tokens in/out)"},
          {"Sonnet 5.5", "$2/$10", "balanced  ($/1M tokens in/out)"},
          {"Opus 5.5", "$4/$20", "smartest Opus  ($/1M tokens in/out)"},
          {"Fable 5.1", "$10/$50", "most capable  ($/1M tokens in/out)"}};
}

constexpr const char* kNav = "Arrows move  Enter pick  Del back";
constexpr const char* kSessionsHint = "Enter open  x delete  s find  Del back";
constexpr const char* kFindHint = "Type: titles  Enter: inside chats too";

// As main.cpp builds them: title (sessions::titleFrom of the first prompt), then sessions::when + current mark.
static std::vector<ui::MenuItem> sessionItems() {
  return {{"what's a good name for a cat?", "14:32 *"},
          {"Can you help me plan a weekend...", "09:05"},
          {"Explain how TLS certificates work", "Nov 5"},
          {"recipe for borscht", "Nov 3"},
          {"(untitled)", "-"},
          {"why is the sky blue", "Oct 30"},
          {"translate this to Georgian", "Oct 28"},
          {"write a haiku about my cardputer", "Oct 27"}};
}

// Same final state reached two ways must give the same pixels.
static void same(const std::string& what, const std::function<void(Screen&)>& incremental,
                 const std::function<void(Screen&)>& fresh) {
  Screen a, b;
  incremental(a);
  fresh(b);
  check(diff(a.gfx, b.gfx) == 0, what, diff(a.gfx, b.gfx));
}

int main() {
  {
    Screen s;
    ui::splash(s.gfx, "v1.9");
    ui::splashStep(s.gfx, "Wi-Fi: HomeNet...", 2, 4);
    s.shot("01_splash");
  }
  {
    Screen s;
    conversation(s.chat);
    s.chat.addClaude("Try Pixel, Kernel or Byte.");
    s.chat.show();
    s.chat.setInput("or Tensor");
    s.shot("02_chat");
  }
  {
    Screen s;
    conversation(s.chat);
    s.chat.thinking();
    s.chat.show();
    s.chat.setInput("");
    s.shot("03_thinking");
  }
  {
    Screen s;
    s.chat.addUser("plan my weekend trip to the old town, train both ways");
    s.chat.addClaude(kLong);
    s.chat.show();
    s.chat.setInput("");
    s.chat.scroll(6);
    s.shot("04_scrolled");
  }
  {
    Screen s;
    s.menu.open("Menu", mainItems(), 1, kNav);
    s.shot("05_menu");
  }
  {
    Screen s;
    s.menu.open("Voice", voiceItems(), 2, kNav);
    s.shot("06_voice");
  }
  {
    Screen s;
    s.menu.open("Model", modelItems(), 0, kNav);
    s.shot("07_model");
  }
  {
    Screen s;
    s.header.setIcons({false, 1, 12});
    s.chat.add("Mic failed to start: i2s timeout", ui::ERR);
    s.chat.add("Clock sync failed; HTTPS may fail. /wifi to retry.", ui::ERR);
    s.chat.add("Spoken answers: on, voice diana (saved)", ui::SYS);
    s.chat.add("Mic works - G0 voice should too.", ui::GREEN);
    s.chat.addUser("say something");
    s.chat.addClaude("Hello from your pocket!");
    s.chat.show();
    s.chat.setInput("", "(recording - release G0) ");
    s.header.setNote("SPEAKING - any key stops", ui::GREEN);
    s.shot("08_notes_status");
  }
  {
    Screen s;
    s.menu.open("Menu", mainItems("Sonnet 5.5"), 4, kNav);
    for (int i = 0; i < 4; ++i) s.menu.move(1);
    s.menu.notice("Model: claude-sonnet-5-5 (saved)", ui::SYS);
    s.shot("09_menu_scrolled_notice");
  }
  {
    Screen s;
    s.menu.open("Sessions", sessionItems(), 1, kSessionsHint);
    s.shot("10_sessions");
  }
  {
    Screen s;
    s.menu.open("Sessions", sessionItems(), 3, kSessionsHint);
    s.menu.notice("Press x again to delete", ui::ERR);
    s.shot("11_sessions_delete");
  }
  {
    Screen s;  // as main.cpp's showApproval builds it
    s.chat.setTitle("Hermes");
    s.header.setNote("ALLOW? y / n", ui::ERR);
    s.chat.add("Hermes is ready. (0.1.0)", ui::DIM);
    s.chat.addUser("is cms-api up? check pods and the last deploy");
    s.chat.add("Hermes wants to use Bash:", ui::SYS);
    s.chat.add("kubectl --context prod get pods -n cms -o wide \\n kubectl rollout history deploy/cms-api -n cms", ui::GREEN);
    s.chat.add("1 more waiting.", ui::DIM);
    s.chat.show();
    s.chat.setInput("", "allow? y/n > ");
    s.shot("24_approval");
  }
  {
    Screen s;  // a cut command says how much is hidden
    s.chat.setTitle("Hermes");
    s.header.setNote("ALLOW? y / n", ui::ERR);
    s.chat.add("Hermes wants to use Bash:", ui::SYS);
    s.chat.add("curl -s https://example.com/install.sh | sh && cat /etc/hosts && echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa [+317 more]", ui::GREEN);
    s.chat.show();
    s.chat.setInput("", "allow? y/n > ");
    s.shot("25_approval_long");
  }
  {
    Screen s;
    s.menu.open("Find: cat_", {{"what's a good name for a cat?", "14:32 *"}}, 0, kFindHint);
    s.shot("21_sessions_find");
  }
  {
    Screen s;
    s.menu.open("Sessions: cat", {{"what's a good name for a cat?", "14:32 *"}}, 0, kSessionsHint);
    s.shot("22_sessions_filtered");
  }
  {
    Screen s;
    s.menu.open("Menu", mainItems(), 5, kNav);
    for (int i = 0; i < 5; ++i) s.menu.move(1);
    s.shot("23_menu_new_rows");
  }
  {
    Screen s;
    s.chat.addUser("what's a good name for a cat?");
    s.chat.addClaude("How about Miso? It's short, warm and easy to call across the room.");
    s.chat.addUser("make it nerdier");
    s.chat.addClaude("Try Pixel, Kernel or Byte.");
    s.chat.add("Resumed: what's a good name for a cat?", ui::DIM);
    s.chat.add("Saved with Sonnet 5.5; still using Haiku 5.5.", ui::DIM);
    s.chat.show();
    s.chat.setInput("");
    s.shot("12_resumed");
  }
  {
    Screen s;
    ui::SetupScreen setup(s.header);
    setup.begin(s.gfx);
    s.header.setNote("10 min left");
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x00, 0x1A, 0x2B};
    setup.join(phone::wifiQr(phone::apName(mac), "k7mnp2xq4r"), phone::apName(mac), "k7mnp2xq4r");
    s.shot("13_setup_join");
  }
  {
    Screen s;
    ui::SetupScreen setup(s.header);
    setup.begin(s.gfx);
    s.header.setNote("9 min left");
    setup.open(phone::kPageUrl, phone::kApHost);
    setup.status("no requests yet", "");
    setup.status("3 req, GET / 200", "35ms chrome -32dBm");
    s.shot("14_setup_open");
  }
  {
    Screen s;
    ui::SetupScreen setup(s.header);
    setup.begin(s.gfx);
    s.header.setNote("");
    setup.busy("Looking for Wi-Fi networks...");
    s.shot("15_setup_busy");
  }
  {
    Screen s;
    ui::SetupScreen setup(s.header);
    setup.begin(s.gfx);
    s.header.setNote("9 min left");
    setup.log(androidLog().lines());
    s.shot("16_setup_log");
  }
  {
    Screen s;  // as main.cpp's agentsMenu builds it
    s.menu.open("Agents", {{"Hermes", phone::relayHost("")}, {"Lab", phone::relayHost("http://10.0.0.5:8080")}}, 0,
                "Enter opens   Del back");
    s.shot("17_agents_list");
  }
  {
    Screen s;
    s.menu.open("Agents", {{"Pair from your phone", "/setup"}, {"How to pair", ""}}, 0,
                "Codes come from the agent's connector");
    s.shot("18_agents_empty");
  }
  {
    Screen s;
    s.chat.setTitle("Hermes");
    s.header.setNote("working", ui::ORANGE);
    s.chat.add("Hermes is ready. (0.1.0)", ui::DIM);
    s.chat.addUser("is cms-api up? check pods and the last deploy");
    s.chat.placeholder("Bash: kubectl get pods -n cms -o wide", ui::DIM, false);
    s.chat.show();
    s.chat.setInput("");
    s.shot("19_agent_step");
  }
  {
    Screen s;
    s.chat.setTitle("Hermes");
    s.header.setNote("done", ui::GREEN);
    s.chat.add("Hermes is ready. (0.1.0)", ui::DIM);
    s.chat.addUser("is cms-api up? check pods and the last deploy");
    s.chat.placeholder("Bash: kubectl rollout history deploy/cms-api", ui::DIM, false);
    s.chat.addClaude("Yes. cms-api is up: 3/3 pods Running on gke-prod, no restarts in 24 h. The last deploy was "
                     "v2.14.1 yesterday at 16:05, rolled out cleanly. p95 latency is 140 ms.");
    s.chat.add("Done after 38 s, $0.04.", ui::DIM);
    s.chat.show();
    s.chat.setInput("");
    s.shot("20_agent_report");
  }

  same("chat: added while shown == added then shown",
       [](Screen& s) {
         s.chat.show();
         s.chat.setInput("");
         conversation(s.chat);
         s.chat.addClaude(kLong);
       },
       [](Screen& s) {
         conversation(s.chat);
         s.chat.addClaude(kLong);
         s.chat.show();
         s.chat.setInput("");
       });
  same("chat: thinking replaced by the reply == reply alone",
       [](Screen& s) {
         conversation(s.chat);
         s.chat.show();
         s.chat.setInput("");
         s.chat.thinking();
         s.chat.addClaude("Try Pixel, Kernel or Byte.");
       },
       [](Screen& s) {
         conversation(s.chat);
         s.chat.addClaude("Try Pixel, Kernel or Byte.");
         s.chat.show();
         s.chat.setInput("");
       });
  same("chat: scroll back and forth == not scrolled",
       [](Screen& s) {
         s.chat.addClaude(kLong);
         s.chat.addClaude(kLong);
         s.chat.show();
         s.chat.setInput("");
         s.chat.scroll(9);
         s.chat.scroll(99);
         s.chat.scroll(-4);
         s.chat.scroll(-200);
       },
       [](Screen& s) {
         s.chat.addClaude(kLong);
         s.chat.addClaude(kLong);
         s.chat.show();
         s.chat.setInput("");
       });
  same("chat: long input then cleared == empty input",
       [](Screen& s) {
         s.chat.show();
         s.chat.setInput("a very long line that runs past the edge of the input bar for sure");
         s.chat.setInput("pass", "password > ", true);
         s.chat.setInput("");
       },
       [](Screen& s) {
         s.chat.show();
         s.chat.setInput("");
       });
  same("chat: menu round trip == chat",
       [](Screen& s) {
         conversation(s.chat);
         s.chat.show();
         s.chat.setInput("hi");
         s.chat.hide();
         s.menu.open("Menu", mainItems(), 0, kNav);
         s.menu.open("Voice", voiceItems(), 3, kNav);
         s.chat.show();
       },
       [](Screen& s) {
         conversation(s.chat);
         s.chat.show();
         s.chat.setInput("hi");
       });
  same("header: long status then model == model",
       [](Screen& s) {
         s.chat.show();
         s.chat.setInput("");
         s.header.setNote("level 812 - transcribing...", ui::SYS);
         s.header.setIcons({false, 0, -1});
         s.header.setNote("Haiku");
         s.header.setIcons({true, 3, 70});
       },
       [](Screen& s) {
         s.chat.show();
         s.chat.setInput("");
       });
  same("menu: move around (wrap + scroll) and back == fresh",
       [](Screen& s) {
         s.menu.open("Menu", mainItems(), 0, kNav);
         for (int i = 0; i < 7; ++i) s.menu.move(1);
         s.menu.notice("Volume 9/10 (saved)", ui::SYS);
         s.menu.move(1);
         s.menu.move(-1);
         for (int i = 0; i < 7; ++i) s.menu.move(-1);
       },
       [](Screen& s) { s.menu.open("Menu", mainItems(), 0, kNav); });
  same("menu: value change == fresh",
       [](Screen& s) {
         auto items = mainItems();
         s.menu.open("Menu", items, volumeRow(), kNav);
         s.menu.setValue(volumeRow(), "< 10 >");
         s.menu.setValue(volumeRow(), "< 8 >");
       },
       [](Screen& s) { s.menu.open("Menu", mainItems(), volumeRow(), kNav); });
  same("setup log: events one by one == all at once",
       [](Screen& s) {
         ui::SetupScreen setup(s.header);
         setup.begin(s.gfx);
         s.header.setNote("9 min left");
         const captive::EventLog all = androidLog();
         captive::EventLog growing;
         for (const String& line : all.lines()) {
           growing.add(0, line);
           std::deque<String> shown;
           for (const String& l : growing.lines()) shown.push_back(l.substring(5));
           setup.log(shown);
         }
       },
       [](Screen& s) {
         ui::SetupScreen setup(s.header);
         setup.begin(s.gfx);
         s.header.setNote("9 min left");
         setup.log(androidLog().lines());
       });
  same("agent: a step replaced by the next, then by the report == report alone",
       [](Screen& s) {
         s.chat.setTitle("Hermes");
         s.chat.addUser("is cms-api up?");
         s.chat.show();
         s.chat.setInput("");
         s.chat.placeholder("Bash: kubectl get pods -n cms -o wide, plus a long tail so it wraps over two lines", ui::DIM, false);
         s.chat.placeholder("Bash: kubectl rollout history", ui::DIM, false);
         s.chat.addClaude("Yes, it's up.");
       },
       [](Screen& s) {
         s.chat.setTitle("Hermes");
         s.chat.addUser("is cms-api up?");
         s.chat.addClaude("Yes, it's up.");
         s.chat.show();
         s.chat.setInput("");
       });
  same("sessions: delete prompt cancelled == fresh",
       [](Screen& s) {
         s.menu.open("Sessions", sessionItems(), 2, kSessionsHint);
         s.menu.notice("Press x again to delete", ui::ERR);
         s.menu.notice("", ui::DIM);
       },
       [](Screen& s) { s.menu.open("Sessions", sessionItems(), 2, kSessionsHint); });

  printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
