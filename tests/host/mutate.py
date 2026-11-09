# Rerunnable: breaks one behavior at a time (session store, phone setup, settings, captive network,
# agent protocol and storage) in a scratch copy of src/ and checks that the
# host tests fail for each. Prints KILLED (tests caught it) or SURVIVED per mutant.
import os, shutil, subprocess, sys, tempfile
HERE0 = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(HERE0, "..", "..", "src"))
ONLY = sys.argv[2] if len(sys.argv) > 2 else ""  # run just the mutants whose name contains this
HERE = os.path.dirname(os.path.abspath(__file__))
MUTANTS = [
    ("sessions.h", "no newline repair before appending",
     "const bool repair = !fresh && !endsWithNewline(path);", "const bool repair = false;"),
    ("sessions.h", "id ignores the newest existing id",
     "if (stamp.length() && (newest.isEmpty() || newest < stamp)) return stamp;", "if (stamp.length()) return stamp;"),
    ("sessions.h", "title always hard-cut",
     "if (cut < static_cast<int>(kTitleChars) / 2) cut = kTitleChars;", "cut = kTitleChars;"),
    ("sessions.h", "load stops at the first bad line",
     "if (line.tooLong || deserializeJson(doc, line.text, line.len)) continue;",
     "if (line.tooLong || deserializeJson(doc, line.text, line.len)) break;"),
    ("sessions.h", "list keeps directory order",
     "ids.insert(std::find_if(ids.begin(), ids.end(), [&id](const String& o) { return o < id; }), id);",
     "ids.push_back(id);"),
    ("sessions.h", "history may start on a reply",
     "while (!h.empty() && !h.front().user) h.erase(h.begin());", ""),
    ("sessions.h", "orphan replies paired with an old prompt",
     '} else if (doc["a"].is<const char*>() && pending) {', '} else if (doc["a"].is<const char*>()) {'),
    ("sessions.h", "resume reads the whole file", "constexpr size_t kResumeLines = 40;", "constexpr size_t kResumeLines = 4000;"),
    ("sessions.h", "macOS ._ files listed", 'if (isDir || name.startsWith(".") || ', "if (isDir || "),

    ("setup_form.h", "a + in a form is kept, not a space", "if (c == '+') {\n      out += ' ';", "if (c == '+') {\n      out += '+';"),
    ("setup_form.h", "malformed %-escapes decode anyway", "  if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n  return -1;",
     "  if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n  return 0;"),
    ("setup_form.h", "saved keys show 6 characters", "key.substring(key.length() - 4)", "key.substring(key.length() - 6)"),
    ("setup_form.h", "a re-rendered page echoes the typed key",
     'html.replace("{{AKEY_NOTE}}", keyNote(now.hasAnthropic, now.anthropicTail));',
     'html.replace("{{AKEY_NOTE}}", keyNote(now.hasAnthropic, now.anthropicTail) + kept.anthropicKey);'),
    ("setup_form.h", "values reach the page unescaped", "      case '<': out += \"&lt;\"; break;\n", ""),
    ("setup_form.h", "page JS flips the POSIX sign", "return name + (east > 0 ? '-' : '') + h", "return name + (east > 0 ? '' : '-') + h"),
    ("setup_form.h", "any key prefix accepted",
     '  if (f.anthropicKey.length() && !f.anthropicKey.startsWith("sk-ant-")) return "The Anthropic key should start with sk-ant-";\n', ""),
    ("setup_form.h", "no body cap", "const size_t room = kMaxBody - std::min<size_t>(text.length(), kMaxBody);", "const size_t room = n;"),
    ("setup_form.h", "look-alike characters in the AP password", 'kPassChars = "abcdefghjkmnpqrstuvwxyz23456789"',
     'kPassChars = "abcdefghijklmnopqrstuvwxyz0123456789"'),
    ("config.h", "SD keys win over the phone's", "    c.anthropicKey = v;\n", "    if (c.anthropicKey.isEmpty()) c.anthropicKey = v;\n"),
    ("config.h", "key source not recorded", "    if (c.anthropicKey != anthropic) c.anthropicFrom = p;\n", ""),
    ("captive.h", "AAAA answered with an address", "const bool address = asked.type == 1 || asked.type == 255;",
     "const bool address = true;"),
    ("captive.h", "compressed or oversized names accepted",
     "if ((label & 0xC0) || pos + 1 + label > len || pos + 1 + label - 12 > 254) return false;",
     "if (pos + 1 + label > len) return false;"),
    ("captive.h", "multi-question queries answered", "if (opcode != 0 || qd != 1 || an != 0 || ns != 0) {",
     "if (opcode != 0 || qd == 0 || an != 0 || ns != 0) {"),
    ("captive.h", "EDNS OPT copied into the reply", "out.insert(out.end(), q + 12, q + questionEnd);",
     "out.insert(out.end(), q + 12, q + len);"),
    ("captive.h", "capport Accept matched case-sensitively",
     "  for (size_t i = 0; i < lower.length(); ++i) lower.setCharAt(i, tolower(lower[i]));\n", ""),
    ("captive.h", "NetworkMonitor's probe not recognized",
     '  if (ua.indexOf("X11; Linux x86_64") >= 0 && ua.indexOf("Chrome/60.0.3112.32") >= 0) return Client::AndroidProbe;\n', ""),
    ("captive.h", "setup log grows without bound", "    if (lines_.size() == kSize) lines_.pop_front();\n", ""),
    ("setup_form.h", "setup network back on 192.168.4.1", "constexpr uint8_t kApIp[4] = {192, 0, 2, 1};",
     "constexpr uint8_t kApIp[4] = {192, 168, 4, 1};"),
    ("agent_proto.h", "AAD ignored when opening",
     "if (!agentcrypto::gcmOpen(key, box.data(), reinterpret_cast<const uint8_t*>(topic.c_str()), topic.length(),",
     "if (!agentcrypto::gcmOpen(key, box.data(), nullptr, 0,"),
    ("agent_proto.h", "replays not deduplicated",
     "    if (std::any_of(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.n == n && s.job == job; })) return false;\n", ""),
    ("agent_proto.h", "time window off by one", "if (now == 0 || static_cast<int64_t>(now) - ts > kPast ||",
     "if (now == 0 || static_cast<int64_t>(now) - ts >= kPast ||"),
    ("agent_proto.h", "HKDF counter starts at 2", "for (uint8_t counter = 1; done < len; ++counter) {",
     "for (uint8_t counter = 2; done < len; ++counter) {"),
    ("agent_proto.h", "base64url stray bits accepted", "  return (acc & ((1u << bits) - 1)) == 0;", "  return true;"),
    ("agent_proto.h", "report completes with parts missing",
     "for (int i = b; all && i < b + wanted(); ++i) all = parts_.count(i) > 0;", "all = all;"),
    ("agent_proto.h", "long reports kept whole", "    if (i <= kKeep) parts_[i] = text;", "    parts_[i] = text;"),
    ("agent_proto.h", "other protocol versions accepted",
     "!doc.is<JsonObject>() || doc[\"v\"] != 1 ||", "!doc.is<JsonObject>() ||"),
    ("setup_form.h", "a typed pairing code comes back on the page",
     'html.replace("{{KEPT_ANAME}}", htmlEscape(kept.agentName));', 'html.replace("{{KEPT_ANAME}}", htmlEscape(kept.agentName + kept.agentCode));'),
    ("config.h", "re-pairing adds a duplicate", "    if (same != kept.end()) *same = add;\n    else kept.push_back(add);",
     "    kept.push_back(add);"),
    ("config.h", "removed agents leave their secrets in NVS", "      else if (p.isKey(key.c_str())) p.remove(key.c_str());\n", ""),
    ("config.h", "migration overwrites an existing new folder",
     "  if (!SD.exists(brand::kOldDataDir) || SD.exists(DATA_DIR)) return Migration::None;",
     "  if (!SD.exists(brand::kOldDataDir)) return Migration::None;"),
    ("config.h", "migration never runs", "  return SD.rename(brand::kOldDataDir, DATA_DIR) ? Migration::Moved : Migration::Failed;",
     "  return Migration::None;"),
]
for file, name, old, new in MUTANTS:
    if ONLY and ONLY not in name and ONLY != file:
        continue
    tmp = tempfile.mkdtemp()
    shutil.copytree(SRC, tmp, dirs_exist_ok=True)
    path = os.path.join(tmp, file)
    text = open(path).read()
    assert text.count(old) == 1, f"mutant '{name}' no longer matches {file}"
    open(path, "w").write(text.replace(old, new))
    r = subprocess.run([os.path.join(HERE, "run.sh"), tmp], capture_output=True, text=True)
    fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    print(("KILLED  " if r.returncode else "SURVIVED") + f"  {name}  ({len(fails)} failing checks)", flush=True)
    for l in fails[:2]:
        print("          " + l.split("  ", 1)[-1][:150], flush=True)
    shutil.rmtree(tmp)
