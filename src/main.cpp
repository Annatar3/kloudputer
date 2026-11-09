// Kloudputer — Claude chat on the M5Stack Cardputer ADV, with push-to-talk voice and spoken answers.
// Type or hold G0 and talk; Enter sends. Tab opens the menu. Fn+; / Fn+. recall earlier prompts, Fn+, / Fn+/ scroll. Commands: /help.
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <functional>
#include <iterator>
#include <vector>
#include "agent_proto.h"
#include "agent_relay.h"
#include "approvals.h"
#include "anthropic.h"
#include "config.h"
#include "keyboard_adv.h"
#include "keycheck.h"
#include "menu.h"
#include "net.h"
#include "notes.h"
#include "portal.h"
#include "power.h"
#include "prompt_history.h"
#include "sessions.h"
#include "tts.h"
#include "ui.h"
#include "usage.h"
#include "voice.h"

// The loop task runs every TLS handshake. From the agent screen, voice upload sits ~7 calls deep (menu, agents,
// agent screen, voice, Groq, TLS) and overflowed the default 8 KB stack. /info shows the measured headroom.
constexpr size_t kLoopStack = 16 * 1024;
size_t getArduinoLoopTaskStackSize() { return kLoopStack; }

namespace {
constexpr const char* kVersion = "v1.9";
constexpr float kSilentRms = 20;  // clip RMS (around its mean) below this is treated as silence
constexpr size_t kMaxInput = 2000;
constexpr size_t kMaxHistoryMsgs = 10;
constexpr size_t kMaxHistoryChars = 8000;
constexpr int kBootSteps = 4;  // keyboard, SD card, Wi-Fi, clock
constexpr size_t kMaxSessions = 50;  // the Sessions list shows the newest ones

// ADV microSD (SPI)
constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

struct ModelOpt {
  const char* id;
  const char* desc;
};
// Prices live in usage.h, which also adds up what the chats cost.
constexpr ModelOpt kModels[] = {
    {"claude-haiku-5-5", "fast, cheapest"},
    {"claude-sonnet-5-5", "balanced"},
    {"claude-opus-5-5", "smartest Opus"},
    {"claude-fable-5-1", "most capable"},
};
static_assert(std::size(kModels) == std::size(usage::kPrices), "every model on the menu has a price");

// In the order of tts::kVoices.
constexpr const char* kVoiceKinds[] = {"female", "female", "female", "male", "male", "male"};
static_assert(std::size(kVoiceKinds) == std::size(tts::kVoices), "one kind per voice");

ui::Header header;
ui::ChatView view(header);
ui::Menu menu(header);
Config cfg;
std::vector<ChatMsg> history;
PollingTCA8418Reader* kb = nullptr;
String input;
prompts::History promptHistory;
size_t historyBytes = 0;  // size of the saved history file
int dictation = 0;  // index into lang::kLanguages
power::ScreenSleep screenSleep;
int sleepOption = power::kDefaultOption;  // index into power::kOptions
uint8_t fullBrightness = 128;
usage::Tally spent;  // estimated cost: this chat, today, and all time
notes::Notes memory;  // what Claude was asked to remember, from the SD card
bool webSearch = true;  // let Claude search the web; off for the session if the API refuses it
int boardId = -1;
bool sdOk = false;
bool timeOk = false;
bool voiceReplies = false;  // speak Claude's answers; never on without a Groq key
const char* voiceName = tts::kVoices[0];
int volume = 8;             // speaker 1-10
uint32_t lastHeaderMs = 0;
bool cryptoOk = false;  // the agent protocol's test vector checks out on this device's mbedTLS
ui::ChatView agentView(header);
String agentInput;
String agentViewOf;  // whose transcript agentView holds; this session only, in RAM
sessions::Meta session;  // where this chat is saved; no id until its first turn is
bool saveWarned = false;

// ---------- hardware ----------
void startKeyboard() {
  boardId = static_cast<int>(M5.getBoard());
  // The ADV keyboard (TCA8418 @0x34) lives on SDA=8/SCL=9; bring that bus up if autodetect didn't.
  if (boardId != static_cast<int>(m5::board_t::board_M5CardputerADV) || !M5.In_I2C.isEnabled()) {
    M5.In_I2C.release();
    M5.In_I2C.begin(I2C_NUM_1, 8, 9);
  }
  kb = new PollingTCA8418Reader();
  M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(kb));
}

