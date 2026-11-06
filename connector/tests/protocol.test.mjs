import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { fixture, lib } from './helpers.mjs';

const S = Buffer.from('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f', 'hex');
const keys = lib.deriveKeys(S);
const PLAIN = '{"v":1,"k":"ask","j":"0badc0de","n":1,"ts":1762444800,"text":"is cms-api up?"}';
const BODY = 'oKGio6SlpqeoqaqrryCiHMBGtQuISoGsA7NUwazBvByIVhFX1I-qq7TCuw1HdIzhCvja_zfG5CjEIn-oxQxZSeOv9TeDzE9lVlGiTtDAtamaB_IxJX6lkDci-Jnku0SdWvmuruLnES2klg';
const VECTOR_NOW = 1762444800 + 60;
const up = (body, extra = {}) => lib.checkInbound(body, { key: keys.keyUp, topic: keys.topicUp, seen: new Map(), now: VECTOR_NOW, ...extra });

test('reproduces the PROTOCOL.md test vector', () => {
  assert.equal(lib.pairingCode(S), 'cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8');
  assert.equal(lib.pairingCode(S).length, 47);
  assert.equal(keys.topicUp, 'cp-lQakgFwxcIsQkFbCO6dVXw');
  assert.equal(keys.topicDown, 'cp-2QNwEsFuNcN6LqaSzNRoCg');
  assert.equal(keys.keyUp.toString('hex'), '093a5da7f6e59518747167e3489862684f77e1947894d4e518671e6c69752e10');
  assert.equal(keys.keyDown.toString('hex'), '75f24d2b1b5bbff8c48897267724db79912b4b465cd07144d017978a48bc86d3');
  assert.equal(lib.seal(keys.keyUp, keys.topicUp, PLAIN, Buffer.from('a0a1a2a3a4a5a6a7a8a9aaab', 'hex')), BODY);
  assert.deepEqual(up(BODY).msg, JSON.parse(PLAIN));
});

test('drops a tampered body', () => {
  for (const at of [0, 20, BODY.length - 1]) {
    const flipped = BODY.slice(0, at) + (BODY[at] === 'A' ? 'B' : 'A') + BODY.slice(at + 1);
    assert.deepEqual(up(flipped), { drop: 'decrypt' }, `flip at ${at}`);
  }
  assert.deepEqual(up(BODY.slice(0, 30)), { drop: 'decrypt' });
  assert.deepEqual(up(''), { drop: 'decrypt' });
});

test('drops a body sealed with another key or for another topic', () => {
  const other = lib.deriveKeys(Buffer.alloc(32, 7));
  assert.deepEqual(up(lib.seal(other.keyUp, other.topicUp, PLAIN)), { drop: 'decrypt' });
  assert.deepEqual(up(lib.seal(keys.keyDown, keys.topicUp, PLAIN)), { drop: 'decrypt' });
  assert.deepEqual(up(lib.seal(keys.keyUp, keys.topicDown, PLAIN)), { drop: 'decrypt' }, 'AAD binds the topic');
});

test('drops messages outside the 15 min past / 2 min ahead window', () => {
  const at = ts => up(lib.seal(keys.keyUp, keys.topicUp, JSON.stringify({ ...JSON.parse(PLAIN), ts })), { now: 1_800_000_000 });
  assert.ok(at(1_800_000_000 - 15 * 60).msg, 'exactly 15 min old is accepted');
  assert.deepEqual(at(1_800_000_000 - 15 * 60 - 1), { drop: 'stale' });
  assert.ok(at(1_800_000_000 + 120).msg, 'exactly 2 min ahead is accepted');
  assert.deepEqual(at(1_800_000_000 + 121), { drop: 'stale' });
});

test('drops a replayed (j, n) pair and accepts the next n', () => {
  const seen = new Map([['0badc0de:1', 1762444800]]);
  assert.deepEqual(up(BODY, { seen }), { drop: 'replay' });
  const next = lib.seal(keys.keyUp, keys.topicUp, PLAIN.replace('"n":1', '"n":2'));
  assert.equal(up(next, { seen }).id, '0badc0de:2');
});

test('drops well-sealed plaintexts that are not protocol messages', () => {
  for (const plain of ['not json', '[]', '{"v":2,"k":"ask","j":"0badc0de","n":1,"ts":1762444800}',
    '{"v":1,"k":"ask","j":"0BADC0DE","n":1,"ts":1762444800}', '{"v":1,"k":"ask","j":"0badc0de","n":"1","ts":1762444800}']) {
    assert.deepEqual(up(lib.seal(keys.keyUp, keys.topicUp, plain)), { drop: 'malformed' }, plain);
  }
});

