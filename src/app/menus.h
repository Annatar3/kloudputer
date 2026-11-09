// Kloudputer — Menu navigation and the Voice, Model and Sessions menus.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

constexpr const char* kNavHint = "Arrows move  Enter pick  Del back";
constexpr const char* kSessionsHint = "Enter open  x delete  s find  Del back";
constexpr const char* kFindHint = "Type: titles  Enter: inside chats too";

enum class Nav : uint8_t { None, Up, Down, Left, Right, Pick, Back, Delete, Find };

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
  if (typed(*st, 's') || typed(*st, 'S')) return Nav::Find;
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
      case Nav::Delete: case Nav::Find: if (deletes) return {nav, menu.selected()}; break;
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
    items.push_back({modelName(kModels[i].id), usage::label(kModels[i].id) + (cur ? "  *" : ""),
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

// Saved chats, newest first. Enter resumes one (rebuilt while the menu still has the screen). The s key
// filters the list by words in the titles; Enter keeps the filter and Del or Esc clears it.
Then sessionsMenu() {
  int row = 0;
  String query;
  bool typing = false;
  std::vector<String> inside;  // chats whose prompts or replies match `insideFor`; filled when Enter ends typing
  String insideFor;
  sessions::Listing found = sdOk ? sessions::list(kMaxSessions) : sessions::Listing();
  while (true) {
    const time_t now = timeOk ? time(nullptr) : 0;
    std::vector<const sessions::Meta*> shown;
    std::vector<ui::MenuItem> items;
    for (const sessions::Meta& m : found.items) {
      const bool deep = query.length() && insideFor == query && std::find(inside.begin(), inside.end(), m.id) != inside.end();
      if (!sessions::matches(titleOf(m), query) && !deep) continue;
      shown.push_back(&m);
      items.push_back({titleOf(m), sessions::when(m.created, now) + (m.id == session.id ? " *" : "")});
    }
    if (items.empty()) items.push_back({!sdOk ? "No SD card" : query.length() ? "Nothing matches" : "No saved chats yet", ""});
    const String title = typing ? "Find: " + query + "_" : query.length() ? "Sessions: " + query : String("Sessions");
    showMenu(title.c_str(), std::move(items), row, typing ? kFindHint : kSessionsHint);

    if (typing) {
      while (true) {
        const auto* st = nextKeys();
        if (!st) {
          delay(5);
          continue;
        }
        if (st->enter) {
          typing = false;
          if (query.length() && insideFor != query) {  // titles filter as you type; the chats themselves are read once, here
            menu.notice("Searching inside chats...", ui::SYS);
            inside.clear();
            for (const sessions::Meta& m : found.items) {
              if (!sessions::matches(titleOf(m), query) && sessions::contains(m.id, query)) inside.push_back(m.id);
            }
            insideFor = query;
          }
        } else if (st->esc) {
          query = "";
          typing = false;
        } else if (st->backspace || st->del) {
          if (query.length()) query.remove(query.length() - 1);
          else typing = false;
        } else if (!st->fn) {
          for (char c : st->word) {
            if (query.length() < 24) query += c;
          }
        } else {
          continue;
        }
        break;
      }
      row = 0;
      continue;
    }

    while (true) {
      const Choice c = navigate(nullptr, true);
      if (c.nav == Nav::Back) {
        if (query.isEmpty()) return {};
        query = "";
        row = 0;
        break;
      }
      if (c.nav == Nav::Find) {
        typing = true;
        row = 0;
        break;
      }
      if (shown.empty()) continue;
      const sessions::Meta& m = *shown[c.row];
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
      found = sessions::list(kMaxSessions);
      row = c.row;
      break;
    }
  }
}
