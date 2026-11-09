import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { chmodSync, existsSync, readFileSync, writeFileSync } from 'node:fs';
import http from 'node:http';
import { join } from 'node:path';
import { test } from 'node:test';
import { startMockNtfy } from './mock-ntfy.mjs';
import { agent, fixture, harness, jobId, lib, reapAfter, tempDir, until, wait, writeFile } from './helpers.mjs';

const alive = pid => {
  try { process.kill(pid, 0); return true; } catch { return false; }
};
const pidsOf = file => until(() => existsSync(file) && readFileSync(file, 'utf8')).then(JSON.parse);
const kinds = msgs => msgs.map(m => m.k);

test('runs an ask end to end: ack, steps, parts, done, all sealed and in order', async t => {
  const h = await harness({ command: agent('replay', fixture('stream-read.jsonl'), '50') });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'Read note.txt and reply with the single word it contains' });
  await h.card.waitEnd(j);

  const msgs = h.card.of(j);
  const order = kinds(msgs).join(' ');
  assert.match(order, /^ack (step )+part done$/);
  assert.equal(msgs.find(m => m.k === 'step').text, 'Read: note.txt');
  for (const step of msgs.filter(m => m.k === 'step')) assert.ok(['Read: note.txt', 'pineapple'].includes(step.text), step.text);
  assert.deepEqual(msgs.filter(m => m.k === 'part').map(({ i, of, text }) => ({ i, of, text })), [{ i: 1, of: 1, text: 'pineapple' }]);
  const done = msgs.at(-1);
  assert.equal(done.ok, true);
  assert.equal(done.cost, 0.1568794);
  assert.ok(Number.isInteger(done.secs) && done.secs >= 0 && done.secs < 10);
  for (const m of msgs) {
    assert.equal(m.v, 1);
    assert.ok(Math.abs(m.ts - Date.now() / 1000) < 30);
    assert.ok(m.bytes <= 2800);
  }
  const ns = msgs.map(m => m.n);
  assert.deepEqual(ns, [...ns].sort((a, b) => a - b));
  assert.equal(new Set(ns).size, ns.length);
  assert.equal(h.ntfy.rejections.noFirebaseHeader, 0, 'every publish carries X-Firebase: no');
});

test('answers ping with hello, and reports busy while a job runs', async t => {
  const h = await harness({ command: agent('slow', reapAfter(t, join(tempDir(), 'pids'))) });
  t.after(() => h.close());
  await h.card.send({ k: 'ping' });
  const hello = await h.card.waitFor(inbox => inbox.find(m => m.k === 'hello'));
  assert.deepEqual({ ...hello, n: 0, ts: 0, relayId: '', bytes: 0 },
    { v: 1, k: 'hello', j: '', n: 0, ts: 0, name: 'Test agent', busy: false, version: lib.VERSION, relayId: '', bytes: 0 });

  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'wait' });
  await h.card.waitFor(() => h.card.of(j).some(m => m.k === 'step'));
  await h.card.send({ k: 'ping' });
  await h.card.waitFor(inbox => inbox.filter(m => m.k === 'hello').length === 2);
  assert.equal(h.card.inbox.filter(m => m.k === 'hello')[1].busy, true);
  await h.card.send({ k: 'cancel', j });
  await h.card.waitEnd(j);
});

test('refuses a second ask with busy, cancels the first and kills its process group', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('slow', pidfile) });
  t.after(() => h.close());
  const [first, second, third] = [jobId(), jobId(), jobId()];

  await h.card.send({ k: 'ask', j: first, text: 'long task' });
  await h.card.waitFor(() => h.card.of(first).some(m => m.k === 'step' && m.text === 'Bash: sleep 600'));
  const [agentPid, grandchildPid] = await pidsOf(pidfile);
  assert.ok(alive(agentPid) && alive(grandchildPid));

  await h.card.send({ k: 'ask', j: second, text: 'another' });
  await h.card.waitEnd(second);
  assert.deepEqual(h.card.of(second).map(({ k, text }) => ({ k, text })), [{ k: 'error', text: 'busy' }]);

  await h.card.send({ k: 'cancel', j: first });
  const end = await h.card.waitEnd(first);
  assert.deepEqual({ k: end.k, text: end.text }, { k: 'error', text: 'cancelled' });
  await until(() => !alive(agentPid) && !alive(grandchildPid), { what: 'process group to die' });

  await h.card.send({ k: 'ask', j: third, text: 'free again' });
  await h.card.waitFor(() => h.card.of(third).some(m => m.k === 'ack'));
  await h.card.send({ k: 'cancel', j: third });
  await h.card.waitEnd(third);
});

