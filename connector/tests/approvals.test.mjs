import assert from 'node:assert/strict';
import { existsSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { agent, harness, jobId, lib, tempDir, until } from './helpers.mjs';

const BASH = JSON.stringify({ command: 'kubectl get pods -n cms' });
const asked = (card, j) => card.of(j).filter(m => m.k === 'approval');
const decisions = (card, j) => JSON.parse(card.of(j).find(m => m.k === 'part').text);
const start = async (t, args, options = {}) => {
  const h = await harness({ command: agent(...args), approvals: true, options });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'go' });
  return { h, j };
};

test('an allowed approval reaches the agent with its input echoed back, and shows what was asked', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH]);
  const ask = await h.card.waitFor(inbox => inbox.find(m => m.j === j && m.k === 'approval'), { what: 'approval' });
  assert.deepEqual({ tool: ask.tool, detail: ask.detail }, { tool: 'Bash', detail: 'kubectl get pods -n cms' });
  assert.match(ask.req, /^[0-9a-f]{8}$/);
  await h.card.send({ k: 'approve', j, req: ask.req, allow: true });
  await h.card.waitEnd(j);
  const answer = decisions(h.card, j);
  assert.equal(answer.toolName, 'mcp__kloudputer__approve');
  assert.deepEqual(answer.listed, ['approve']);
  assert.deepEqual(answer.decisions, [{ behavior: 'allow', updatedInput: { command: 'kubectl get pods -n cms' } }]);
  assert.equal(h.card.of(j).at(-1).k, 'done');
});

test('a denied approval tells the agent no', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH]);
  const ask = await h.card.waitFor(inbox => inbox.find(m => m.j === j && m.k === 'approval'));
  await h.card.send({ k: 'approve', j, req: ask.req, allow: false });
  await h.card.waitEnd(j);
  assert.deepEqual(decisions(h.card, j).decisions, [{ behavior: 'deny', message: 'The owner denied this from the Cardputer.' }]);
});

test('with no answer the request is denied after the timeout', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH], { approvalMs: 400 });
  await h.card.waitEnd(j);
  const [decision] = decisions(h.card, j).decisions;
  assert.deepEqual(decision, { behavior: 'deny', message: 'No answer from the Cardputer in time, so it was denied.' });
});

test('simultaneous requests get their own request ids and answers', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH, 'Edit', JSON.stringify({ file_path: '/etc/hosts' })]);
  await h.card.waitFor(box => box.filter(m => m.j === j && m.k === 'approval').length === 2);
  const [bash, edit] = ['Bash', 'Edit'].map(tool => h.card.inbox.find(m => m.k === 'approval' && m.tool === tool));
  assert.notEqual(bash.req, edit.req);
  assert.equal(edit.detail, '/etc/hosts');
  await h.card.send({ k: 'approve', j, req: edit.req, allow: false });
  await h.card.send({ k: 'approve', j, req: bash.req, allow: true });
  await h.card.waitEnd(j);
  const [first, second] = decisions(h.card, j).decisions;
  assert.equal(first.behavior, 'allow', 'the Bash call was allowed');
  assert.equal(second.behavior, 'deny', 'the Edit call was denied');
});

test('answers that do not match a waiting request change nothing', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH], { approvalMs: 3000 });
  const ask = await h.card.waitFor(inbox => inbox.find(m => m.j === j && m.k === 'approval'));
  const other = jobId();
  await h.card.send({ k: 'approve', j, req: '00000000', allow: true });
  await h.card.send({ k: 'approve', j: other, req: ask.req, allow: true });
  await h.card.send({ k: 'approve', j, req: ask.req, allow: 'yes' });
  await h.card.send({ k: 'approve', j, req: ask.req, allow: 1 });
  await new Promise(resolve => setTimeout(resolve, 400));
  assert.equal(h.card.of(j).some(m => m.k === 'done'), false, 'still waiting');
  await h.card.send({ k: 'approve', j, req: ask.req, allow: false });
  await h.card.waitEnd(j);
  assert.equal(decisions(h.card, j).decisions[0].behavior, 'deny');
});

test('an answer arriving twice does not flip the first one', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH]);
  const ask = await h.card.waitFor(inbox => inbox.find(m => m.j === j && m.k === 'approval'));
  await h.card.send({ k: 'approve', j, req: ask.req, allow: false });
  await h.card.send({ k: 'approve', j, req: ask.req, allow: true });
  await h.card.waitEnd(j);
  assert.equal(decisions(h.card, j).decisions[0].behavior, 'deny');
});

test('cancelling a job with a waiting request denies it and lets the job end', async t => {
  const { h, j } = await start(t, ['perm', 'Bash', BASH]);
  await h.card.waitFor(inbox => inbox.find(m => m.j === j && m.k === 'approval'));
  await h.card.send({ k: 'cancel', j });
  await h.card.waitEnd(j);
  await until(() => h.connector.job === null, { what: 'job to close' });
});