const sealedPartBytes = (text, i, of) => Buffer.byteLength(JSON.stringify({
  v: 1, k: 'part', j: 'ffffffff', n: Number.MAX_SAFE_INTEGER, ts: 9999999999, i, of, text,
}));

test('chunks reports at exactly 2,400 characters', () => {
  assert.deepEqual(lib.chunkReport('a'.repeat(2400)).map(c => c.length), [2400]);
  assert.deepEqual(lib.chunkReport('a'.repeat(2401)).map(c => c.length), [2400, 1]);
  assert.deepEqual(lib.chunkReport('a'.repeat(4800)).map(c => c.length), [2400, 2400]);
  assert.deepEqual(lib.chunkReport(''), []);
});

test('keeps every sealed part under 2,800 plaintext bytes whatever the text', () => {
  const texts = {
    georgian: 'გამარჯობა '.repeat(700),
    escapes: '"\\\n\t\u0001'.repeat(1500),
    emoji: '😀'.repeat(3000),
    mixed: ('ok "quoted" ' + 'ფ' + '😀\n').repeat(900),
  };
  for (const [name, text] of Object.entries(texts)) {
    const parts = lib.chunkReport(text);
    assert.equal(parts.join(''), text, `${name} survives intact`);
    parts.forEach((part, i) => {
      assert.ok(part.length <= 2400, `${name} part ${i} chars`);
      assert.ok(sealedPartBytes(part, i + 1, parts.length) <= 2800, `${name} part ${i} bytes ${sealedPartBytes(part, i + 1, parts.length)}`);
      assert.ok(!/[\uD800-\uDBFF]$/.test(part), `${name} part ${i} does not end inside a surrogate pair`);
    });
  }
});

test('cuts an oversized report to 20 parts and says how much is missing', () => {
  const parts = lib.chunkReport('x'.repeat(2400 * 25));
  assert.equal(parts.length, 20);
  assert.equal(parts.at(-1), `[report cut: ${2400 * 6} more characters on the agent host]`);
});

test('turns captured stream-json into step lines', () => {
  const steps = name => readFileSync(fixture(name), 'utf8').split('\n').filter(Boolean).map(l => lib.stepText(JSON.parse(l))).filter(Boolean);
  assert.deepEqual(steps('stream-read.jsonl'), ['Read: note.txt', 'pineapple']);
  assert.deepEqual(steps('stream-denied.jsonl'), ['Bash: touch made.txt', 'Denied: Bash', 'refused']);
  const tool = (name, input) => lib.stepText({ type: 'assistant', message: { content: [{ type: 'text', text: 'Let me look.' }, { type: 'tool_use', name, input }] } });
  assert.equal(tool('Bash', { command: '\nkubectl get pods -n cms \\\n  -o wide' }), 'Bash: kubectl get pods -n cms \\');
  assert.equal(tool('Edit', { file_path: '/srv/agent/workspace/deploy/values.yaml' }), 'Edit: values.yaml');
  assert.equal(tool('mcp__kube__list', { ns: 'cms' }), 'mcp__kube__list');
  for (const name of ['__defineGetter__', 'constructor', 'hasOwnProperty']) assert.equal(tool(name, {}), name);
});

test('scrubs key- and token-shaped strings', () => {
  const line = 'auth failed: sk-ant-api03-AAAAbbbbCCCC1234 Bearer abcdef123456 CLAUDE_CODE_OAUTH_TOKEN=sk-xyz password: hunter2 '
    + 'blob QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVoxMjM0NTY3ODkw end';
  const out = lib.scrub(line);
  for (const secret of ['sk-ant', 'abcdef123456', 'sk-xyz', 'hunter2', 'QUJDREVG']) assert.ok(!out.includes(secret), `${secret} in ${out}`);
  assert.ok(out.startsWith('auth failed:') && out.endsWith(' end'));
  const shapes = lib.scrub('{"password": "hunter2", "api_key":"k3y"} postgres://admin:s3cret@db/x mysql --password hunter3 -token=abc');
  for (const secret of ['hunter2', 'k3y', 's3cret', 'hunter3', 'abc']) assert.ok(!shapes.includes(secret), `${secret} in ${shapes}`);
  assert.ok(shapes.includes('postgres://admin:[redacted]@db/x'), shapes);
  const plain = 'Error: connect ECONNREFUSED 10.0.0.5:443 while reading /srv/agent/state/gcloud/configurations';
  assert.equal(lib.scrub(plain), plain);
});

test('scrubs in linear time, even on hostile input', () => {
  for (const text of ['a-'.repeat(32_000), 'token'.repeat(12_000), 'x://'.repeat(16_000), '--'.repeat(32_000)]) {
    const started = performance.now();
    lib.scrub(text);
    const ms = performance.now() - started;
    assert.ok(ms < 250, `${text.slice(0, 8)}... took ${Math.round(ms)} ms`);
  }
});
