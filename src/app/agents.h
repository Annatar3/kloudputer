// Kloudputer — Agents: the agent screen, approvals, and listening for a job in the background.
// Part of main.cpp: included there once, in order, and uses the globals and helpers defined above it.
#pragma once

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
  approvals::Queue asks;  // permission requests waiting for y or n
  bool prompting = false;      // the front request is on screen
  bool paired = false;
};

// The agent whose screen is open, or whose job is still running after its screen was left.
std::unique_ptr<AgentLink> agentLink;
bool agentOnScreen = false;

void agentPause() {
  if (agentLink) agentLink->relay.pause();
}

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
  if (voiceReplies && agentOnScreen) {
    a.relay.pause();  // speech opens its own TLS session; one at a time
    speak(text, agentView);
  }
}

bool sendApprove(AgentLink& a, const String& req, bool allow) {
  const String body = agent::seal(a.keys.keyUp, a.keys.topicUp, agent::buildApprove(a.job, ++a.n, time(nullptr), req, allow));
  return body.length() && a.relay.publish(a.keys.topicUp, body);
}

// Puts the oldest waiting request on screen and asks for y or n.
void showApproval(AgentLink& a) {
  const approvals::Pending& p = a.asks.front();
  agentView.add(a.entry.name + " wants to use " + p.tool + ":", ui::SYS);
  agentView.add(p.detail.length() ? p.detail : String("(no details)"), ui::GREEN);
  if (a.asks.size() > 1) agentView.add(String(static_cast<int>(a.asks.size()) - 1) + " more waiting.", ui::DIM);
  agentView.setInput("", "allow? y/n > ");
  a.prompting = true;
  pokeScreen();
  beep();
}

// Back to the normal prompt, or on to the next request.
void nextApproval(AgentLink& a) {
  a.prompting = false;
  if (a.asks.empty()) agentView.setInput(agentInput);
  else showApproval(a);
}

void answerApproval(AgentLink& a, bool allow) {
  const approvals::Pending p = a.asks.pop();
  const bool sent = sendApprove(a, p.req, allow);
  agentView.add(allow ? "Allowed: " + p.tool : "Denied: " + p.tool, allow ? ui::GREEN : ui::DIM);
  if (!sent) agentView.add("Couldn't send that; it will time out as denied.", ui::ERR);
  nextApproval(a);
}

// Everything still waiting is denied, for when the job ends or the screen closes. Best effort: the connector
// denies it anyway after five minutes.
void denyAllApprovals(AgentLink& a, bool tell) {
  for (const approvals::Pending& p : a.asks.takeAll()) {
    if (tell) sendApprove(a, p.req, false);
  }
  a.prompting = false;
}

// News from an agent whose screen isn't open: a line in the chat, the footer of an open menu, and a beep.
void notifyAgent(const String& text, bool sound) {
  view.add(text, ui::SYS);
  if (!view.shown()) menu.notice(text, ui::SYS);
  pokeScreen();
  if (sound) beep();
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
    case agent::Kind::Approval:
      if (a.state != AgentState::Working) break;  // the job is over; nothing is waiting on an answer
      switch (a.asks.add(m.req, m.tool, m.detail, millis())) {
        case approvals::Queue::Add::Kept:
          if (!agentOnScreen) notifyAgent(a.entry.name + " asks to use " + m.tool + ". Tab > Agents to answer.", true);
          else if (!a.prompting) showApproval(a);
          break;
        case approvals::Queue::Add::Full:
          sendApprove(a, m.req, false);  // more than can be shown; refuse it rather than leave it hanging
          break;
        default:
          break;
      }
      break;
    case agent::Kind::Part:
      if (a.report.add(m.i, m.of, m.text) && a.report.complete() && !a.reported) showReport(a);
      break;
    case agent::Kind::Done:
      denyAllApprovals(a, false);
      agentView.setInput(agentInput);
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
      denyAllApprovals(a, false);
      agentView.setInput(agentInput);
      agentView.add(m.text, ui::ERR);
      a.state = AgentState::Error;
      break;
    default:
      break;
  }
}

