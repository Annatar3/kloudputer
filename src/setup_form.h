// Kloudputer — the page a phone fills in during setup, and reading what it sends back.
// Pure (no radio, no server), so all of it runs in the host tests.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>
#include "agent_proto.h"
#include "brand.h"
#include "config.h"
#include "text.h"

namespace phone {
constexpr size_t kMaxBody = 4096;
constexpr size_t kMaxField = 256;
constexpr const char* kOther = "__other__";  // the network select's "type a name" choice
// The setup network's address, in TEST-NET-1 (192.0.2.0/24: never used on the internet). Not 192.168.x.x:
// every DNS answer would then be a private address, and Android's NetworkMonitor (with its private-IP
// rule on) calls such a network "no internet" without probing it, so no sign-in prompt ever appears.
constexpr uint8_t kApIp[4] = {192, 0, 2, 1};
constexpr const char* kApHost = "192.0.2.1";
constexpr const char* kPageUrl = "http://192.0.2.1/";

struct Form {
  String ssid;  // empty keeps the current network
  String wifiPass;
  String anthropicKey;  // empty keeps the stored key
  String groqKey;
  String model;
  String tz;
  String agentName;  // a new or re-paired agent; empty adds none
  String agentCode;
  String agentRelay;
  std::vector<int> removeAgents;  // indices of agents to remove
};

struct Choice {
  String id;
  String name;
};

struct AgentRow {
  String name;
  String relay;  // shown as its host; the pairing code never reaches the page
};

// All the page may know about the current settings: keys only as their last 4 characters.
struct Current {
  String wifiSsid;
  bool hasAnthropic = false;
  String anthropicTail;
  bool hasGroq = false;
  String groqTail;
  String model;
  String tz;
  std::vector<AgentRow> agents;
};

inline String tail(const String& key) { return key.length() >= 12 ? key.substring(key.length() - 4) : String(); }

inline Current currentFrom(const String& ssid, const String& anthropicKey, const String& groqKey,
                           const String& model, const String& tz) {
  Current c;
  c.wifiSsid = ssid;
  c.hasAnthropic = anthropicKey.length() > 0;
  c.anthropicTail = tail(anthropicKey);
  c.hasGroq = groqKey.length() > 0;
  c.groqTail = tail(groqKey);
  c.model = model;
  c.tz = tz;
  return c;
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Form encoding: '+' is a space and %XX a byte; a malformed escape is kept as it was typed.
inline String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    const int hi = c == '%' && i + 2 < s.length() ? hexValue(s[i + 1]) : -1;
    const int lo = hi >= 0 ? hexValue(s[i + 2]) : -1;
    if (c == '+') {
      out += ' ';
    } else if (lo >= 0) {
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

struct Field {
  const char* name;  // form field name
  const char* label;
  String Form::*value;
};
constexpr Field kFields[] = {
    {"ssid", "The Wi-Fi name", &Form::ssid},       {"pass", "The Wi-Fi password", &Form::wifiPass},
    {"akey", "The Anthropic key", &Form::anthropicKey}, {"gkey", "The Groq key", &Form::groqKey},
    {"model", "The model", &Form::model},           {"tz", "The time zone", &Form::tz},
    {"aname", "The agent name", &Form::agentName},  {"acode", "The pairing code", &Form::agentCode},
    {"arelay", "The relay", &Form::agentRelay},
};

inline Form parseForm(const String& body) {
  Form f;
  String other;
  for (int start = 0; start <= static_cast<int>(body.length());) {
    int amp = body.indexOf('&', start);
    if (amp < 0) amp = body.length();
    const String pair = body.substring(start, amp);
    const int eq = pair.indexOf('=');
    const String key = urlDecode(eq < 0 ? pair : pair.substring(0, eq));
    const String value = eq < 0 ? String() : urlDecode(pair.substring(eq + 1));
    if (key == "other") other = value;
    if (key == "aremove" && value.length() == 1 && isdigit(static_cast<unsigned char>(value[0]))) {
      f.removeAgents.push_back(value[0] - '0');
    }
    for (const Field& field : kFields) {
      if (key == field.name) f.*field.value = value;
    }
    start = amp + 1;
  }
  if (f.ssid == kOther) f.ssid = other;
  f.anthropicKey.trim();  // pasted keys often carry a stray space or newline
  f.groqKey.trim();
  f.tz.trim();
  f.agentName.trim();
  f.agentCode.trim();
  f.agentRelay.trim();
  return f;
}

// Collects a request body up to kMaxBody; anything past that marks it oversized instead of growing.
struct Body {
  String text;
  bool oversized = false;
  void clear() {
    text = String();
    text.reserve(kMaxBody);
    oversized = false;
  }
  void add(const uint8_t* data, size_t n) {
    const size_t room = kMaxBody - std::min<size_t>(text.length(), kMaxBody);
    if (n > room) oversized = true;
    for (size_t i = 0; i < std::min(n, room); ++i) text += static_cast<char>(data[i]);
  }
};

inline bool printable(const String& s, bool spaces) {
  for (size_t i = 0; i < s.length(); ++i) {
    const uint8_t c = s[i];
    if (c < 0x20 || c == 0x7F || (!spaces && c == ' ')) return false;
  }
  return true;
}

// "" when an agent can be paired with these, else what to fix. The phone page and /pair both use it.
inline String validateAgent(const String& name, const String& code, const String& relay) {
  uint8_t secret[32];
  agent::RelayUrl url;
  if (name.isEmpty() || code.isEmpty()) return "Give the agent a name and paste its pairing code.";
  if (name.length() > 24 || !printable(name, true)) return "Agent names are up to 24 plain characters.";
  if (!agent::parsePairingCode(code, secret)) return "That pairing code doesn't look right: it's cp1- and 43 more characters.";
  if (relay.length() && !agent::parseRelay(relay, url)) return "The relay should be a URL like https://ntfy.sh";
  return "";
}

// "" when the form can be saved, else what to fix. Keys are only checked for shape here; the
// Cardputer tries them against the real APIs once it's back online.
inline String validate(const Form& f, bool haveAnthropicKey, const std::vector<Choice>& models,
                       const std::vector<AgentRow>& agents = {}) {
  for (const Field& field : kFields) {
    if ((f.*field.value).length() > kMaxField) return String(field.label) + " is too long.";
  }
  if (f.ssid.length() > 32) return "Wi-Fi names are at most 32 characters.";
  if (f.ssid.isEmpty() && f.wifiPass.length()) return "Choose the network that password is for.";
  if (f.wifiPass.length() && (f.wifiPass.length() < 8 || f.wifiPass.length() > 64)) {
    return "Wi-Fi passwords are 8 to 64 characters.";
  }
  if (!printable(f.ssid, true) || !printable(f.wifiPass, true)) return "The Wi-Fi name or password has odd characters.";
  if (!printable(f.anthropicKey, false) || !printable(f.groqKey, false)) return "A key has spaces or odd characters in it.";
  if (f.anthropicKey.length() && !f.anthropicKey.startsWith("sk-ant-")) return "The Anthropic key should start with sk-ant-";
  if (f.groqKey.length() && !f.groqKey.startsWith("gsk_")) return "The Groq key should start with gsk_";
  if (!haveAnthropicKey && f.anthropicKey.isEmpty()) return "Paste your Anthropic API key to finish setup.";
  if (f.model.length() && std::none_of(models.begin(), models.end(), [&f](const Choice& m) { return m.id == f.model; })) {
    return "Pick a model from the list.";
  }
  if (f.agentName.length() || f.agentCode.length() || f.agentRelay.length()) {
    const String error = validateAgent(f.agentName, f.agentCode, f.agentRelay);
    if (error.length()) return error;
    int kept = 0;
    bool repair = false;
    for (int i = 0; i < static_cast<int>(agents.size()); ++i) {
      if (std::find(f.removeAgents.begin(), f.removeAgents.end(), i) != f.removeAgents.end()) continue;
      ++kept;
      repair = repair || agents[i].name == f.agentName;
    }
    if (kept + !repair > kMaxAgents) return "There's room for 4 agents. Tick one to remove first.";
  }
  for (size_t i = 0; i < f.tz.length(); ++i) {
    const char c = f.tz[i];
    if (!isalnum(static_cast<unsigned char>(c)) && (c == '\0' || !strchr("<>+-:,./", c))) {
      return "That time zone doesn't look right. Example: <+04>-4";
    }
  }
  return "";
}

inline String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += s[i];
    }
  }
  return out;
}