test('closing the approvals denies what is waiting and removes the socket folder', async () => {
  const sent = [];
  const approvals = new lib.Approvals({ j: 'aaaaaaaa', send: msg => sent.push(msg), timeoutMs: 60_000, log: () => {} });
  const folder = dirname(approvals.socketPath);
  await until(() => existsSync(approvals.socketPath), { what: 'the socket' });
  assert.equal(statSync(folder).mode & 0o077, 0, 'only the owner can enter the folder');
  const { connect } = await import('node:net');
  const socket = connect(approvals.socketPath);
  let reply = '';
  socket.setEncoding('utf8');
  socket.on('data', chunk => { reply += chunk; });
  socket.write(JSON.stringify({ tool_name: 'Bash', input: { command: 'ls' } }) + '\n');
  await until(() => sent.length === 1 && approvals.pending === 1, { what: 'the request' });
  approvals.close();
  await until(() => reply.includes('\n'), { what: 'the denial' });
  assert.equal(JSON.parse(reply).allow, false);
  assert.equal(approvals.pending, 0);
  assert.equal(existsSync(folder), false, 'the folder is gone');
  approvals.close();
});

test('without approvals the agent gets no prompt tool and nothing is asked', async t => {
  const h = await harness({ command: agent('echo') });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'go' });
  await h.card.waitEnd(j);
  assert.equal(asked(h.card, j).length, 0);
});

test('with approvals on, the options go before the prompt and the -- that precedes it', async t => {
  const h = await harness({ command: [process.execPath, new URL('./fake-agent.mjs', import.meta.url).pathname, 'argv', '--', '{prompt}'], approvals: true });
  t.after(() => h.close());
  const j = jobId();
  await h.card.send({ k: 'ask', j, text: 'hello there' });
  await h.card.waitEnd(j);
  const argv = JSON.parse(h.card.of(j).find(m => m.k === 'part').text);
  const dashes = argv.indexOf('--');
  assert.equal(argv.at(-1), 'hello there');
  assert.equal(dashes, argv.length - 2, 'the prompt stays after the --');
  assert.ok(argv.indexOf('--permission-prompt-tool') < dashes && argv.indexOf('--mcp-config') < dashes);
});

test('approvals must be true or false in the config file', () => {
  const dir = tempDir();
  const secretFile = join(dir, 'secret');
  lib.writeSecret(secretFile, Buffer.alloc(32, 7));
  const load = approvals => {
    const file = join(dir, 'config.json');
    writeFileSync(file, JSON.stringify({ secretFile, name: 'x', command: ['claude', '{prompt}'], stateFile: join(dir, 's.json'), ...(approvals === undefined ? {} : { approvals }) }));
    return lib.loadConfig(file);
  };
  assert.equal(load().approvals, false, 'off unless asked for');
  assert.equal(load(true).approvals, true);
  assert.throws(() => load('yes'), /"approvals" must be true or false/);
  assert.throws(() => load(1), /"approvals" must be true or false/);
});

test('describeApproval keeps paths whole, flags cut text, flattens lines and scrubs secrets', () => {
  assert.deepEqual(lib.describeApproval('Edit', { file_path: '/etc/hermes/settings.json' }), { tool: 'Edit', detail: '/etc/hermes/settings.json' });
  assert.equal(lib.describeApproval('Bash', { command: 'a\nb' }).detail, 'a \\n b');
  assert.match(lib.describeApproval('Bash', { command: 'curl -H "Authorization: Bearer abcdefghijklmnop1234" x' }).detail, /\[redacted\]/);
  const long = lib.describeApproval('Bash', { command: 'echo ' + 'a'.repeat(500) }).detail;
  assert.equal(long.length, 200);
  const hidden = Number(/\[\+(\d+) more\]$/.exec(long)[1]);
  assert.equal(long.indexOf(' [+') + hidden, 505, 'the marker counts exactly what was cut');
  assert.equal(lib.describeApproval('mcp__x__y', { a: 1 }).detail, '{"a":1}');
  assert.equal(lib.describeApproval('X'.repeat(100), {}).tool.length, 40);
  assert.equal(lib.describeApproval('Bash', {}).detail, '');
});

test('a request the MCP tool cannot deliver is denied, not allowed', async () => {
  const { PassThrough } = await import('node:stream');
  const input = new PassThrough();
  const output = new PassThrough();
  const done = lib.runMcp('/nonexistent/approve.sock', { input, output, retryMs: 100 });
  const replies = [];
  output.on('data', chunk => replies.push(...String(chunk).split('\n').filter(Boolean).map(JSON.parse)));
  input.write(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/call', params: { name: 'approve', arguments: { tool_name: 'Bash', input: { command: 'ls' } } } }) + '\n');
  await until(() => replies.length === 1, { what: 'a reply' });
  const verdict = JSON.parse(replies[0].result.content[0].text);
  assert.equal(verdict.behavior, 'deny');
  assert.match(verdict.message, /could not be reached/);
  input.write(JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/call', params: { name: 'other', arguments: {} } }) + '\n');
  await until(() => replies.length === 2, { what: 'a second reply' });
  assert.equal(JSON.parse(replies[1].result.content[0].text).behavior, 'deny');
  input.end();
  await done;
});