// Runs from nextKeys, so it is alive in the chat and in every menu. While a job is running and its agent's
// screen is closed, this keeps reading the relay: a request for permission or the end of the job beeps and
// leaves a line in the chat. Nothing is answered from here; the agent's screen does that.
void agentBackgroundTick() {
  static bool busy = false;
  if (!agentLink || agentOnScreen || agentHolds || busy) return;
  busy = true;
  AgentLink& a = *agentLink;
  for (String body; a.relay.poll(body);) onAgentMessage(a, body);
  if (const size_t lapsed = a.asks.expire(millis())) {
    notifyAgent(a.entry.name + ": " + String(static_cast<int>(lapsed)) + " request(s) timed out, so they were denied.", false);
  }
  if (a.state != AgentState::Working) {  // the job ended; its report is waiting on the agent's screen
    notifyAgent(a.entry.name + (a.state == AgentState::Done ? " finished." : " stopped.") + " Tab > Agents to read it.", true);
    a.relay.stop();
    agentLink.reset();
  }
  busy = false;
}

// The agent screen: type or speak a task, watch its steps, read (and hear) the report. Tab, or Del on
// an empty line, goes back to the list. Leaving while a job runs keeps listening for it in the background.
void agentScreen(const AgentEntry& entry) {
  const bool back = agentLink && agentLink->entry.name == entry.name && agentLink->entry.code == entry.code;
  if (!back) {
    if (agentLink) {  // another agent's job was still being listened to; it can't be any more
      denyAllApprovals(*agentLink, true);
      agentLink->relay.stop();
    }
    agentLink.reset(new AgentLink());
  }
  AgentLink& a = *agentLink;
  agentOnScreen = true;
  if (agentViewOf != entry.name) {
    agentView.clear();
    agentInput = "";
    agentViewOf = entry.name;
  }
  agentView.setTitle(entry.name);
  agentView.show();
  agentView.setInput(agentInput);
  if (back) {
    if (!a.asks.empty()) showApproval(a);
  } else {
    a.entry = entry;
    uint8_t secret[32];
    agent::RelayUrl url;
    a.paired = agent::parsePairingCode(entry.code, secret) && agent::deriveKeys(secret, a.keys) &&
               agent::parseRelay(entry.relay.length() ? entry.relay : agent::kDefaultRelay, url);
    memset(secret, 0, sizeof secret);
    if (!a.paired) {
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
  }

  String noteShown;
  while (true) {
    for (String body; a.relay.poll(body);) onAgentMessage(a, body);
    const bool waiting = a.relay.state() == agent::Relay::State::Waiting;
    const AgentState shown = waiting ? AgentState::Offline : a.state;
    if (const size_t lapsed = a.asks.expire(millis())) {
      agentView.add(String(static_cast<int>(lapsed)) + " request(s) timed out, so they were denied.", ui::SYS);
      nextApproval(a);
    }
    const String note = a.prompting ? String("ALLOW? y / n") : waiting ? "offline: " + a.relay.error() : String(stateName(shown));
    if (note != noteShown) {
      noteShown = note;
      header.setNote(note, a.prompting ? ui::ERR : stateColor(shown));
    }

    // While a request is on screen the keys answer it, so nothing typed lands in the task line.
    String answerKeys;
    const bool wasEmpty = agentInput.isEmpty();
    const KeyEvent ev = pollKeys(a.prompting ? answerKeys : agentInput);
    if (a.prompting) {
      approvals::Answer answer = approvals::answerFrom(answerKeys);
      if (answer == approvals::Answer::None && ev.esc) answer = approvals::Answer::Deny;
      if (answer != approvals::Answer::None) answerApproval(a, answer == approvals::Answer::Allow);
      if (ev.scroll) agentView.scroll(ev.scroll);
      if (ev.tab) break;
      delay(5);
      continue;
    }
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
      } else if (a.paired && a.relay.state() != agent::Relay::State::Off) {
        a.job = agent::newJobId();
        a.n = 0;
        denyAllApprovals(a, false);
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
  agentOnScreen = false;
  agentView.hide();
  if (a.state == AgentState::Working && a.job.length() && a.relay.state() != agent::Relay::State::Off) {
    a.prompting = false;  // a request left unanswered keeps waiting and shows again on the way back in
    view.add(entry.name + " keeps working. You'll hear a beep if it asks or finishes.", ui::DIM);
    return;
  }
  denyAllApprovals(a, true);
  a.relay.stop();
  agentLink.reset();
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