test('times out a job that ignores SIGTERM, escalating to SIGKILL', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('stubborn', pidfile), maxJobSecs: 1 });
  t.after(() => h.close());
  const j = jobId();
  const started = Date.now();
  await h.card.send({ k: 'ask', j, text: 'hang' });
  const [pid] = await pidsOf(pidfile);
  const end = await h.card.waitEnd(j);
  assert.deepEqual({ k: end.k, text: end.text }, { k: 'error', text: 'timed out after 1 s' });
  assert.ok(Date.now() - started >= 1000);
  assert.equal(alive(pid), false);
});

test('reports the last stderr line, scrubbed, when the agent exits without a result', async t => {
  const h = await harness({ command: agent('fail') });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'go' });
  const end = await h.card.waitEnd(j);
  assert.equal(end.k, 'error');
  assert.match(end.text, /^Error: API key \[redacted\] rejected, token=\[redacted\]$/);
});

test('reports a command that cannot start, without an ack', async t => {
  const h = await harness({ command: ['/nonexistent/claude', '-p', '{prompt}'] });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'go' });
  await h.card.waitEnd(j);
  assert.deepEqual(h.card.of(j).map(({ k, text }) => ({ k, text })), [{ k: 'error', text: 'cannot start the agent: ENOENT' }]);
  const k = jobId();
  await h.card.send({ k: 'ask', j: k, text: 'again' });
  await h.card.waitEnd(k);
  assert.notEqual(h.card.of(k)[0].text, 'busy');
});

test('passes the prompt as one argv element, never through a shell', async t => {
  const dir = tempDir();
  const marks = ['a', 'b', 'c', 'd'].map(n => join(dir, `pwned-${n}`));
  const prompt = `--help $(touch ${marks[0]}) \`touch ${marks[1]}\`; touch ${marks[2]} && touch ${marks[3]} "$HOME" '*'`;
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: prompt });
  await h.card.waitEnd(j);
  assert.equal(h.card.of(j).find(m => m.k === 'part').text, prompt);
  for (const mark of marks) assert.equal(existsSync(mark), false, `${mark} must not exist`);
});

test('refuses a task the OS cannot pass as an argument, and stays free', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const [bad, next] = [jobId(), jobId()];
  await h.card.send({ k: 'ask', j: bad, text: 'a\u0000b' });
  await h.card.waitEnd(bad);
  assert.deepEqual(h.card.of(bad).map(({ k, text }) => ({ k, text })), [{ k: 'error', text: 'cannot start the agent: ERR_INVALID_ARG_VALUE' }]);
  await h.card.send({ k: 'ask', j: next, text: 'fine' });
  await h.card.waitEnd(next);
  assert.equal(h.card.of(next).at(-1).k, 'done');
});

test('refuses empty and over-long asks', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const [empty, long, max] = [jobId(), jobId(), jobId()];
  await h.card.send({ k: 'ask', j: empty, text: '   ' });
  await h.card.send({ k: 'ask', j: long, text: 'x'.repeat(2001) });
  await h.card.waitEnd(long);
  await h.card.waitEnd(empty);
  assert.equal(h.card.of(empty)[0].text, 'empty task');
  assert.equal(h.card.of(long)[0].text, 'task too long (max 2000 characters)');
  await h.card.send({ k: 'ask', j: max, text: 'y'.repeat(2000) });
  await h.card.waitEnd(max);
  assert.equal(h.card.of(max).at(-1).k, 'done');
});