// ---------- input ----------
struct KeyEvent {
  bool enter = false, esc = false, tab = false, back = false, changed = false;
  bool up = false, down = false, left = false, right = false;
  int scroll = 0;
};

// The keyboard state if a key just went down, else nullptr. Also refreshes button state (G0).
// An agent's job can keep running after its screen is left; agentBackgroundTick (below, with the agent
// screen) keeps listening for it from wherever keys are read.
void agentBackgroundTick();
void agentPause();
int agentHolds = 0;

// While one of these is alive the agent relay stays closed: only one TLS session fits in memory, and Claude,
// Whisper and speech each need it.
struct RelayHold {
  RelayHold() {
    agentPause();
    ++agentHolds;
  }
  ~RelayHold() { --agentHolds; }
  RelayHold(const RelayHold&) = delete;
  RelayHold& operator=(const RelayHold&) = delete;
};

void setBacklight(power::Level level) {
  const uint8_t dim = std::max<int>(4, fullBrightness / 8);
  M5Cardputer.Display.setBrightness(level == power::Level::Awake ? fullBrightness : level == power::Level::Dim ? dim : 0);
  // With the screen off there is nothing to draw, so the CPU drops to the slowest clock Wi-Fi still works at.
  setCpuFrequencyMhz(level == power::Level::Off ? 80 : 240);
}

// Wakes the screen when something shows on it without a key press, like a reply arriving.
void pokeScreen() {
  const power::Step step = screenSleep.poke(millis(), power::kOptions[sleepOption].ms);
  if (step.changed) setBacklight(step.level);
}

const Keyboard_Class::KeysState* nextKeys() {
  agentBackgroundTick();
  M5Cardputer.update();
  M5Cardputer.Keyboard.updateKeyList();
  M5Cardputer.Keyboard.updateKeysState();
  const bool pressed = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
  const power::Step step =
      screenSleep.tick(millis(), power::kOptions[sleepOption].ms, pressed || M5Cardputer.BtnA.isPressed());
  if (step.changed) setBacklight(step.level);
  if (!pressed || step.swallow) return nullptr;
  return &M5Cardputer.Keyboard.keysState();
}

// Applies one keyboard change to `buf`; returns what happened.
KeyEvent pollKeys(String& buf) {
  KeyEvent ev;
  const auto* st = nextKeys();
  if (!st) return ev;
  if (st->up) ev.scroll = 3;
  if (st->down) ev.scroll = -3;
  ev.up = st->up;
  ev.down = st->down;
  ev.left = st->left;
  ev.right = st->right;
  if (st->esc) ev.esc = true;
  if (st->tab) ev.tab = true;
  if (st->backspace || st->del) {
    ev.back = true;
    if (buf.length()) buf.remove(buf.length() - 1);
    ev.changed = true;
  }
  if (!st->fn) {
    for (char c : st->word) {
      if (buf.length() < kMaxInput) buf += c;
      ev.changed = true;
    }
  }
  if (st->enter) ev.enter = true;
  return ev;
}

// Blocking single-line prompt (setup, pickers). Returns "" on Esc.
String readLine(const char* prompt, bool mask = false) {
  String buf;
  view.setInput(buf, prompt, mask);
  while (true) {
    const KeyEvent ev = pollKeys(buf);
    if (ev.esc) { view.setInput("", "> "); return ""; }
    if (ev.enter) { view.setInput("", "> "); return buf; }
    if (ev.scroll) view.scroll(ev.scroll);
    if (ev.changed) view.setInput(buf, prompt, mask);
    delay(5);
  }
}

// ---------- status ----------
String shortModel(const String& id) {
  return id.startsWith("claude-") ? id.substring(7) : id;
}

// "claude-haiku-5-5" -> "Haiku 5.5"; ids not shaped like that keep their own spelling.
String modelName(const String& id) {
  const String s = shortModel(id);
  const int dash = s.indexOf('-');
  if (dash <= 0) return s;
  String version = s.substring(dash + 1);
  for (size_t i = 0; i < version.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(version[i])) && version[i] != '-') return s;
  }
  version.replace("-", ".");
  String family = s.substring(0, dash);
  family.setCharAt(0, toupper(family[0]));
  return family + " " + version;
}

