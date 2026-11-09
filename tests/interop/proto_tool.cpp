// The Cardputer's own protocol code (src/agent_proto.h, OpenSSL underneath) as a command-line tool, so the
// interop test drives the Node connector with the device's implementation instead of a Node re-implementation.
//   proto_tool topics <code>                          -> "<topicUp> <topicDown>"
//   proto_tool ask|ping|cancel <code> <job> <n> <ts> [text]   -> sealed body for topic-up
//   proto_tool open <code> <body>                     -> the down plaintext as parsed by the device, or exit 1
//   proto_tool approve <code> <job> <n> <ts> <req> <1|0>   -> sealed approve body for topic-up
#include "shim.h"
#include "agent_proto.h"
#include <cstdlib>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const String cmd = argv[1];
  uint8_t secret[32];
  agent::Keys k;
  if (!agent::parsePairingCode(argv[2], secret) || !agent::deriveKeys(secret, k)) return 3;
  if (cmd == "topics") {
    printf("%s %s\n", k.topicUp.c_str(), k.topicDown.c_str());
    return 0;
  }
  if (cmd == "open") {
    String plain;
    agent::Message m;
    if (argc < 4 || !agent::open(k.keyDown, k.topicDown, argv[3], plain) || !agent::parse(plain, m)) return 1;
    printf("%s|%s|%u|%s|%s|%d|%d|%d|%s|%s|%s\n", agent::kindName(m.kind), m.job.c_str(), m.n,
           (m.text.length() ? m.text : m.name).c_str(), m.busy ? "busy" : "-", m.i, m.of, m.ok ? 1 : 0,
           m.req.c_str(), m.tool.c_str(), m.detail.c_str());
    return 0;
  }
  if (cmd == "approve") {  // approve <code> <job> <n> <ts> <req> <allow 1|0>
    if (argc < 8) return 2;
    const String plain = agent::buildApprove(argv[3], strtoul(argv[4], nullptr, 10), strtoul(argv[5], nullptr, 10), argv[6], atoi(argv[7]) != 0);
    printf("%s\n", agent::seal(k.keyUp, k.topicUp, plain).c_str());
    return 0;
  }
  agent::Kind kind = cmd == "ask" ? agent::Kind::Ask : cmd == "ping" ? agent::Kind::Ping : cmd == "cancel" ? agent::Kind::Cancel
                                                                                          : agent::Kind::Unknown;
  if (kind == agent::Kind::Unknown || argc < 6) return 2;
  const String plain = agent::buildUp(kind, argv[3], strtoul(argv[4], nullptr, 10), strtoul(argv[5], nullptr, 10),
                                      argc > 6 ? String(argv[6]) : String());
  printf("%s\n", agent::seal(k.keyUp, k.topicUp, plain).c_str());
  return 0;
}