test('drops tampered, wrong-key, stale and replayed asks, logging only counts', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const task = 'zebra-confidential-task';
  const [tampered, wrongKey, stale, good] = [jobId(), jobId(), jobId(), jobId()];
  const body = h.card.seal({ k: 'ask', j: tampered, text: task });
  await h.card.post(body.slice(0, 40) + (body[40] === 'A' ? 'B' : 'A') + body.slice(41));
  await h.card.send({ k: 'ask', j: wrongKey, text: task }, { key: Buffer.alloc(32, 9) });
  await h.card.send({ k: 'ask', j: stale, text: task, ts: Math.floor(Date.now() / 1000) - 16 * 60 });
  const goodBody = h.card.seal({ k: 'ask', j: good, text: task });
  await h.card.post(goodBody);
  await h.card.waitEnd(good);
  await h.card.post(goodBody);
  await until(() => h.connector.drops.replay === 1, { what: 'replay drop' });

  assert.deepEqual(h.connector.drops, { decrypt: 2, malformed: 0, stale: 1, replay: 1 });
  for (const j of [tampered, wrongKey, stale]) assert.deepEqual(h.card.of(j), [], `no reply for ${j}`);
  assert.equal(h.card.of(good).filter(m => m.k === 'ack').length, 1);
  assert.ok(h.logs.some(l => /dropped 4 messages so far/.test(l)), h.logs.join('\n'));
  const code = lib.pairingCode(lib.readSecret(join(h.dir, 'secret')));
  for (const line of h.logs) {
    assert.ok(!line.includes(task) && !line.includes(code.slice(4)), `log leaks content: ${line}`);
  }
});

test('throttles steps to one per window and keeps only the latest', async t => {
  const h = await harness({ command: agent('steps', '5', '40', '900'), options: { stepEveryMs: 400 } });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'steps' });
  await h.card.waitEnd(j);
  const steps = h.card.of(j).filter(m => m.k === 'step');
  assert.deepEqual(steps.map(s => s.text), ['Bash: step-1', 'Bash: step-5']);
  const gap = h.ntfy.arrivedAt(steps[1].relayId) - h.ntfy.arrivedAt(steps[0].relayId);
  assert.ok(gap >= 380, `steps ${gap} ms apart`);
});

test('the default step window is 8 s', { timeout: 20_000 }, async t => {
  const h = await harness({ command: agent('steps', '3', '50', '9800'), options: { stepEveryMs: undefined } });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'steps' });
  await h.card.waitEnd(j, { timeoutMs: 15_000 });
  const steps = h.card.of(j).filter(m => m.k === 'step');
  assert.deepEqual(steps.map(s => s.text), ['Bash: step-1', 'Bash: step-3']);
  const gap = h.ntfy.arrivedAt(steps[1].relayId) - h.ntfy.arrivedAt(steps[0].relayId);
  // Never closer than the window at the relay. The upper bound only says the default is 8 s, with room for a slow runner.
  assert.ok(gap >= 8000 && gap < 10_500, `steps ${gap} ms apart`);
});

test('a pending step is dropped when the job ends, so done is last', async t => {
  const h = await harness({ command: agent('steps', '2', '20', '0'), options: { stepEveryMs: 600 } });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'steps' });
  await h.card.waitEnd(j);
  await wait(1000);
  await h.card.poll();
  assert.deepEqual(kinds(h.card.of(j)), ['ack', 'step', 'part', 'done']);
});

test('chunks a long multibyte report into sealed parts under the relay limit', async t => {
  const dir = tempDir();
  const report = 'ქართული ტექსტი "quoted"\n'.repeat(600);
  const h = await harness({ command: agent('report', writeFile(dir, 'report.txt', report)) });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'report' });
  await h.card.waitEnd(j);
  const parts = h.card.of(j).filter(m => m.k === 'part');
  assert.ok(parts.length > 1);
  assert.deepEqual(parts.map(p => p.i), parts.map((_, i) => i + 1));
  assert.ok(parts.every(p => p.of === parts.length && p.bytes <= 2800 && p.text.length <= 2400));
  assert.equal(parts.map(p => p.text).join(''), report);
  assert.equal(h.card.of(j).at(-1).k, 'done');
  assert.equal(h.ntfy.rejections.oversize, 0);
});