String modelFamily(const String& id) {
  const String name = modelName(id);
  const int sp = name.indexOf(' ');
  return sp > 0 ? name.substring(0, sp) : name;
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  const int rssi = WiFi.RSSI();
  return rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
}

void refreshHeader() {
  const bool up = WiFi.status() == WL_CONNECTED;
  // M5Unified reads the battery on Cardputer boards (ADC on GPIO10); elsewhere the level is negative
  // and the icon stays hidden.
  header.setIcons({voiceReplies, wifiBars(), static_cast<int>(M5.Power.getBatteryLevel())});
  header.setNote(up ? modelFamily(cfg.model) : String("offline"), up ? ui::DIM : ui::ERR);
}

// ---------- model ----------
void setModel(const String& id) {
  cfg.model = id;
  Preferences p;
  p.begin("cputer", false);
  p.putString("model", id);
  p.end();
  view.add("Model: " + id + " (saved)", ui::SYS);
  refreshHeader();
}

// Needs cfg loaded: voice defaults to on when a Groq key exists.
void loadPrefs() {
  bool voice = true;
  Preferences p;
  if (p.begin("cputer", true)) {
    voice = p.getBool("voice", voice);
    if (const char* v = tts::findVoice(p.getString("vname", ""))) voiceName = v;
    volume = p.getUChar("vol", volume);
    dictation = lang::indexOf(p.getString("lang", ""));
    webSearch = p.getBool("web", webSearch);
    spent.date = p.getUInt("udate", 0);
    spent.day = p.getUInt("uday", 0);
    spent.total = p.getUInt("utotal", 0);
    sleepOption = power::optionFor(p.getUInt("sleep", power::kOptions[power::kDefaultOption].ms));
    p.end();
  }
  voiceReplies = voice && cfg.groqKey.length();
  adv_audio::setVolume(volume);
}

// The SD files, then what was saved on the Cardputer on top (phone setup, Model menu), then voice
// and volume. Runs at boot and again after phone setup, which needs no reboot.
void loadSettings() {
  cfg = Config();
  if (sdOk) loadConfig(cfg);
  loadPhone(cfg);
  loadPrefs();
  if (sdOk) memory = notes::load();
  setenv("TZ", cfg.tz.c_str(), 1);  // dates in the Sessions list, before a clock sync sets it again
  tzset();
}

void chooseModel() {
  const int n = std::size(kModels);
  for (int i = 0; i < n; ++i) {
    const bool cur = cfg.model == kModels[i].id;
    view.add(String(i + 1) + ") " + shortModel(kModels[i].id) + (cur ? " *" : "") + "  " + kModels[i].desc + "  " +
                 usage::label(kModels[i].id),
             cur ? ui::GREEN : ui::DIM);
  }
  view.add("(price per 1M tokens in/out)", ui::DIM);
  const String pick = readLine("model # > ");
  const int idx = pick.toInt() - 1;
  if (idx < 0 || idx >= n) {
    view.add("Kept " + cfg.model, ui::SYS);
    return;
  }
  setModel(kModels[idx].id);
}

#include "app/wifi.h"
#include "app/speech.h"
#include "app/chat.h"
#include "app/voice.h"
#include "app/commands.h"
#include "app/menus.h"
#include "app/setup.h"
#include "app/agents.h"

struct MainRow {
  const char* label;
  String (*value)();        // nullptr: no value
  Then (*enter)();          // nullptr: Enter does nothing
  void (*adjust)(int dir);  // left/right
  const char* hint;
};

