# Kloudputer agent connector

The connector runs next to a Claude Code agent and lets the Cardputer hand it tasks. The Cardputer sends an
`ask`, the connector runs the agent once with that text, and the Cardputer gets an `ack`, progress `step`s, the final
report in `part`s, and `done`. Everything travels through an ntfy relay as AES-256-GCM ciphertext on topics derived
from a shared secret, as [PROTOCOL.md](PROTOCOL.md) specifies. The relay can delay or drop messages, but it cannot
read, forge or replay them.

`kloudputer-connector.mjs` is a single file that needs Node 18.3 or later and nothing from npm.

## Commands

```sh
node kloudputer-connector.mjs pair --secret <file> [--force]   # create the secret (0600), print the pairing code
node kloudputer-connector.mjs code --secret <file>             # print the pairing code again
node kloudputer-connector.mjs run --config <file.json>         # serve the paired Cardputer
```

`pair` refuses to replace an existing secret. With `--force` it replaces it, and the Cardputer paired with the old
secret stops working. `run` refuses a secret file that group or other users can read.

The pairing code is the secret. Anyone holding it can give this agent tasks, so only `pair` and `code` print it.

## Chat with a Claude Pro or Max account (no API key)

The firmware talks to Anthropic with an API key. A Pro/Max plan can't be used that way: Anthropic only allows
those logins inside Claude Code and claude.ai. The connector runs the real `claude` CLI, so it can use your
plan, and the Cardputer chats with it as an agent.

1. On the PC: `claude` once and `/login` with your Pro account. Make the folder `deploy/chat.json` names as `cwd`.
2. `node kloudputer-connector.mjs pair --secret chat.secret`, then `node kloudputer-connector.mjs run --config deploy/chat.json`.
3. On the Cardputer, paste the code in setup (an API key is no longer required once an agent is paired), then Tab > Agents.

`deploy/chat.json` uses `--continue`, so replies keep the conversation going in that folder, and `--tools ""`, so chat
cannot run commands. Replies come after the whole answer is ready, not word by word. The PC must stay on.

## Configuration

| key          | default            | meaning                                                                                  |
|--------------|--------------------|------------------------------------------------------------------------------------------|
| `relay`      | `https://ntfy.sh`  | ntfy base URL, without a user name or password. The Cardputer must use the same one.     |
| `secretFile` | required           | The 32-byte secret that `pair` wrote.                                                    |
| `name`       | required           | Display name sent in `hello`, at most 64 characters.                                     |
| `cwd`        | current directory  | Working directory for the agent command.                                                 |
| `command`    | required           | argv array. Each element equal to `"{prompt}"` becomes the task text, as one argument.   |
| `env`        | `{}`               | Extra environment variables for the agent, on top of the connector's own.                |
| `maxJobSecs` | `900`              | A job still running after this many seconds is killed and reported as timed out. At most 86,400. |
| `stateFile`  | required           | Where the connector keeps the last relay message id, its message counter and recent `(j, n)` pairs. Must be writable. |

Relative paths resolve against the config file's directory. An unknown key is an error, so a typo cannot silently fall
back to a default.

Put `--` before `"{prompt}"` when the agent is Claude Code. Claude Code parses a prompt that starts with `-` as an
option. With 2.1.319, `claude -p "--bogus"` fails with `unknown option '--bogus'`, and `claude -p -- "--bogus"` treats
it as the prompt. The connector never uses a shell, so `$( )`, backticks and `;` in a task stay literal text.

## What a job looks like

- One job runs at a time. A second `ask` gets `error` "busy". `ping` gets `hello` with `busy` set accordingly.
- The command starts in its own process group with stdin closed. A `cancel` or a timeout sends SIGTERM to the group
  and SIGKILL 5 s later. When the agent exits, anything it left running in its group gets the same treatment, so no
  job leaves processes behind. A process that left the group but still holds the agent's output open cannot keep the
  job busy: the connector stops waiting for that output 5 s after the agent exits.
- The connector reads Claude Code's `--output-format stream-json --verbose` lines. A `tool_use` block becomes a step
  such as `Bash: kubectl get pods -n cms` or `Read: values.yaml`. A refused tool call becomes `Denied: Bash`. An
  assistant message without a tool call contributes its first line. Steps go out at most once every 8 s, counted
  from when the previous step was actually sent, and only the newest waiting step is kept. A step still waiting when
  the job ends is dropped.
- The report goes out as soon as the `result` event arrives, in `part`s numbered from `i: 1` to `of`, followed by
  `done {ok, secs, cost}`. `cost` is `total_cost_usd`, or `null` when Claude Code does not report it. Each part holds
  at most 2,400 characters and at most 2,800 bytes once sealed, so non-ASCII text gets smaller parts. A report longer
  than 20 parts keeps its first 19 parts, and the 20th says how many characters were cut. An agent still running
  5 s after its result is stopped like a cancelled one.
- When the agent exits without a `result`, the Cardputer gets `error` with the last line the agent wrote to stderr,
  with key- and token-shaped strings replaced by `[redacted]`. Steps go through the same scrubbing.
- Incoming messages that fail to decrypt, fall outside the timestamp window, repeat a `(j, n)` pair or are not
  protocol messages are dropped. The log shows how many were dropped and why, never what they said. It also never
  shows task text, steps or reports.