// No external files: the phone has no internet while it's on the Cardputer's Wi-Fi.
constexpr const char* kHead = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>{{PRODUCT}} setup</title>
<style>
:root{--bg:#1a1918;--card:#262422;--line:#3a3633;--text:#f0eee6;--soft:#bfbaae;--dim:#8a857c;--accent:#d97757;--err:#e06c5a;--ok:#8db377}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;-webkit-text-size-adjust:100%}
main{max-width:30rem;margin:0 auto;padding:1.25rem 1rem 3rem}
header{display:flex;align-items:center;gap:.8rem;margin:.25rem 0 1.25rem}
header svg{width:3.2rem;height:2.4rem;flex:none}
h1{font-size:1.4rem;line-height:1.2;margin:0;font-weight:650}
header p{margin:0;color:var(--soft);font-size:.95rem}
section{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1rem;margin:0 0 .9rem}
h2{font-size:1.05rem;margin:0 0 .6rem;font-weight:600}
h2 small{color:var(--dim);font-weight:400;font-size:.85rem;margin-left:.4rem}
label{display:block;color:var(--soft);font-size:.9rem;margin:.7rem 0 .3rem}
label:first-of-type{margin-top:0}
input,select{width:100%;font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:.65rem .8rem;min-height:2.9rem}
input::placeholder{color:#6f6a62}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:1px}
#other{margin-top:.5rem}
.pw{display:flex;gap:.5rem}
.pw input{flex:1;min-width:0}
.pw button{flex:none;font:inherit;font-size:.9rem;color:var(--soft);background:none;border:1px solid var(--line);border-radius:10px;padding:0 .9rem}
.hint{color:var(--dim);font-size:.85rem;margin:.45rem 0 0}
.saved{color:var(--ok);font-size:.9rem;margin:0 0 .55rem}
.err{background:#3a2422;border:1px solid var(--err);color:#f3c4bb;border-radius:12px;padding:.75rem .9rem;margin:0 0 .9rem}
.save{width:100%;font:inherit;font-weight:650;font-size:1.05rem;color:var(--bg);background:var(--accent);border:0;border-radius:12px;padding:.9rem;margin-top:.3rem}
.save:active{filter:brightness(.9)}
.agent{display:flex;align-items:center;gap:.6rem;margin:0 0 .5rem;color:var(--text);font-size:1rem}
.agent input{width:1.2rem;min-height:0;height:1.2rem;flex:none}
.agent small{color:var(--dim);margin-left:auto}
[hidden]{display:none}
</style></head><body><main>
<header><svg viewBox="0 0 64 48" aria-hidden="true"><defs><clipPath id="cl"><path d="M61.2 31.2L61.1 32.7L61.1 34.3L61.2 35.8L60.9 37.3L60.2 38.8L59.3 40.1L58.0 41.2L56.3 42.0L54.0 42.4L51.4 42.4L49.2 42.4L47.4 42.4L45.8 42.4L44.4 42.4L43.2 42.4L42.1 42.4L41.1 42.4L40.1 42.4L39.3 42.4L38.5 42.4L37.7 42.4L37.0 42.4L36.3 42.4L35.6 42.4L35.0 42.4L34.4 42.4L33.8 42.4L33.2 42.4L32.6 42.4L32.0 42.4L31.4 42.4L30.8 42.4L30.2 42.4L29.6 42.4L29.0 42.4L28.4 42.4L27.7 42.4L27.0 42.4L26.3 42.4L25.5 42.4L24.7 42.4L23.9 42.4L22.9 42.4L21.9 42.4L20.8 42.4L19.6 42.4L18.2 42.4L16.6 42.4L14.8 42.4L12.6 42.4L10.0 42.4L7.9 41.9L6.3 41.1L5.0 40.0L4.1 38.7L3.5 37.3L3.2 35.8L3.3 34.2L3.4 32.7L3.2 31.2L3.2 29.7L3.4 28.2L3.8 26.7L4.3 25.3L5.1 24.0L6.0 22.7L7.0 21.6L8.2 20.6L9.6 19.8L11.1 19.1L12.7 18.7L14.4 18.4L16.3 18.4L16.7 17.5L17.1 16.3L17.6 15.2L18.2 14.1L18.8 13.1L19.6 12.1L20.4 11.1L21.3 10.2L22.3 9.3L23.3 8.6L24.4 7.9L25.6 7.2L26.8 6.7L28.1 6.3L29.3 6.0L30.7 5.7L32.0 5.6L33.3 5.6L34.7 5.7L36.0 5.9L37.3 6.2L38.6 6.6L39.8 7.1L41.0 7.7L42.1 8.4L43.2 9.2L44.2 10.1L45.1 11.0L46.0 12.0L46.7 13.0L47.4 14.1L48.0 15.2L48.5 16.4L48.9 17.5L49.2 18.7L49.4 19.9L51.3 20.1L53.1 20.4L54.8 21.1L56.2 21.9L57.5 22.9L58.6 24.1L59.5 25.3L60.3 26.7L60.8 28.2L61.1 29.7Z"/></clipPath></defs><path d="M61.2 31.2L61.1 32.7L61.1 34.3L61.2 35.8L60.9 37.3L60.2 38.8L59.3 40.1L58.0 41.2L56.3 42.0L54.0 42.4L51.4 42.4L49.2 42.4L47.4 42.4L45.8 42.4L44.4 42.4L43.2 42.4L42.1 42.4L41.1 42.4L40.1 42.4L39.3 42.4L38.5 42.4L37.7 42.4L37.0 42.4L36.3 42.4L35.6 42.4L35.0 42.4L34.4 42.4L33.8 42.4L33.2 42.4L32.6 42.4L32.0 42.4L31.4 42.4L30.8 42.4L30.2 42.4L29.6 42.4L29.0 42.4L28.4 42.4L27.7 42.4L27.0 42.4L26.3 42.4L25.5 42.4L24.7 42.4L23.9 42.4L22.9 42.4L21.9 42.4L20.8 42.4L19.6 42.4L18.2 42.4L16.6 42.4L14.8 42.4L12.6 42.4L10.0 42.4L7.9 41.9L6.3 41.1L5.0 40.0L4.1 38.7L3.5 37.3L3.2 35.8L3.3 34.2L3.4 32.7L3.2 31.2L3.2 29.7L3.4 28.2L3.8 26.7L4.3 25.3L5.1 24.0L6.0 22.7L7.0 21.6L8.2 20.6L9.6 19.8L11.1 19.1L12.7 18.7L14.4 18.4L16.3 18.4L16.7 17.5L17.1 16.3L17.6 15.2L18.2 14.1L18.8 13.1L19.6 12.1L20.4 11.1L21.3 10.2L22.3 9.3L23.3 8.6L24.4 7.9L25.6 7.2L26.8 6.7L28.1 6.3L29.3 6.0L30.7 5.7L32.0 5.6L33.3 5.6L34.7 5.7L36.0 5.9L37.3 6.2L38.6 6.6L39.8 7.1L41.0 7.7L42.1 8.4L43.2 9.2L44.2 10.1L45.1 11.0L46.0 12.0L46.7 13.0L47.4 14.1L48.0 15.2L48.5 16.4L48.9 17.5L49.2 18.7L49.4 19.9L51.3 20.1L53.1 20.4L54.8 21.1L56.2 21.9L57.5 22.9L58.6 24.1L59.5 25.3L60.3 26.7L60.8 28.2L61.1 29.7Z" fill="none" stroke="#d97757" stroke-width="7.2" clip-path="url(#cl)"/><path d="M20.0 24.0L27.2 29.6L20.0 35.2" fill="none" stroke="#d97757" stroke-width="3.6" stroke-linecap="round" stroke-linejoin="round"/><rect x="31.2" y="32.0" width="12.0" height="3.2" fill="#d97757"/></svg>
)HTML";

constexpr const char* kForm = R"HTML(<div><h1>{{PRODUCT}}</h1><p>Set up your {{TAGLINE}}</p></div></header>
{{ERROR}}<form method="post" action="/save" autocomplete="off">
<section><h2>Wi-Fi</h2>
<label for="ssid">Network</label>
<select id="ssid" name="ssid" data-kept="{{KEPT_SSID}}" data-now="{{WIFI_NOW}}"><option value="">{{WIFI_KEEP}}</option><option value="__other__">Other network...</option></select>
<input id="other" name="other" maxlength="32" placeholder="Network name" autocapitalize="off" autocorrect="off" spellcheck="false" hidden>
<label for="pass">Password</label>
<div class="pw"><input id="pass" name="pass" type="password" maxlength="64" autocapitalize="off" autocorrect="off" spellcheck="false"><button type="button" data-show="pass">Show</button></div>
<p class="hint" id="scan">Looking for networks...</p>
</section>
<section><h2>Anthropic API key</h2>
{{AKEY_NOTE}}<div class="pw"><input id="akey" name="akey" type="password" maxlength="256" placeholder="sk-ant-..." autocapitalize="off" autocorrect="off" spellcheck="false" aria-label="Anthropic API key"><button type="button" data-show="akey">Show</button></div>
<p class="hint">Make one at console.anthropic.com, under API keys. What you chat is billed to that Anthropic account.</p>
</section>
<section><h2>Groq API key<small>optional</small></h2>
{{GKEY_NOTE}}<div class="pw"><input id="gkey" name="gkey" type="password" maxlength="256" placeholder="gsk_..." autocapitalize="off" autocorrect="off" spellcheck="false" aria-label="Groq API key"><button type="button" data-show="gkey">Show</button></div>
<p class="hint">Turns on voice: hold G0 to talk, and hear the answers. Free at console.groq.com/keys.</p>
</section>
<section><h2>Agents<small>optional</small></h2>
{{AGENTS}}<label for="aname">Pair an agent</label>
<input id="aname" name="aname" value="{{KEPT_ANAME}}" maxlength="24" placeholder="Name, like Hermes" autocapitalize="off" autocorrect="off" spellcheck="false">
<div class="pw" style="margin-top:.5rem"><input id="acode" name="acode" type="password" maxlength="64" placeholder="Pairing code, cp1-..." autocapitalize="off" autocorrect="off" spellcheck="false" aria-label="Pairing code"><button type="button" data-show="acode">Show</button></div>
<input id="arelay" name="arelay" value="{{KEPT_ARELAY}}" maxlength="128" placeholder="Relay: ntfy.sh unless you run your own" autocapitalize="off" autocorrect="off" spellcheck="false" aria-label="Relay" style="margin-top:.5rem">
<p class="hint">The agent's connector prints the pairing code. Keep it private: whoever has it can give that agent work.</p>
</section>
<section><h2>Model</h2>
<select name="model" aria-label="Model">{{MODELS}}</select>
<p class="hint">You can change it any time from the Cardputer's menu.</p>
</section>
<section><h2>Time zone</h2>
<input id="tz" name="tz" value="{{TZ}}" data-now="{{TZ_NOW}}" maxlength="64" autocapitalize="off" autocorrect="off" spellcheck="false" aria-label="Time zone">
<p class="hint" id="tzname"></p>
<p class="hint" id="tzdst"></p>
</section>
<button class="save" type="submit">Save</button>
<p class="hint" style="text-align:center">On Android, if this page stops loading or won't save, turn off mobile data for a minute.</p>
<p class="hint" style="text-align:center">{{DISCLAIMER}}</p>
</form></main>
<script id="tzlib">
// getTimezoneOffset() counts minutes west of UTC (UTC+4 is -240). POSIX TZ also counts west as
// positive, but the <name> spells the east offset, e.g. "<+04>-4".
function posixTz(offsetMin) {
  if (offsetMin === 0) return 'UTC0';
  var east = -offsetMin, a = Math.abs(east), h = Math.floor(a / 60), m = a % 60;
  var mm = m ? (m < 10 ? '0' : '') + m : '';
  var name = '<' + (east > 0 ? '+' : '-') + (h < 10 ? '0' : '') + h + mm + '>';
  return name + (east > 0 ? '-' : '') + h + (mm ? ':' + mm : '');
}
// The phone's zone for `year` as a POSIX TZ string. With daylight saving it carries the rule, like
// "<+01>-1<+02>-2,M3.5.0/2,M10.5.0/3", worked out from the phone's own clock: the two moments in the year its
// offset changes. Without daylight saving, or with a pattern that isn't two changes, it is the fixed offset.
function posixRule(year) {
  var H = 3600000, start = Date.UTC(year, 0, 1), changes = [];
  var off = function (ms) { return new Date(ms).getTimezoneOffset(); };
  var prev = off(start);
  for (var i = 1; i <= 366 * 24; i++) {
    var at = start + i * H, cur = off(at);
    if (cur === prev) continue;
    while (off(at - 60000) === cur) at -= 60000;  // some zones change on the half hour
    changes.push({at: at, from: prev, to: cur});
    prev = cur;
  }
  if (changes.length !== 2 || changes[0].from !== changes[1].to) return posixTz(off(Date.now()));
  var std = Math.max(changes[0].from, changes[0].to), dst = Math.min(changes[0].from, changes[0].to);
  var part = function (o) { return o === 0 ? '<+00>0' : posixTz(o); };
  var rule = function (c) {
    var local = new Date(c.at - c.from * 60000);  // wall clock just before the change, read from the UTC fields
    var day = local.getUTCDate(), month = local.getUTCMonth(), m = local.getUTCMinutes();
    var days = new Date(Date.UTC(year, month + 1, 0)).getUTCDate();
    var week = day + 7 > days ? 5 : Math.ceil(day / 7);
    return 'M' + (month + 1) + '.' + week + '.' + local.getUTCDay() + '/' + local.getUTCHours() + (m ? ':' + (m < 10 ? '0' : '') + m : '');
  };
  var toDst = changes[0].to === dst ? changes[0] : changes[1], toStd = toDst === changes[0] ? changes[1] : changes[0];
  return part(std) + part(dst) + ',' + rule(toDst) + ',' + rule(toStd);
}
</script>
<script>
(function () {
  var tz = document.getElementById('tz'), phoneTz = posixTz(new Date().getTimezoneOffset()), zone = '';
  try { zone = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch (e) {}
  var phoneRule = posixRule(new Date().getFullYear());
  // A saved fixed offset that matches the phone right now is the same zone without its rule: upgrade it.
  if (!tz.value || tz.value === phoneTz) tz.value = phoneRule;
  document.getElementById('tzdst').textContent = tz.value.indexOf(',') > 0 ?
      'It follows daylight saving changes.' : 'A fixed offset from UTC, with no daylight saving changes.';
  var cardTz = tz.dataset.now;
  document.getElementById('tzname').textContent = (zone ? 'Your phone is on ' + zone + ' (' + phoneTz + ').' :
      'From your phone: ' + phoneTz + '.') + (cardTz && cardTz !== 'UTC0' && cardTz !== phoneTz ? ' The Cardputer has ' + cardTz + '.' : '');

  document.querySelectorAll('[data-show]').forEach(function (b) {
    b.onclick = function () {
      var input = document.getElementById(b.dataset.show), shown = input.type === 'text';
      input.type = shown ? 'password' : 'text';
      b.textContent = shown ? 'Show' : 'Hide';
    };
  });

  var sel = document.getElementById('ssid'), other = document.getElementById('other'), scan = document.getElementById('scan');
  sel.onchange = function () {
    other.hidden = sel.value !== '__other__';
    if (!other.hidden) other.focus();
  };
  fetch('/scan').then(function (r) { return r.json(); }).then(function (nets) {
    var bars = ['\u2582', '\u2582\u2584', '\u2582\u2584\u2586', '\u2582\u2584\u2586\u2588'];
    nets.forEach(function (n) {
      var o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + '   ' + bars[n.rssi > -55 ? 3 : n.rssi > -67 ? 2 : n.rssi > -78 ? 1 : 0] + (n.open ? '   open' : '');
      sel.insertBefore(o, sel.lastElementChild);
    });
    scan.textContent = !nets.length ? 'No networks nearby. Pick Other to type one.' :
        sel.dataset.now ? 'Leave both empty to keep ' + sel.dataset.now + '.' : 'Pick the network the Cardputer should use.';
  }).catch(function () {
    scan.textContent = 'Couldn\'t list networks. Pick Other to type one.';
  }).then(function () {
    var kept = sel.dataset.kept;
    if (!kept) return;
    if (Array.prototype.some.call(sel.options, function (o) { return o.value === kept; })) {
      sel.value = kept;
    } else {
      sel.value = '__other__';
      other.hidden = false;
      other.value = kept;
    }
  });
})();
</script></body></html>
)HTML";

constexpr const char* kSaved = R"HTML(<div><h1>Saved</h1><p>Look at your Cardputer</p></div></header>
<section><p style="margin:0">It's joining your Wi-Fi and trying your keys, and it shows how that went on its screen. You can close this page.</p></section>
</main></body></html>
)HTML";

inline String relayHost(const String& relay) {
  agent::RelayUrl url;
  return relay.isEmpty() ? String("ntfy.sh") : agent::parseRelay(relay, url) ? url.host : relay;
}

inline String agentRows(const std::vector<AgentRow>& agents) {
  String rows;
  for (size_t i = 0; i < agents.size(); ++i) {
    rows += "<label class=\"agent\"><input type=\"checkbox\" name=\"aremove\" value=\"" + String(static_cast<int>(i)) +
            "\"><span>" + htmlEscape(agents[i].name) + "</span><small>" + htmlEscape(relayHost(agents[i].relay)) +
            " &middot; tick to remove</small></label>\n";
  }
  return rows;
}

inline String keyNote(bool has, const String& tailChars) {
  if (!has) return "";
  return String("<p class=\"saved\">Saved") + (tailChars.length() ? ", ends in ..." + htmlEscape(tailChars) : String()) +
         ". Leave this empty to keep it.</p>";
}

// The setup form. After a failed save, `kept` brings back what was typed, except the secrets.
// The product's name and tagline into a page.
inline String branded(String html) {
  html.replace("{{PRODUCT}}", brand::kProduct);
  html.replace("{{TAGLINE}}", brand::kTagline);
  html.replace("{{DISCLAIMER}}", brand::kDisclaimer);
  return html;
}

inline String page(const Current& now, const std::vector<Choice>& models, const Form& kept, const String& error) {
  String html = branded(String(kHead) + kForm);
  const String model = kept.model.length() ? kept.model : now.model;
  String options;
  for (const Choice& m : models) {
    options += "<option value=\"" + htmlEscape(m.id) + "\"" + (m.id == model ? " selected" : "") + ">" +
               htmlEscape(m.name) + "</option>";
  }
  html.replace("{{ERROR}}", error.length() ? "<p class=\"err\" role=\"alert\">" + htmlEscape(error) + "</p>\n" : String());
  html.replace("{{KEPT_SSID}}", htmlEscape(kept.ssid));
  html.replace("{{WIFI_NOW}}", htmlEscape(now.wifiSsid));
  html.replace("{{WIFI_KEEP}}", now.wifiSsid.length() ? "Keep " + htmlEscape(now.wifiSsid) : String("Choose a network"));
  html.replace("{{AKEY_NOTE}}", keyNote(now.hasAnthropic, now.anthropicTail));
  html.replace("{{GKEY_NOTE}}", keyNote(now.hasGroq, now.groqTail));
  html.replace("{{MODELS}}", options);
  html.replace("{{TZ}}", htmlEscape(kept.tz));
  html.replace("{{TZ_NOW}}", htmlEscape(now.tz));
  html.replace("{{AGENTS}}", agentRows(now.agents));
  html.replace("{{KEPT_ANAME}}", htmlEscape(kept.agentName));
  html.replace("{{KEPT_ARELAY}}", htmlEscape(kept.agentRelay));
  return html;
}

inline String savedPage() { return branded(String(kHead) + kSaved); }

// "Kloudputer-1A2B": the product name and the last two bytes of the access point's MAC address.
inline String apName(const uint8_t mac[6]) {
  char tail[8];
  snprintf(tail, sizeof tail, "-%02X%02X", mac[4], mac[5]);
  return String(brand::kProduct) + tail;
}

// Lowercase letters and digits without look-alikes (no i l 1 o 0), easy to type from the screen.
constexpr const char* kPassChars = "abcdefghjkmnpqrstuvwxyz23456789";

inline String apPassword(uint32_t (*random)()) {
  const size_t n = strlen(kPassChars);
  String pass;
  for (int i = 0; i < 10; ++i) pass += kPassChars[random() % n];
  return pass;
}

// The Wi-Fi QR format phone cameras join from. \ ; , : " in the name or password are escaped.
inline String wifiQr(const String& ssid, const String& pass) {
  auto escape = [](const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); ++i) {
      if (strchr("\\;,:\"", s[i])) out += '\\';
      out += s[i];
    }
    return out;
  };
  return "WIFI:T:WPA;S:" + escape(ssid) + ";P:" + escape(pass) + ";;";
}
}  // namespace phone