const MainRow kMainMenu[] = {
    {"New chat", nullptr, inChat<newChat>, nullptr, nullptr},
    {"Sessions", savedCount, sessionsMenu, nullptr, nullptr},
    {"Agents", agentsValue, agentsMenu, nullptr, nullptr},
    {"Voice",
     []() -> String {
       if (cfg.groqKey.isEmpty()) return "needs a voice key";
       return voiceReplies ? "on - " + String(voiceName) : String("off");
     },
     inMenu<voiceMenu>, nullptr, nullptr},
    {"Dictation", []() -> String { return String("< ") + lang::kLanguages[dictation].name + " >"; }, nullptr,
     [](int dir) {
       dictation = lang::step(dictation, dir);
       Preferences p;
       if (p.begin("cputer", false)) {
         p.putString("lang", lang::kLanguages[dictation].code);
         p.end();
       }
     },
     "Left/right: language   Del back"},
    {"Web search", []() -> String { return String("< ") + (webSearch ? "on" : "off") + " >"; }, nullptr,
     [](int) {
       webSearch = !webSearch;
       Preferences p;
       if (p.begin("cputer", false)) {
         p.putBool("web", webSearch);
         p.end();
       }
     },
     "Left/right: on or off   Del back"},
    {"Screen sleep", []() -> String { return String("< ") + power::kOptions[sleepOption].name + " >"; }, nullptr,
     [](int dir) {
       sleepOption = power::stepOption(sleepOption, dir);
       Preferences p;
       if (p.begin("cputer", false)) {
         p.putUInt("sleep", power::kOptions[sleepOption].ms);
         p.end();
       }
     },
     "Left/right: dim then off   Del back"},
    {"Volume", []() -> String { return "< " + String(volume) + " >"; }, nullptr,
     [](int dir) { applyVolume(std::min(std::max(volume + dir, 1), 10)); }, "Left / right change it   Del back"},
    {"Model", []() -> String { return modelName(cfg.model); }, inMenu<modelMenu>, nullptr, nullptr},
    {"Usage", []() -> String { return usage::money(usage::spentToday(spent, todayStamp())) + " today"; }, inChat<showUsage>,
     nullptr, "Enter: estimated cost   Del back"},
    {"Notes", []() -> String { return String(memory.items().size()) + " saved"; }, inChat<showNotes>, nullptr,
     "Enter lists them   Del back"},
    {"Wi-Fi", []() -> String { return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("offline"); },
     inChat<changeWifi>, nullptr, "Enter scans for networks   Del back"},
    {"Setup from phone", nullptr, setupFromPhone, nullptr, "Enter: Wi-Fi and keys from a phone"},
    {"Device info", nullptr, inChat<showInfo>, nullptr, nullptr},
    {"Keys/help", nullptr, inChat<showHelp>, nullptr, nullptr},
};

// The Tab menu.
Then mainMenu() {
  int row = 0;
  while (true) {
    std::vector<ui::MenuItem> items;
    for (const MainRow& r : kMainMenu) items.push_back({r.label, r.value ? r.value() : String(), r.hint});
    showMenu("Menu", std::move(items), row);
    Choice c;
    do {
      c = navigate([](int at, int dir) {
        if (!kMainMenu[at].adjust) return;
        kMainMenu[at].adjust(dir);
        menu.setValue(at, kMainMenu[at].value());
      });
    } while (c.nav == Nav::Pick && !kMainMenu[c.row].enter);
    if (c.nav == Nav::Back) return {};
    row = c.row;
    if (const Then then = kMainMenu[row].enter()) return then;
  }
}

// Gives the screen to a menu; the chat comes back with its transcript when the menu is done.
void openMenu(Then (*screen)()) {
  menuSeen = view.added();
  view.hide();
  const Then then = screen();
  view.show();
  if (then) then();
}

void handleCommand(const String& line) {
  const int sp = line.indexOf(' ');
  const String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? String() : line.substring(sp + 1);
  arg.trim();
  if (cmd == "/help") showHelp();
  else if (cmd == "/new") newChat();
  else if (cmd == "/wifi") changeWifi();
  else if (cmd == "/info") showInfo();
  else if (cmd == "/mic") micTest();
  else if (cmd == "/codec") {
    view.add("codec FD FE 00 01 02 09 0A 0D 0E 14 17:", ui::DIM);
    view.add(adv_audio::codecDump(), ui::DIM);
  } else if (cmd == "/model") {
    if (arg.length()) setModel(arg);
    else chooseModel();
  } else if (cmd == "/voice") voiceCommand(arg);
  else if (cmd == "/vol") volCommand(arg);
  else if (cmd == "/say") sayCommand(arg);
  else if (cmd == "/sessions") openMenu(sessionsMenu);
  else if (cmd == "/setup") openMenu(setupFromPhone);
  else if (cmd == "/pair") pairCommand(arg);
  else if (cmd == "/remember") remember(arg);
  else if (cmd == "/notes") showNotes();
  else if (cmd == "/forget") forget(arg);
  else view.add("Unknown command. /help", ui::ERR);
}
}  // namespace