test('waits out a 429 on publish, honouring Retry-After', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  h.ntfy.inject429('POST', 1, { retryAfter: 1, path: `/${h.card.keys.topicDown}` });
  const sent = Date.now();
  await h.card.send({ k: 'ping' });
  const hello = await h.card.waitFor(inbox => inbox.find(m => m.k === 'hello'), { timeoutMs: 5000 });
  assert.ok(h.ntfy.arrivedAt(hello.relayId) - sent >= 950, 'retried only after Retry-After');
  const downPosts = h.ntfy.requests.filter(r => r.method === 'POST' && r.path === `/${h.card.keys.topicDown}`);
  assert.equal(downPosts.length, 2);
});

test('waits out a 429 on subscribe, then resumes', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const stream = `/${h.card.keys.topicUp}/json`;
  h.ntfy.inject429('GET', 1, { retryAfter: 1, path: stream });
  const dropped = Date.now();
  h.ntfy.dropStreams();
  await until(() => h.ntfy.subscribers(h.card.keys.topicUp) === 1, { what: 'resubscribe' });
  assert.ok(Date.now() - dropped >= 950);
  await h.card.send({ k: 'ping' });
  await h.card.waitFor(inbox => inbox.some(m => m.k === 'hello'));
});

test('reconnects with since=<last id> and picks up what arrived meanwhile', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const topic = h.card.keys.topicUp;
  await h.card.send({ k: 'ping' });
  await h.card.waitFor(inbox => inbox.some(m => m.k === 'hello'));
  const lastId = h.ntfy.messages(topic).at(-1).id;
  const before = h.ntfy.requests.length;

  h.ntfy.dropStreams();
  h.ntfy.store(topic, h.card.seal({ k: 'ping' }));
  await h.card.waitFor(inbox => inbox.filter(m => m.k === 'hello').length === 2);
  const reconnect = h.ntfy.requests.slice(before).find(r => r.method === 'GET' && r.path === `/${topic}/json`);
  assert.equal(reconnect.query.since, lastId);
  await wait(200);
  await h.card.poll();
  assert.equal(h.card.inbox.filter(m => m.k === 'hello').length, 2);
});

test('reconnects when the stream goes silent, but not while keepalives flow', async t => {
  const quiet = await harness({ command: agent('echo'), options: { idleMs: 250 } });
  t.after(() => quiet.close());
  const gets = h => h.ntfy.requests.filter(r => r.method === 'GET' && r.path === `/${h.card.keys.topicUp}/json`).length;
  await until(() => gets(quiet) >= 3, { what: 'idle reconnects' });

  const ntfy = await startMockNtfy({ keepaliveMs: 80 });
  t.after(() => ntfy.close());
  const live = await harness({ command: agent('echo'), options: { idleMs: 250 }, ntfy });
  t.after(() => live.close());
  await wait(1000);
  assert.equal(gets(live), 1);
});

test('survives a restart: resumes since, keeps n rising, and still rejects a replay', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const j = jobId();
  const body = h.card.seal({ k: 'ask', j, text: 'once' });
  await h.card.post(body);
  await h.card.waitEnd(j);
  const maxN = Math.max(...h.card.inbox.map(m => m.n));
  const lastId = h.ntfy.messages(h.card.keys.topicUp).at(-1).id;
  const before = h.ntfy.requests.length;

  await h.restart();
  const resubscribe = h.ntfy.requests.slice(before).find(r => r.method === 'GET');
  assert.equal(resubscribe.query.since, lastId);

  h.ntfy.store(h.card.keys.topicUp, body);
  await until(() => h.connector.drops.replay === 1, { what: 'replay after restart' });
  await h.card.send({ k: 'ping' });
  const hello = await h.card.waitFor(inbox => inbox.find(m => m.k === 'hello'));
  assert.ok(hello.n > maxN, `n ${hello.n} after ${maxN}`);
  assert.equal(h.card.of(j).filter(m => m.k === 'ack').length, 1);
});

