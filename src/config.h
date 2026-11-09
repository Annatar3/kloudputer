// Kloudputer — settings: what was set up from a phone (kept in NVS) outranks files on the SD card,
// which stay as a fallback. The SD files are /keys.txt, then /kloudputer/config.txt, which overrides it.
// Format: KEY=VALUE per line, '#' starts a comment line.
//   ANTHROPIC_API_KEY=sk-ant-...   GROQ_API_KEY=gsk_... (or GEMINI_API_KEY=AIza...)   WIFI_SSID=...   WIFI_PASS=...   MODEL=claude-haiku-5-5
//   TZ=<+04>-4   (POSIX time zone for dates on screen; UTC when absent)
#pragma once
#ifdef ARDUINO
#include <Preferences.h>
#include <SD.h>
#endif
#include <algorithm>
#include <vector>
#include "brand.h"

struct Config {
  String anthropicKey;
  String groqKey;
  String wifiSsid;
  String wifiPass;
  String model = "claude-haiku-5-5";
  String tz = "UTC0";
  String anthropicFrom;  // "phone" or the file that set it; empty when there's no key
  String groqFrom;
  String source;  // everywhere settings came from, e.g. "phone + /keys.txt"
};

// NVS namespace, shared with net.h and the menu's choices. Internal, so it keeps its old name: renaming it
// would forget every saved setting.
constexpr const char* kPrefs = "cputer";

inline void parseConfig(File& f, Config& c) {
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    const int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);
    k.trim();
    k.toUpperCase();
    String v = line.substring(eq + 1);
    v.trim();
    if (v.length() >= 2 && ((v[0] == '"' && v[v.length() - 1] == '"') ||
                            (v[0] == '\'' && v[v.length() - 1] == '\''))) {
      v = v.substring(1, v.length() - 1);
    }
    if (k == "ANTHROPIC_API_KEY") c.anthropicKey = v;
    else if (k == "GROQ_API_KEY" || k == "GEMINI_API_KEY") c.groqKey = v;  // the voice key: either provider
    else if (k == "WIFI_SSID") c.wifiSsid = v;
    else if (k == "WIFI_PASS" || k == "WIFI_PASSWORD") c.wifiPass = v;
    else if (k == "MODEL" || k == "CLAUDE_MODEL") c.model = v;
    else if (k == "TZ") c.tz = v;
  }
}

inline bool loadConfig(Config& c) {
  const char* paths[] = {"/keys.txt", DATA_DIR "/config.txt"};
  bool any = false;
  for (const char* p : paths) {
    if (!SD.exists(p)) continue;
    File f = SD.open(p);
    if (!f) continue;
    const String anthropic = c.anthropicKey, groq = c.groqKey;
    parseConfig(f, c);
    f.close();
    if (c.anthropicKey != anthropic) c.anthropicFrom = p;
    if (c.groqKey != groq) c.groqFrom = p;
    if (c.source.length()) c.source += " + ";
    c.source += p;
    any = true;
  }
  return any;
}

// Applies what was saved on the Cardputer over the SD files: keys and time zone from phone setup, and
// the model, which phone setup and the Model menu both save.
inline void loadPhone(Config& c) {
  Preferences p;
  if (!p.begin(kPrefs, true)) return;
  auto saved = [&p](const char* key) { return p.isKey(key) ? p.getString(key, "") : String(); };
  bool fromPhone = false;
  if (const String v = saved("akey"); v.length()) {
    c.anthropicKey = v;
    c.anthropicFrom = "phone";
    fromPhone = true;
  }
  if (const String v = saved("gkey"); v.length()) {
    c.groqKey = v;
    c.groqFrom = "phone";
    fromPhone = true;
  }
  if (const String v = saved("tz"); v.length()) {
    c.tz = v;
    fromPhone = true;
  }
  if (const String v = saved("model"); v.length()) c.model = v;
  p.end();
  if (fromPhone) c.source = c.source.length() ? "phone + " + c.source : String("phone");
}

// Stores what the phone sent; an empty field keeps what's there. False if NVS refused.
inline bool savePhone(const String& anthropicKey, const String& groqKey, const String& model, const String& tz) {
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const struct {
    const char* key;
    const String& value;
  } fields[] = {{"akey", anthropicKey}, {"gkey", groqKey}, {"model", model}, {"tz", tz}};
  bool ok = true;
  for (const auto& f : fields) {
    if (f.value.length()) ok = p.putString(f.key, f.value) > 0 && ok;
  }
  p.end();
  return ok;
}

// ---------- paired agents ----------
constexpr int kMaxAgents = 4;

struct AgentEntry {
  String name;
  String code;   // the pairing code (cp1-...): the agent's secret, never shown or logged
  String relay;  // empty: the default relay
};

inline String agentKey(int slot, char field) { return String("ag") + slot + field; }

inline std::vector<AgentEntry> loadAgents() {
  std::vector<AgentEntry> list;
  Preferences p;
  if (!p.begin(kPrefs, true)) return list;
  for (int i = 0; i < kMaxAgents; ++i) {
    if (!p.isKey(agentKey(i, 'n').c_str())) break;
    list.push_back({p.getString(agentKey(i, 'n').c_str(), ""), p.getString(agentKey(i, 'c').c_str(), ""),
                    p.isKey(agentKey(i, 'r').c_str()) ? p.getString(agentKey(i, 'r').c_str(), "") : String()});
  }
  p.end();
  return list;
}

// Rewrites every slot, so a removal leaves no gap and no stale secret behind.
inline bool saveAgents(const std::vector<AgentEntry>& list) {
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  bool ok = true;
  for (int i = 0; i < kMaxAgents; ++i) {
    for (char field : {'n', 'c', 'r'}) {
      const String key = agentKey(i, field);
      const AgentEntry* a = i < static_cast<int>(list.size()) ? &list[i] : nullptr;
      const String& value = !a ? String() : field == 'n' ? a->name : field == 'c' ? a->code : a->relay;
      if (value.length()) ok = p.putString(key.c_str(), value) > 0 && ok;
      else if (p.isKey(key.c_str())) p.remove(key.c_str());
    }
  }
  p.end();
  return ok;
}

// Removes the ticked agents, then adds the new one, or re-pairs it when an agent of that name remains.
inline void editAgents(std::vector<AgentEntry>& list, const std::vector<int>& remove, const AgentEntry& add) {
  std::vector<AgentEntry> kept;
  for (int i = 0; i < static_cast<int>(list.size()); ++i) {
    if (std::find(remove.begin(), remove.end(), i) == remove.end()) kept.push_back(list[i]);
  }
  if (add.name.length()) {
    auto same = std::find_if(kept.begin(), kept.end(), [&add](const AgentEntry& a) { return a.name == add.name; });
    if (same != kept.end()) *same = add;
    else kept.push_back(add);
  }
  list = kept;
}

// The SD folder had the product's old name before the rename. Moved once: only while the old folder
// exists and the new one doesn't.
enum class Migration : uint8_t { None, Moved, Failed };

inline Migration migrateDataDir() {
  if (!SD.exists(brand::kOldDataDir) || SD.exists(DATA_DIR)) return Migration::None;
  return SD.rename(brand::kOldDataDir, DATA_DIR) ? Migration::Moved : Migration::Failed;
}
