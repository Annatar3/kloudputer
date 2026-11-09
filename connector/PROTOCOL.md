# Kloudputer agent protocol, v1

The Cardputer talks to a connector next to your own Claude Code agent through a dumb relay. The relay is an
ntfy server (`https://ntfy.sh` by default, or self-hosted). It only ever sees random topic names and
ciphertext. It can drop or delay messages, but it can't read them, forge them, or replay them past the checks below.

## Pairing

The connector creates a 32-byte random secret `S` once and prints the pairing code:

    cp1-<base64url(S), no padding>        (47 characters)

The owner pastes the code into the Cardputer's phone-setup page with a display name. Both sides derive everything from `S`
with HKDF-SHA256, salt `claudeputer-v1`:

| label        | length | use                                                         |
|--------------|--------|-------------------------------------------------------------|
| `topic-up`   | 16     | topic `cp-` + base64url(no padding): Cardputer → connector   |
| `topic-down` | 16     | topic `cp-` + base64url(no padding): connector → Cardputer   |
| `key-up`     | 32     | AES-256-GCM key for up messages                             |
| `key-down`   | 32     | AES-256-GCM key for down messages                           |

The salt keeps the project's original name. It is a fixed protocol constant, and changing it would change every
derived topic and key and unpair every device.

The relay base URL is set separately on each side and defaults to `https://ntfy.sh`.

## Envelope

The ntfy message body is `base64url(nonce[12] || ciphertext || tag[16])`, without padding. The nonce is 12 random
bytes, and the AAD is the topic name in ASCII. Publish with `POST <relay>/<topic>`, the body as text, and the header
`X-Firebase: no`. Bodies must stay under ntfy's 4,096-byte limit, so a plaintext is at most 2,800 bytes.

Receivers poll with `GET <relay>/<topic>/json?poll=1&since=<last id>`, or stream `GET <relay>/<topic>/json`. A message
is dropped silently if it fails to decrypt, its `ts` is more than 15 minutes old or 2 minutes ahead, or its
`(j, n)` pair was already seen.

## Plaintext

Each plaintext is a JSON object with these common fields:
- `"v":1`
- `"k"`: the kind
- `"j"`: the job id, 8 hex characters chosen by the Cardputer (empty for hello and ping)
- `"n"`: a per-sender counter that goes up within a job
- `"ts"`: unix seconds

Up kinds (Cardputer → connector):
- `ask {text}`: start a job, with text of at most 2,000 characters. The connector runs one job at a time and answers
  `error` with "busy" for a second one.
- `cancel {}`: stop the job.
- `approve {req, allow}`: the answer to an `approval`. `req` is the id it carried and `allow` is a JSON boolean.
- `ping {}`: the connector answers `hello`.

Down kinds (connector → Cardputer):
- `hello {name, busy, version}`
- `ack {}`: the job started.
- `step {text}`: progress, at most 120 characters, for example `Bash: kubectl get pods -n cms`. Steps go out at
  most once every 8 s, and only the latest is kept, which stays within ntfy.sh's limits.
- `approval {req, tool, detail}`: the agent wants to use a tool that needs permission. See Approvals below.
- `part {i, of, text}`: the final report, in chunks of at most 2,400 characters.
- `done {ok, secs, cost}`: the job is over (it follows the last part).
- `error {text}`

## Approvals

A connector run with `"approvals": true` gives the agent a permission prompt tool. Each time the agent wants something its
settings don't already allow, the connector sends an `approval` for the job that is running and waits for the answer.

- `req` is 8 lowercase hex characters, new for every request.
- `tool` is the tool's name, at most 40 characters.
- `detail` is at most 200 characters and says what will happen: the whole command for Bash, the whole path for file tools,
  the URL, the query, or the tool's input as JSON for anything else. A line break shows as `\n`, and credential-shaped
  text is replaced by `[redacted]`. When it doesn't fit it is cut and ends with ` [+N more]`, with N the exact number of
  characters left out, so a long command can't hide its tail.

The answer is one `approve` with the same `j` and `req`. Only `"allow": true` allows. Anything else is a denial. The
decision covers that one call: there is no "always allow". The connector ignores an `approve` whose job or `req` doesn't
match a request that is waiting, and an answer that arrives a second time.

A request is denied when nothing answers within 5 minutes, when the job ends or is cancelled, when the connector stops, and
when the connector can't reach its own prompt tool.

A device keeps listening for a running job after its owner leaves the agent screen (firmware v1.9 and later), and beeps
when a request arrives, so the request can be answered by going back. It stops listening when the job ends, when the
device restarts, or when another agent's screen is opened; a request it never sees is denied after the 5 minutes. Up to 8
requests can wait at once, and a device refuses a ninth.

## Test vector (generated by `node vector.mjs`)

    S          000102…1e1f (bytes 0x00 to 0x1f)
    pairing    cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8
    topic-up   cp-lQakgFwxcIsQkFbCO6dVXw
    topic-down cp-2QNwEsFuNcN6LqaSzNRoCg
    key-up     093a5da7f6e59518747167e3489862684f77e1947894d4e518671e6c69752e10
    key-down   75f24d2b1b5bbff8c48897267724db79912b4b465cd07144d017978a48bc86d3
    nonce      a0a1a2a3a4a5a6a7a8a9aaab
    plaintext  {"v":1,"k":"ask","j":"0badc0de","n":1,"ts":1762444800,"text":"is cms-api up?"}
    body (up)  oKGio6SlpqeoqaqrryCiHMBGtQuISoGsA7NUwazBvByIVhFX1I-qq7TCuw1HdIzhCvja_zfG5CjEIn-oxQxZSeOv9TeDzE9lVlGiTtDAtamaB_IxJX6lkDci-Jnku0SdWvmuruLnES2klg