- Publishing waits out HTTP 429 for the `Retry-After` value, capped at 15 minutes, and retries other failures with
  backoff up to 60 s. The subscription reconnects with backoff, resumes with `since=<last id>`, and reconnects on its
  own when the stream is silent for 90 s, which is twice ntfy's keepalive interval. The connector treats the relay as
  hostile. It never follows a redirect from it, and it drops a stream line longer than 64 KB and reconnects.
- The state file keeps the last relay id, the outgoing counter `n` and the `(j, n)` pairs from the last 15 minutes.
  After a restart the connector neither repeats old tasks nor reuses message numbers, and a relay that replays an old
  ciphertext gets it dropped. If the state file cannot be written, `run` exits with an error before acting on the
  message, because replay protection depends on that file.
- On SIGTERM a running job is stopped and the Cardputer gets `error` "connector stopped" before the connector goes
  offline, which takes at most about 8 s.

## Run it beside your agent

`deploy/systemd/` has an example unit, `kloudputer-connector.service`, and a matching `connector.json`. Both use
placeholders: the user `agent`, its directory `/srv/agent`, and `/etc/kloudputer` for the config, the pairing secret
and the agent's credentials. The comment at the top of the unit lists what to adapt. With those in place:

```sh
sudo install -D -m 644 kloudputer-connector.mjs /usr/local/lib/kloudputer/kloudputer-connector.mjs
sudo install -d -m 700 -o agent -g agent /etc/kloudputer
sudo install -m 600 -o agent -g agent deploy/systemd/connector.json /etc/kloudputer/connector.json
sudo -u agent node /usr/local/lib/kloudputer/kloudputer-connector.mjs pair --secret /etc/kloudputer/connector.secret
sudo install -m 644 deploy/systemd/kloudputer-connector.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now kloudputer-connector
journalctl -u kloudputer-connector -n 20   # ends with: connector 0.1.0 "My agent" listening on ntfy.sh
```

The agent command in the example is `claude -p --output-format stream-json --verbose -- {prompt}`, run in the
agent's working directory with its environment. Every task from the Cardputer is therefore an ordinary Claude Code
run. The agent's managed and project settings, its hooks and any audit logging apply to it unchanged.

**Phase A refuses tools that need approval.** When the agent's permission settings make a tool call ask first, nothing
can answer that prompt in phase A. The command has no permission-prompt tool, and `claude -p` has no terminal. Claude
Code refuses each such call without asking, and the Cardputer shows a `Denied: <tool>` step. Tools the settings
allow outright, such as reads, still run.

This was checked with Claude Code 2.1.319 under the default permission mode. A `touch` through Bash that needed
approval came back as a tool error reading "needs approval". It also produced a `system/permission_denied` event
and an entry in the result's `permission_denials`, and no file was created. `tests/fixtures/stream-denied.jsonl` is
that capture.

**Keep the pairing secret away from the agent.** Anyone holding the secret can give the agent tasks. The connector
and the agent run as the same user, so file modes alone cannot hide the secret from the agent. Three layers help:

- Keep the secret readable only by the service user. `pair` creates it with mode 600, and `run` refuses a looser
  mode. Keep it outside the agent's working directory, and let `ProtectSystem=` make it read-only to the service.
- Deny the path in the agent's permission settings, for example `"deny": ["Read(/etc/kloudputer/**)"]` in managed
  settings that the agent cannot edit.
- Block shell commands that name the file with a `PreToolUse` hook on Bash. A hook that matches paths is one layer,
  not a boundary, because a glob such as `/etc/kloudputer/conn*` gets past a plain match.

Running the agent as a different user from the connector would be stronger. The connector cannot do that without a
privileged helper, which phase A does not have.

## Phase B, not built

`approve {req, allow}` is accepted and ignored. Phase B would put an MCP tool into the command through
`--permission-prompt-tool`. Claude Code 2.1.319 accepts that flag but leaves it out of `--help`. The tool would send
`approval {req, tool, detail}`, wait up to 5 minutes for the matching `approve`, and deny when none arrives. The
connector already has one job at a time and an ordered outbox for this.

## Tests

```sh
node --test                 # 49 tests, about 30 s, zero dependencies
node tests/mutate.mjs       # breaks 32 behaviors one at a time; every mutant must fail the suite
```

`tests/mock-ntfy.mjs` is an in-process ntfy with publish, poll, streaming, the 4,096-byte body limit and injected
429s. `tests/fake-agent.mjs` replays the captured stream-json fixtures. It also has variants that fail, hang, ignore
SIGTERM, or leave processes behind. `tests/helpers.mjs` has a `Card` class that does what the Cardputer does: it seals `ask`s, polls the down
topic over HTTP, and decrypts every reply. No test touches a real relay.

The fixtures come from real `claude -p --output-format stream-json --verbose` runs. Before a fixture goes into the
repo, `tests/scrub-capture.mjs` removes ids, paths, the user name, MCP servers, plugins, hook output and rate-limit
details. To refresh them from the Claude Code version on your agent host:

```sh
claude -p --output-format stream-json --verbose --max-turns 3 -- "Read note.txt and reply with the single word it contains" > raw.jsonl
node tests/scrub-capture.mjs raw.jsonl "$PWD" > tests/fixtures/stream-read.jsonl
```
