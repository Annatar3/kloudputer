// Kloudputer — Claude chat on the M5Stack Cardputer ADV, with push-to-talk voice and spoken answers.
// Type or hold G0 and talk; Enter sends. Tab opens the menu. Fn+; / Fn+. scroll. Commands: /help.
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <functional>
#include <iterator>
#include <vector>
#include "agent_proto.h"
#include "agent_relay.h"
#include "anthropic.h"
#include "config.h"
#include "keyboard_adv.h"
#include "keycheck.h"
#include "menu.h"
#include "net.h"
#include "portal.h"
#include "sessions.h"
#include "tts.h"
#include "ui.h"
#include "voice.h"

// The loop task runs every TLS handshake. From the agent screen, voice upload sits ~7 calls deep (menu, agents,
// agent screen, voice, Groq, TLS) and overflowed the default 8 KB stack. /info shows the measured headroom.
constexpr size_t kLoopStack = 16 * 1024;
size_t getArduinoLoopTaskStackSize() { return kLoopStack; }

namespace {
constexpr const char* kVersion = "v1.7";
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
  const char* price;  // per 1M tokens in/out
};
constexpr ModelOpt kModels[] = {
    {"claude-haiku-5-5", "fast, cheapest", "$0.1/$0.5"},
    {"claude-sonnet-5-5", "balanced", "$2/$10"},
    {"claude-opus-5-5", "smartest Opus", "$4/$20"},
    {"claude-fable-5-1", "most capable", "$10/$50"},
};

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
  int scroll = 0;
};

// The keyboard state if a key just went down, else nullptr. Also refreshes button state (G0).
const Keyboard_Class::KeysState* nextKeys() {
  M5Cardputer.update();
  M5Cardputer.Keyboard.updateKeyList();
  M5Cardputer.Keyboard.updateKeysState();
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return nullptr;
  return &M5Cardputer.Keyboard.keysState();
}

// Applies one keyboard change to `buf`; returns what happened.
KeyEvent pollKeys(String& buf) {
  KeyEvent ev;
  const auto* st = nextKeys();
  if (!st) return ev;
  if (st->up) ev.scroll = 3;
  if (st->down) ev.scroll = -3;
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
  setenv("TZ", cfg.tz.c_str(), 1);  // dates in the Sessions list, before a clock sync sets it again
  tzset();
}