void setup() {
  auto m5cfg = M5.config();
  // M5Unified's speaker/mic must never claim the codec pins: on IDF 5.5 the mic reads a constant -1
  // after the speaker has used them. Audio is ours alone (audio_adv.h).
  m5cfg.internal_spk = false;
  m5cfg.internal_mic = false;
  M5Cardputer.begin(m5cfg, false);  // keyboard comes up in startKeyboard()
  Serial.begin(115200);
  auto& display = M5Cardputer.Display;
  display.setRotation(1);
  fullBrightness = display.getBrightness() >= 16 ? display.getBrightness() : 128;
  header.begin(display);
  view.begin(display);
  agentView.begin(display);
  menu.begin(display);
  ui::splash(display, kVersion);

  bootStep("starting keyboard...", 0);
  startKeyboard();
  String micErr;
  const bool micOk = adv_audio::start(micErr);  // mic runs from boot: push-to-talk starts instantly

  bootStep("reading SD card...", 1);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI, 25000000);
  const Migration moved = sdOk ? migrateDataDir() : Migration::None;
  if (moved == Migration::Moved) {
    view.add(String("Moved the SD folder ") + brand::kOldDataDir + " to " DATA_DIR ".", ui::SYS);
  } else if (moved == Migration::Failed) {
    view.add(String("Couldn't rename ") + brand::kOldDataDir + " to " DATA_DIR " on the SD card.", ui::ERR);
  }
  if (sdOk) historyBytes = prompts::load(promptHistory);
  if (!micOk) view.add("Mic failed to start: " + micErr, ui::ERR);
  if (!sdOk) view.add("No SD card: voice and saved chats need one.", ui::SYS);
  loadSettings();
  cryptoOk = agent::selfTest();

  if (cfg.anthropicKey.isEmpty()) {
    bootStep("ready", kBootSteps);
    delay(300);
    view.add("No Anthropic API key yet, so let's set up from your phone.", ui::SYS);
    view.add("Or put ANTHROPIC_API_KEY=... in /keys.txt on the SD card.", ui::DIM);
    refreshHeader();
    openMenu(setupFromPhone);
  } else {
    bringUpNetwork();
  }
  Serial.printf("[boot] %s %s board=%d kb=%d mic=%d sd=%d cfg=%s wifi=%d time=%d groq=%d crypto=%d heap=%u\n",
                brand::kProduct, kVersion, boardId, kb->ok(), adv_audio::codecPresent(), sdOk, cfg.source.c_str(),
                WiFi.status() == WL_CONNECTED, timeOk, cfg.groqKey.length() > 0, cryptoOk, ESP.getFreeHeap());
  view.add(cfg.groqKey.length() ? "Type or hold G0 to talk. Tab: menu." : "Ask me anything. Tab opens the menu.",
           ui::DIM);
  if (!view.shown()) {
    bootStep("ready", kBootSteps);
    delay(300);  // let the full bar register before the chat replaces it
  }
  refreshHeader();
  view.show();
  view.setInput("");
}

void loop() {
  const KeyEvent ev = pollKeys(input);  // also updates button state
  if (M5Cardputer.BtnA.wasPressed()) doVoice(view, input);
  if (ev.tab) openMenu(mainMenu);
  if (ev.left) view.scroll(3);
  if (ev.right) view.scroll(-3);
  if (ev.up || ev.down) {
    String recalled;
    if (ev.up ? promptHistory.older(input, recalled) : promptHistory.newer(recalled)) {
      input = recalled;
      view.setInput(input);
    }
  }
  if (ev.esc && input.length()) {
    promptHistory.stop();
    input = "";
    view.setInput(input);
  }
  if (ev.changed) {
    promptHistory.stop();
    view.setInput(input);
  }
  if (ev.enter) {
    String line = input;
    line.trim();
    if (promptHistory.add(line) && sdOk) prompts::append(promptHistory, line, historyBytes);
    input = "";
    view.setInput(input);
    if (line.startsWith("/")) handleCommand(line);
    else if (line.length()) sendPrompt(line);
  }
  if (millis() - lastHeaderMs > 5000) {
    lastHeaderMs = millis();
    refreshHeader();
  }
  delay(5);
}