test('finishes promptly when the agent exits but leaves a process holding its output', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('linger', pidfile), maxJobSecs: 20 });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'linger' });
  const end = await h.card.waitEnd(j, { timeoutMs: 4000 });
  assert.equal(end.k, 'done');
  assert.equal(h.card.of(j).find(m => m.k === 'part').text, 'lingered');
  const [, grandchild] = await pidsOf(pidfile);
  await until(() => !alive(grandchild), { what: 'leftover process to die' });
});

test('sends the report on the result event, then frees the slot from an agent that lingers', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('resultHang', pidfile), maxJobSecs: 20, options: { killGraceMs: 1500 } });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'answer and hang' });
  const done = await h.card.waitEnd(j, { timeoutMs: 1500 });
  const [pid] = await pidsOf(pidfile);
  assert.ok(alive(pid), 'done went out while the agent was still running');
  assert.equal(done.k, 'done');
  assert.equal(h.card.of(j).find(m => m.k === 'part').text, 'still here');
  await until(() => !alive(pid), { timeoutMs: 4000, what: 'lingering agent to be stopped' });
  await until(() => !h.connector.job, { timeoutMs: 2000, what: 'slot to free' });
});

test('frees the slot when a process that left the group keeps the output open', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('escape', pidfile), maxJobSecs: 20 });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'escape' });
  const end = await h.card.waitEnd(j, { timeoutMs: 3000 });
  assert.deepEqual({ k: end.k, text: end.text }, { k: 'error', text: 'agent exited with code 0 and no report' });
  await h.card.send({ k: 'ping' });
  const hello = await h.card.waitFor(inbox => inbox.find(m => m.k === 'hello'));
  assert.equal(hello.busy, false);
});

test('keeps steps one window apart while the relay is slow', async t => {
  const h = await harness({ command: agent('steps', '25', '60', '400'), options: { stepEveryMs: 300 } });
  t.after(() => h.close());
  h.ntfy.inject429('POST', 1, { retryAfter: 1, path: `/${h.card.keys.topicDown}` });
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'steps' });
  await h.card.waitEnd(j, { timeoutMs: 8000 });
  const arrivals = h.card.of(j).filter(m => m.k === 'step').map(m => h.ntfy.arrivedAt(m.relayId));
  assert.ok(arrivals.length >= 2, `${arrivals.length} steps`);
  const gaps = arrivals.slice(1).map((at, i) => at - arrivals[i]);
  assert.ok(gaps.every(gap => gap >= 280), `gaps ${gaps.join(', ')} ms`);
});

test('caps a huge Retry-After instead of retrying in a tight loop', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  h.ntfy.inject429('POST', 1, { retryAfter: 9_999_999, path: `/${h.card.keys.topicDown}` });
  await h.card.send({ k: 'ping' });
  await wait(1000);
  const posts = h.ntfy.requests.filter(r => r.method === 'POST' && r.path === `/${h.card.keys.topicDown}`);
  assert.equal(posts.length, 1);
});

test('survives tools named like Object.prototype methods', async t => {
  const h = await harness({ command: agent('badtool') });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'go' });
  await h.card.waitEnd(j);
  assert.equal(h.card.of(j).find(m => m.k === 'part').text, 'survived');
});

test('tells the Cardputer when the connector stops in the middle of a job', async t => {
  const pidfile = reapAfter(t, join(tempDir(), 'pids'));
  const h = await harness({ command: agent('slow', pidfile) });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'long' });
  await h.card.waitFor(() => h.card.of(j).some(m => m.k === 'step'));
  await h.connector.stop();
  const end = await h.card.waitEnd(j);
  assert.deepEqual({ k: end.k, text: end.text }, { k: 'error', text: 'connector stopped' });
});

