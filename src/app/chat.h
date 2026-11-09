// Kloudputer — The conversation with Claude: saving and resuming chats, notes, the cost estimate, and sending a prompt.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

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
  spent.chat = 0;  // a resumed chat counts from here; what it cost before isn't stored
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

void remember(const String& text) {
  if (!sdOk) {
    view.add("Notes need the SD card.", ui::ERR);
    return;
  }
  notes::Notes next = memory;
  switch (next.add(text)) {
    case notes::Add::Empty: view.add("Remember what? Try: /remember my server is atlas", ui::SYS); return;
    case notes::Add::Duplicate: view.add("Already noted.", ui::SYS); return;
    case notes::Add::Full:
      view.add("Notes are full (" + String(notes::kMaxNotes) + "). /forget one first.", ui::ERR);
      return;
    case notes::Add::Added: break;
  }
  if (!notes::save(next)) {
    view.add("Couldn't write the note to the SD card.", ui::ERR);
    return;
  }
  memory = next;
  view.add("Noted (" + String(memory.items().size()) + " of " + String(notes::kMaxNotes) + ").", ui::GREEN);
}

void showNotes() {
  if (memory.items().empty()) {
    view.add("No notes. Say or type: remember that ...", ui::SYS);
    return;
  }
  for (size_t i = 0; i < memory.items().size(); ++i) view.add(String(i + 1) + ") " + memory.items()[i], ui::DIM);
  view.add("/forget N deletes one, /forget all clears", ui::DIM);
}

void forget(const String& arg) {
  notes::Notes next = memory;
  if (arg == "all") next.clear();
  else if (!next.remove(arg.toInt())) {
    view.add("Which one? /notes shows the numbers.", ui::SYS);
    return;
  }
  if (!notes::save(next)) {
    view.add("Couldn't write the SD card.", ui::ERR);
    return;
  }
  memory = next;
  view.add(arg == "all" ? String("All notes forgotten.") : String("Forgotten."), ui::GREEN);
}

// yyyymmdd in local time, or 0 while the clock isn't set.
uint32_t todayStamp() {
  if (!timeOk) return 0;
  const time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

// Adds one API call to the running cost estimate and saves the day and all-time totals.
void recordSpend(const String& model, const ChatResult& r) {
  usage::add(spent, usage::costMicros(model, r.inTok, r.outTok, r.searches), todayStamp());
  Preferences p;
  if (p.begin("cputer", false)) {
    p.putUInt("udate", spent.date);
    p.putUInt("uday", spent.day);
    p.putUInt("utotal", spent.total);
    p.end();
  }
}

void showUsage() {
  view.add("Estimated from list prices and token counts:", ui::DIM);
  view.add("this chat " + usage::money(spent.chat) + "   today " + usage::money(usage::spentToday(spent, todayStamp())) +
               "   total " + usage::money(spent.total),
           ui::GREEN);
  if (!usage::priceOf(cfg.model)) view.add("No list price for " + cfg.model + ", so its tokens aren't counted.", ui::SYS);
  view.add("The Anthropic Console has the real bill.", ui::DIM);
}

// After a chat's first exchange, Haiku names it so the Sessions list reads better than the first prompt.
// Quiet on failure: the first-prompt title stays.
void nameSession(const String& prompt, const String& reply) {
  if (!sdOk || session.id.isEmpty()) return;
  const ChatResult named = claudeTitle(cfg.anthropicKey, prompt, reply);
  if (!named.ok) return;
  recordSpend(kTitleModel, named);
  const String title = sessions::cleanTitle(named.text);
  if (title.isEmpty() || !sessions::retitle(session.id, title)) return;
  session.title = title;
  Serial.printf("[chat] titled '%s'\n", title.c_str());
}

void sendPrompt(const String& text) {
  const RelayHold hold;
  view.addUser(text);
  String note;
  if (notes::rememberText(text, note)) {
    remember(note);
    return;
  }
  if (cfg.anthropicKey.isEmpty()) {
    view.add("No Anthropic API key yet. Type /setup to add one from your phone.", ui::ERR);
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
  RequestOptions opts;
  opts.spoken = voiceReplies;
  opts.webSearch = webSearch;
  opts.notes = memory.joined();
  const ChatResult r = anthropicChat(cfg.anthropicKey, cfg.model, history, opts);
  Serial.printf("[chat] <- ok=%d %ums in=%d out=%d searches=%d heap=%u err=%s\n", r.ok, r.ms, r.inTok, r.outTok,
                r.searches, ESP.getFreeHeap(), r.error.c_str());
  if (r.searchBlocked) {
    webSearch = false;
    view.add("Web search isn't available for this key, so it is off. Enable it in the Anthropic Console.", ui::SYS);
  }
  if (r.ok) {
    recordSpend(cfg.model, r);
    history.push_back({false, r.text});
    view.addClaude(r.text);
    const bool firstTurn = session.id.isEmpty();
    saveTurn(text, r.text);
    if (voiceReplies) speak(r.text);
    if (firstTurn) nameSession(text, r.text);
    header.setNote(String(r.ms / 100 / 10.0, 1) + "s " + r.inTok + "/" + r.outTok + " tok" +
                       (r.searches ? " " + String(r.searches) + " web" : String()),
                   ui::DIM);
  } else {
    history.pop_back();  // keep user/assistant turns paired
    view.add(r.error, ui::ERR);
    refreshHeader();
  }
  pokeScreen();
  lastHeaderMs = millis();
}