void chooseModel() {
  const int n = std::size(kModels);
  for (int i = 0; i < n; ++i) {
    const bool cur = cfg.model == kModels[i].id;
    view.add(String(i + 1) + ") " + shortModel(kModels[i].id) + (cur ? " *" : "") + "  " + kModels[i].desc + "  " +
                 kModels[i].price,
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

// ---------- wifi ----------
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

// ---------- spoken answers ----------
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
  if (cfg.groqKey.isEmpty() || WiFi.status() != WL_CONNECTED) {
    voiceError(cfg.groqKey.isEmpty() ? "needs a Groq key (/setup)" : "offline", v);
    return;
  }
  const std::vector<String> parts = tts::chunks(text);
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

// ---------- chat ----------
void trimHistory() { sessions::trimHistory(history, kMaxHistoryMsgs, kMaxHistoryChars); }

String titleOf(const sessions::Meta& m) { return m.title.length() ? m.title : String("(untitled)"); }

// Every finished turn goes to the chat's file on the SD card, which the first turn creates. When the
// card is missing or refuses, chatting carries on in RAM and the error shows once, not every turn.
void saveTurn(const String& prompt, const String& reply) {
  if (sdOk && session.id.isEmpty()) {
    const time_t now = time(nullptr);
    session.id = sessions::nextId(timeOk ? sessions::stampOf(now) : String(), sessions::newestId());
    session.title = sessions::titleFrom(prompt);
    session.model = cfg.model;
    session.created = timeOk ? now : 0;
  }
  if (sdOk && sessions::append(session, prompt, reply)) {
    saveWarned = false;
    return;
  }
  if (!saveWarned) view.add(sdOk ? "Couldn't save this chat to the SD card." : "No SD card, so chats aren't saved.", ui::ERR);
  saveWarned = true;
}

// Rebuilds the chat from a saved session; new turns then go on in the same file. The model in use
// stays as it is, with a note when the session was saved under another one.
void resume(const String& id) {
  sessions::Meta meta;
  history.clear();
  view.clear();
  const bool ok = sessions::load(id, meta, [](const String& prompt, const String& reply) {
    view.addUser(prompt);
    view.addClaude(reply);
    history.push_back({true, prompt});
    history.push_back({false, reply});
    trimHistory();
  });
  if (!ok) {
    session = sessions::Meta();
    view.add("Couldn't read that chat from the SD card.", ui::ERR);
    return;
  }
  session = meta;
  view.add("Resumed: " + titleOf(meta), ui::DIM);
  if (meta.model.length() && meta.model != cfg.model) {
    view.add("Saved with " + modelName(meta.model) + "; still using " + modelName(cfg.model) + ".", ui::DIM);
  }
}

void sendPrompt(const String& text) {
  view.addUser(text);
  if (cfg.anthropicKey.isEmpty()) {
    view.add("No Anthropic API key. Type /setup to add one, or chat via Tab > Agents (Claude Code, Pro login).", ui::ERR);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    view.add("Offline. Type /wifi to connect.", ui::ERR);
    return;
  }
  history.push_back({true, text});
  trimHistory();
  view.thinking();
  Serial.printf("[chat] -> %u chars, %u msgs, heap=%u maxblk=%u\n", text.length(), history.size(),
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const ChatResult r = anthropicChat(cfg.anthropicKey, cfg.model, history, voiceReplies);
  Serial.printf("[chat] <- ok=%d %ums in=%d out=%d heap=%u err=%s\n", r.ok, r.ms, r.inTok, r.outTok,
                ESP.getFreeHeap(), r.error.c_str());
  if (r.ok) {
    history.push_back({false, r.text});
    view.addClaude(r.text);
    saveTurn(text, r.text);
    if (voiceReplies) speak(r.text);
    header.setNote(String(r.ms / 100 / 10.0, 1) + "s " + r.inTok + "/" + r.outTok + " tok", ui::DIM);
  } else {
    history.pop_back();  // keep user/assistant turns paired
    view.add(r.error, ui::ERR);
    refreshHeader();
  }
  lastHeaderMs = millis();
}

// ---------- voice ----------
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
  if (cfg.groqKey.isEmpty()) {
    v.add("Voice needs a Groq key. Type /setup to add one.", ui::ERR);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    v.add("Offline. Type /wifi to connect.", ui::ERR);
    return;
  }
  v.setInput("", "(recording - release G0) ");
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
  const voice::Transcript t = voice::transcribe(cfg.groqKey);
  Serial.printf("[voice] stt ok=%d %ums text=%s err=%s heap=%u\n", t.ok, t.ms, t.text.c_str(), t.error.c_str(),
                ESP.getFreeHeap());
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
  header.setNote("heard " + String(rec.ms / 1000.0, 1) + "s L" + String(rec.rmsL, 0) + " R" +
                     String(rec.rmsR, 0) + " - Enter",
                 ui::GREEN);
}

// ---------- commands ----------
void showHelp() {
  view.add("Type or hold G0 to talk. Enter sends.", ui::DIM);
  view.add("Tab    menu: sessions, voice, model", ui::DIM);
  view.add("Fn+; / Fn+.  scroll   Fn+`  clear", ui::DIM);
  view.add("Menus: ; . move  , / change  Del back", ui::DIM);
  view.add("/new    start a fresh conversation", ui::DIM);
  view.add("/sessions  saved chats (x deletes)", ui::DIM);
  view.add("/setup  Wi-Fi + keys from a phone", ui::DIM);
  view.add("/pair NAME CODE  pair an agent", ui::DIM);
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
  if (on && cfg.groqKey.isEmpty()) {
    view.add("Voice needs a Groq key. Type /setup to add one.", ui::ERR);
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
  view.add("claude key " + (cfg.anthropicKey.length() ? cfg.anthropicFrom : String("MISSING")) + "  groq key " +
               (cfg.groqKey.length() ? cfg.groqFrom : String("-")),
           ui::DIM);
  view.add(String("voice ") + (voiceReplies ? "on" : "off") + " " + voiceName + "  vol " + String(volume) + "/10",
           ui::DIM);
  view.add("agents " + String(static_cast<int>(loadAgents().size())) + " paired  crypto " + (cryptoOk ? "ok" : "FAIL"),
           cryptoOk ? ui::DIM : ui::ERR);
}

// ---------- menus ----------
constexpr const char* kNavHint = "Arrows move  Enter pick  Del back";
constexpr const char* kSessionsHint = "Enter resume   x delete   Del back";

enum class Nav : uint8_t { None, Up, Down, Left, Right, Pick, Back, Delete };

bool typed(const Keyboard_Class::KeysState& st, char c) {
  return std::find(st.word.begin(), st.word.end(), c) != st.word.end();
}

// Menus take no text, so the bare ; . , / keys (they carry the arrows) navigate as well as Fn+.
Nav readNav() {
  const auto* st = nextKeys();
  if (!st) return Nav::None;
  if (st->up || typed(*st, ';')) return Nav::Up;
  if (st->down || typed(*st, '.')) return Nav::Down;
  if (st->left || typed(*st, ',')) return Nav::Left;
  if (st->right || typed(*st, '/')) return Nav::Right;
  if (st->enter) return Nav::Pick;
  if (st->backspace || st->del || st->tab || st->esc || typed(*st, '`')) return Nav::Back;
  if (typed(*st, 'x') || typed(*st, 'X')) return Nav::Delete;
  return Nav::None;
}

struct Choice {
  Nav nav;  // Pick, Back, or Delete
  int row;
};

// Runs the open menu until Enter, back, or (in a list that `deletes`) x. Left/right go to `adjust`.
Choice navigate(void (*adjust)(int row, int dir) = nullptr, bool deletes = false) {
  while (true) {
    const Nav nav = readNav();
    switch (nav) {
      case Nav::Up: menu.move(-1); break;
      case Nav::Down: menu.move(1); break;
      case Nav::Left: if (adjust) adjust(menu.selected(), -1); break;
      case Nav::Right: if (adjust) adjust(menu.selected(), 1); break;
      case Nav::Delete: if (deletes) return {nav, menu.selected()}; break;
      case Nav::Pick: case Nav::Back: return {nav, menu.selected()};
      case Nav::None: break;
    }
    if (millis() - lastHeaderMs > 5000) {
      lastHeaderMs = millis();
      refreshHeader();
    }
    delay(5);
  }
}

uint32_t menuSeen = 0;  // chat messages already echoed in a menu footer

// Draws a menu. Actions report in the chat, so the newest message since the last draw shows in the
// footer: you see what happened without leaving the menu.
void showMenu(const char* title, std::vector<ui::MenuItem> items, int row, const char* hint = kNavHint) {
  menu.open(title, std::move(items), row, hint);
  if (view.added() != menuSeen) menu.notice(view.lastText(), view.lastColor());
  menuSeen = view.added();
}

// What a menu leaves to run once the chat is back on screen; an empty one keeps the menu open.
using Then = std::function<void()>;
const Then kToChat = [] {};
template <void (*Action)()>
Then inChat() { return Action; }
template <void (*Action)()>
Then inMenu() {
  Action();
  return {};
}

int currentVoice() {
  for (size_t i = 0; i < std::size(tts::kVoices); ++i) {
    if (tts::kVoices[i] == voiceName) return i;
  }
  return 0;
}

void voiceMenu() {
  int row = voiceReplies ? 1 + currentVoice() : 0;
  while (true) {
    std::vector<ui::MenuItem> items{{"Spoken answers", voiceReplies ? "on" : "off", "Enter turns them on / off   Del back"}};
    for (size_t i = 0; i < std::size(tts::kVoices); ++i) {
      String name = tts::kVoices[i];
      name.setCharAt(0, toupper(name[0]));
      const bool cur = tts::kVoices[i] == voiceName;
      items.push_back({name, String(kVoiceKinds[i]) + (cur ? "  *" : ""), "Enter: hear it and keep it   Del back"});
    }
    showMenu("Voice", std::move(items), row);
    const Choice c = navigate();
    if (c.nav == Nav::Back) return;
    row = c.row;
    voiceCommand(row == 0 ? (voiceReplies ? "off" : "on") : tts::kVoices[row - 1]);
  }
}

void modelMenu() {
  std::vector<ui::MenuItem> items;
  int row = 0;
  for (size_t i = 0; i < std::size(kModels); ++i) {
    const bool cur = cfg.model == kModels[i].id;
    if (cur) row = i;
    items.push_back({modelName(kModels[i].id), String(kModels[i].price) + (cur ? "  *" : ""),
                     String(kModels[i].desc) + "  ($/1M tokens in/out)"});
  }
  showMenu("Model", std::move(items), row);
  const Choice c = navigate();
  if (c.nav == Nav::Pick) setModel(kModels[c.row].id);
}

String savedCount() { return sdOk ? String(sessions::count()) + " saved" : String("no SD card"); }

// After a first x, deletes only if the very next key is x too; any other key cancels.
bool confirmDelete() {
  menu.notice("Press x again to delete", ui::ERR);
  while (true) {
    if (const auto* st = nextKeys()) {
      const bool yes = typed(*st, 'x') || typed(*st, 'X');
      if (!yes) menu.notice("", ui::DIM);
      return yes;
    }
    delay(5);
  }
}

// Saved chats, newest first. Enter resumes one (rebuilt while the menu still has the screen).
Then sessionsMenu() {
  int row = 0;
  while (true) {
    const sessions::Listing found = sdOk ? sessions::list(kMaxSessions) : sessions::Listing();
    const time_t now = timeOk ? time(nullptr) : 0;
    std::vector<ui::MenuItem> items;
    for (const sessions::Meta& m : found.items) {
      items.push_back({titleOf(m), sessions::when(m.created, now) + (m.id == session.id ? " *" : "")});
    }
    if (items.empty()) items.push_back({sdOk ? "No saved chats yet" : "No SD card", ""});
    showMenu("Sessions", std::move(items), row, kSessionsHint);
    while (true) {
      const Choice c = navigate(nullptr, true);
      if (c.nav == Nav::Back) return {};
      if (found.items.empty()) continue;
      const sessions::Meta& m = found.items[c.row];
      if (c.nav == Nav::Pick) {
        resume(m.id);
        return kToChat;
      }
      if (!confirmDelete()) continue;
      if (sessions::remove(m.id)) {
        if (m.id == session.id) session = sessions::Meta();
        view.add("Deleted: " + titleOf(m), ui::SYS);
      } else {
        view.add("Couldn't delete " + titleOf(m), ui::ERR);
      }
      row = c.row;
      break;
    }
  }
}

// ---------- setup from a phone ----------
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
    if (cfg.groqKey.length()) reportKey("Groq", keycheck::groq(cfg.groqKey));
  }
  refreshHeader();
}

// Phone setup takes the screen (its own Wi-Fi and page); the outcome is handled back in the chat.
Then setupFromPhone() {
  phone::Current now = phone::currentFrom(currentSsid(), cfg.anthropicKey, cfg.groqKey, cfg.model, cfg.tz);
  for (const AgentEntry& a : loadAgents()) now.agents.push_back({a.name, a.relay});
  const portal::Outcome o = portal::run(M5Cardputer.Display, header, {now, modelChoices(), setupKey});
  return [o] { afterSetup(o); };
}

// ---------- agents ----------
// What the open agent is doing, as its header shows it.
enum class AgentState : uint8_t { Connecting, Idle, Working, Done, Error, Offline };

const char* stateName(AgentState s) {
  switch (s) {
    case AgentState::Connecting: return "connecting";
    case AgentState::Idle: return "idle";
    case AgentState::Working: return "working";
    case AgentState::Done: return "done";
    case AgentState::Error: return "error";
    default: return "offline";
  }
}

uint16_t stateColor(AgentState s) {
  return s == AgentState::Working ? ui::ORANGE : s == AgentState::Done ? ui::GREEN
       : s == AgentState::Error || s == AgentState::Offline ? ui::ERR : ui::DIM;
}

// The agent screen's link: keys, the relay connection, and the job in progress.
struct AgentLink {
  AgentEntry entry;
  agent::Keys keys;
  agent::Relay relay;
  agent::ReplayGuard guard;
  agent::Report report;
  String job;           // the latest job; its late messages still count after done
  uint32_t n = 0;       // our counter within the job
  uint32_t loose = 0;   // our counter outside jobs (ping); starts at the clock so it rises across reboots
  AgentState state = AgentState::Connecting;
  bool cancelArmed = false;
  bool reported = false;
};

bool sendUp(AgentLink& a, agent::Kind kind, const String& job, uint32_t n, const String& text = String()) {
  header.setNote("sending...", ui::SYS);
  const String body = agent::seal(a.keys.keyUp, a.keys.topicUp, agent::buildUp(kind, job, n, time(nullptr), text));
  if (body.length() && a.relay.publish(a.keys.topicUp, body)) return true;
  agentView.add("Couldn't send: " + a.relay.error(), ui::ERR);
  return false;
}

void showReport(AgentLink& a) {
  a.reported = true;
  const String text = a.report.text();
  a.report.reset();
  agentView.addClaude(text);
  if (voiceReplies) {
    a.relay.pause();  // speech opens its own TLS session; one at a time
    speak(text, agentView);
  }
}

// One message off the relay. Anything that fails a check is dropped silently, as the protocol says.
void onAgentMessage(AgentLink& a, const String& body) {
  String plain;
  agent::Message m;
  if (!agent::open(a.keys.keyDown, a.keys.topicDown, body, plain) || !agent::parse(plain, m) ||
      !a.guard.accept(m.job, m.n, m.ts, time(nullptr)) || (m.job.length() && m.job != a.job)) {
    return;
  }
  switch (m.kind) {
    case agent::Kind::Hello:
      agentView.add((m.name.length() ? m.name : a.entry.name) + (m.busy ? " is busy with a job." : " is ready.") +
                        (m.version.length() ? " (" + m.version + ")" : String()),
                    ui::DIM);
      if (m.busy) a.state = AgentState::Working;
      else if (a.state == AgentState::Connecting) a.state = AgentState::Idle;
      break;
    case agent::Kind::Ack:
      a.state = AgentState::Working;
      agentView.placeholder("Started.", ui::DIM, false);
      break;
    case agent::Kind::Step:
      agentView.placeholder(m.text, ui::DIM, false);
      break;
    case agent::Kind::Approval:  // answering approvals from here is phase B
      agentView.add(a.entry.name + " asks to use " + m.tool + ": " + m.detail +
                        ". Approving from the Cardputer comes later; unanswered, it's denied in 5 minutes.",
                    ui::SYS);
      break;
    case agent::Kind::Part:
      if (a.report.add(m.i, m.of, m.text) && a.report.complete() && !a.reported) showReport(a);
      break;
    case agent::Kind::Done:
      if (!a.reported && a.report.have()) {
        agentView.add("Only " + String(a.report.have()) + " of " + String(a.report.of()) + " report parts arrived:", ui::SYS);
        showReport(a);
      }
      a.state = m.ok ? AgentState::Done : AgentState::Error;
      agentView.add(String(m.ok ? "Done" : "Stopped") + " after " + String(m.secs, 0) + " s" +
                        (m.cost > 0 ? ", $" + String(m.cost, 2) : String()) + ".",
                    m.ok ? ui::DIM : ui::SYS);
      break;
    case agent::Kind::Error:
      agentView.add(m.text, ui::ERR);
      a.state = AgentState::Error;
      break;
    default:
      break;
  }
}

// The agent screen: type or speak a task, watch its steps, read (and hear) the report. Tab, or Del on
// an empty line, goes back to the list. The relay is only streamed while this screen is open.
void agentScreen(const AgentEntry& entry) {
  AgentLink a;
  a.entry = entry;
  uint8_t secret[32];
  agent::RelayUrl url;
  const bool paired = agent::parsePairingCode(entry.code, secret) && agent::deriveKeys(secret, a.keys) &&
                      agent::parseRelay(entry.relay.length() ? entry.relay : agent::kDefaultRelay, url);
  memset(secret, 0, sizeof secret);
  if (agentViewOf != entry.name) {
    agentView.clear();
    agentInput = "";
    agentViewOf = entry.name;
  }
  agentView.setTitle(entry.name);
  agentView.show();
  agentView.setInput(agentInput);
  if (!paired) {
    agentView.add("This agent's pairing is damaged. Pair it again from /setup.", ui::ERR);
    a.state = AgentState::Error;
  } else if (!timeOk || WiFi.status() != WL_CONNECTED) {
    agentView.add("Agents need Wi-Fi and the clock (messages carry the time). /wifi to connect.", ui::ERR);
    a.state = AgentState::Offline;
  } else {
    const uint32_t now = time(nullptr);
    a.loose = now;
    a.relay.begin(url, a.keys.topicDown, now - 5);  // a little early, for clock skew against the relay
    sendUp(a, agent::Kind::Ping, "", ++a.loose);
  }

  String noteShown;
  while (true) {
    for (String body; a.relay.poll(body);) onAgentMessage(a, body);
    const bool waiting = a.relay.state() == agent::Relay::State::Waiting;
    const AgentState shown = waiting ? AgentState::Offline : a.state;
    const String note = waiting ? "offline: " + a.relay.error() : String(stateName(shown));
    if (note != noteShown) {
      noteShown = note;
      header.setNote(note, stateColor(shown));
    }

    const bool wasEmpty = agentInput.isEmpty();
    const KeyEvent ev = pollKeys(agentInput);
    if (M5Cardputer.BtnA.wasPressed()) {
      a.relay.pause();  // Whisper needs TLS too; one session at a time
      doVoice(agentView, agentInput);
      noteShown = "";
    }
    if (ev.tab || (ev.back && wasEmpty)) break;
    if (ev.esc && a.state == AgentState::Working && a.job.length()) {
      if (a.cancelArmed) {
        a.cancelArmed = false;
        if (sendUp(a, agent::Kind::Cancel, a.job, ++a.n)) agentView.placeholder("Cancelling...", ui::DIM, false);
      } else {
        a.cancelArmed = true;
        agentView.placeholder("Press Esc again to cancel the job.", ui::SYS, false);
      }
    } else if (ev.esc && agentInput.length()) {
      agentInput = "";
      agentView.setInput(agentInput);
    } else if (ev.changed || ev.enter) {
      a.cancelArmed = false;
    }
    if (ev.scroll) agentView.scroll(ev.scroll);
    if (ev.changed) agentView.setInput(agentInput);
    if (ev.enter) {
      String text = agentInput;
      text.trim();
      if (text.isEmpty()) {
      } else if (a.state == AgentState::Working) {
        agentView.add("A job is running. Esc twice cancels it.", ui::SYS);
      } else if (paired && a.relay.state() != agent::Relay::State::Off) {
        a.job = agent::newJobId();
        a.n = 0;
        a.report.reset();
        a.reported = false;
        agentView.addUser(text);
        agentInput = "";
        agentView.setInput(agentInput);
        if (sendUp(a, agent::Kind::Ask, a.job, ++a.n, text)) {
          a.state = AgentState::Working;
          agentView.placeholder("Sent. Waiting for " + entry.name + "...", ui::DIM, false);
        }
        noteShown = "";
      }
    }
    delay(5);
  }
  a.relay.stop();
  agentView.hide();
}

String agentsValue() {
  const size_t n = loadAgents().size();
  return n ? String(static_cast<int>(n)) + " paired" : String("none yet");
}

void pairHelp() {
  view.add("Pair an agent with the code its connector prints (cp1-...):", ui::DIM);
  view.add("- on your phone: Tab > Setup from phone, Agents", ui::DIM);
  view.add("- or here: /pair NAME CODE", ui::DIM);
}

// The paired agents; Enter opens one. With none yet, the ways to pair one.
Then agentsMenu() {
  int row = 0;
  while (true) {
    const std::vector<AgentEntry> agents = loadAgents();
    if (agents.empty()) {
      showMenu("Agents", {{"Pair from your phone", "/setup"}, {"How to pair", ""}}, row,
               "Codes come from the agent's connector");
      const Choice c = navigate();
      if (c.nav == Nav::Back) return {};
      return c.row == 0 ? setupFromPhone() : inChat<pairHelp>();
    }
    std::vector<ui::MenuItem> items;
    for (const AgentEntry& a : agents) items.push_back({a.name, phone::relayHost(a.relay)});
    showMenu("Agents", std::move(items), row, "Enter opens   Del back");
    const Choice c = navigate();
    if (c.nav == Nav::Back) return {};
    row = c.row;
    agentScreen(agents[row]);
  }
}

// /pair NAME CODE: pairing without the phone. The code is never shown back.
void pairCommand(const String& arg) {
  const int sp = arg.lastIndexOf(' ');
  String name = sp > 0 ? arg.substring(0, sp) : String();
  name.trim();
  const String code = sp > 0 ? arg.substring(sp + 1) : arg;
  String error = phone::validateAgent(name, code, "");
  std::vector<AgentEntry> agents = loadAgents();
  const bool repair = std::any_of(agents.begin(), agents.end(), [&name](const AgentEntry& a) { return a.name == name; });
  if (error.isEmpty() && !repair && static_cast<int>(agents.size()) >= kMaxAgents) error = "There's room for 4 agents.";
  if (error.length()) {
    view.add(error + " Usage: /pair NAME CODE", ui::ERR);
    return;
  }
  editAgents(agents, {}, {name, code, ""});
  if (saveAgents(agents)) view.add(String(repair ? "Re-paired " : "Paired ") + name + ". Open it from Tab > Agents.", ui::GREEN);
  else view.add("Couldn't store the agent. Try again.", ui::ERR);
}

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
       if (cfg.groqKey.isEmpty()) return "needs Groq key";
       return voiceReplies ? "on - " + String(voiceName) : String("off");
     },
     inMenu<voiceMenu>, nullptr, nullptr},
    {"Volume", []() -> String { return "< " + String(volume) + " >"; }, nullptr,
     [](int dir) { applyVolume(std::min(std::max(volume + dir, 1), 10)); }, "Left / right change it   Del back"},
    {"Model", []() -> String { return modelName(cfg.model); }, inMenu<modelMenu>, nullptr, nullptr},
    {"Wi-Fi", []() -> String { return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("offline"); },
     inChat<changeWifi>, nullptr, "Enter scans for networks   Del back"},
    {"Setup from phone", nullptr, setupFromPhone, nullptr, "Enter: Wi-Fi and keys from a phone"},
    {"Mic test", nullptr, inChat<micTest>, nullptr, nullptr},
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
  if (!micOk) view.add("Mic failed to start: " + micErr, ui::ERR);
  if (!sdOk) view.add("No SD card: voice and saved chats need one.", ui::SYS);
  loadSettings();
  cryptoOk = agent::selfTest();

  if (cfg.anthropicKey.isEmpty() && loadAgents().empty()) {
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
  if (ev.scroll) view.scroll(ev.scroll);
  if (ev.esc && input.length()) {
    input = "";
    view.setInput(input);
  }
  if (ev.changed) view.setInput(input);
  if (ev.enter) {
    String line = input;
    line.trim();
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