test('stops without acting when the state file cannot be written', { skip: process.getuid?.() === 0 && 'root ignores directory modes' }, async t => {
  const mark = join(tempDir(), 'ran');
  const h = await harness({ command: agent('mark', mark) });
  t.after(() => h.close());
  t.after(() => chmodSync(h.dir, 0o755));
  chmodSync(h.dir, 0o555);
  await h.card.send({ k: 'ask', j: jobId(), text: 'must not run' });
  await assert.rejects(h.running, { code: 'EACCES' });
  await wait(300);
  assert.equal(existsSync(mark), false, 'the agent ran without the state on disk');
  assert.ok(!h.logs.some(l => / started /.test(l)), 'the agent was spawned without the state on disk');
  assert.deepEqual(await h.card.poll(), []);
  assert.ok(h.logs.some(l => /cannot write .*state\.json \(EACCES\); stopping/.test(l)), h.logs.join('\n'));
});

test('a reconnect before the first message still picks up what arrived in between', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const before = h.ntfy.requests.length;
  h.ntfy.dropStreams();
  h.ntfy.store(h.card.keys.topicUp, h.card.seal({ k: 'ping' }));
  await h.card.waitFor(inbox => inbox.some(m => m.k === 'hello'));
  const reconnect = h.ntfy.requests.slice(before).find(r => r.method === 'GET' && r.path === `/${h.card.keys.topicUp}/json`);
  assert.match(reconnect.query.since, /^\d+$/);
});

async function hostileRelay(handler) {
  const server = http.createServer(handler);
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  return { url: `http://127.0.0.1:${server.address().port}`, close: () => { server.closeAllConnections(); return new Promise(r => server.close(r)); } };
}

function connectorFor(t, relay, options = {}) {
  const dir = tempDir();
  lib.writeSecret(join(dir, 'secret'), randomBytes(32));
  writeFileSync(join(dir, 'c.json'), JSON.stringify({ relay, secretFile: 'secret', name: 'x', command: agent('echo'), stateFile: 'state.json' }));
  const logs = [];
  const connector = new lib.Connector(lib.loadConfig(join(dir, 'c.json')), { backoffMs: 50, maxBackoffMs: 200, log: l => logs.push(l), ...options });
  const running = connector.start().catch(() => {});
  t.after(async () => { await connector.stop(); await running; });
  return logs;
}

test('never follows a redirect from the relay', async t => {
  const target = { hits: 0 };
  const inside = await hostileRelay((req, res) => { target.hits++; res.end(); });
  t.after(() => inside.close());
  const relay = await hostileRelay((req, res) => res.writeHead(307, { Location: `${inside.url}/api/v1/namespaces/cms/pods` }).end());
  t.after(() => relay.close());

  const logs = connectorFor(t, relay.url);
  const controller = new AbortController();
  const publisher = new lib.Relay(relay.url, { signal: controller.signal, log: () => {}, backoffMs: 50, maxBackoffMs: 100 });
  const publishing = publisher.publish('cp-topic', 'body');
  await wait(600);
  controller.abort();
  await publishing;
  assert.equal(target.hits, 0);
  assert.ok(logs.some(l => /relay subscribe dropped/.test(l)), logs.join('\n'));
});

test('reconnects when the relay streams a line with no end', async t => {
  let gets = 0;
  const relay = await hostileRelay((req, res) => {
    gets++;
    res.writeHead(200);
    res.write('{"event":"open"}\n');
    const chunk = 'x'.repeat(16 * 1024);
    const pump = setInterval(() => res.write(chunk), 5);
    res.on('close', () => clearInterval(pump));
  });
  t.after(() => relay.close());
  const logs = connectorFor(t, relay.url);
  await until(() => gets >= 2, { what: 'reconnect after an over-long line' });
  assert.ok(logs.some(l => /over-long line/.test(l)), logs.join('\n'));
});
