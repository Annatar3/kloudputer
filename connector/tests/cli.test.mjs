import assert from 'node:assert/strict';
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { chmodSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { startMockNtfy } from './mock-ntfy.mjs';
import { CONNECTOR, Card, agent, jobId, lib, tempDir, until } from './helpers.mjs';

const cli = (...args) => spawnSync(process.execPath, [CONNECTOR, ...args], { encoding: 'utf8' });
const pairUnderUmask000 = (file, ...extra) => spawnSync('sh', ['-c', 'umask 000 && exec "$@"', 'sh', process.execPath, CONNECTOR, 'pair', '--secret', file, ...extra], { encoding: 'utf8' });
const mode = file => statSync(file).mode & 0o777;

test('pair creates a 0600 secret even under umask 000 and prints its code; code prints it again', () => {
  const file = join(tempDir(), 'secret');
  const paired = pairUnderUmask000(file);
  assert.equal(paired.status, 0, paired.stderr);
  assert.equal(mode(file), 0o600);
  const secret = readFileSync(file);
  assert.equal(secret.length, 32);
  const code = paired.stdout.trim();
  assert.equal(code, lib.pairingCode(secret));
  assert.match(code, /^cp1-[A-Za-z0-9_-]{43}$/);
  assert.ok(!paired.stderr.includes(code.slice(4)), 'the code goes to stdout only');
  assert.equal(cli('code', '--secret', file).stdout.trim(), code);
});

test('pair refuses to overwrite without --force, and --force replaces at 0600', () => {
  const file = join(tempDir(), 'secret');
  assert.equal(pairUnderUmask000(file).status, 0);
  const original = readFileSync(file);
  const again = pairUnderUmask000(file);
  assert.notEqual(again.status, 0);
  assert.match(again.stderr, /exists; --force replaces it/);
  assert.deepEqual(readFileSync(file), original);

  chmodSync(file, 0o644);
  const forced = pairUnderUmask000(file, '--force');
  assert.equal(forced.status, 0, forced.stderr);
  assert.notDeepEqual(readFileSync(file), original);
  assert.equal(mode(file), 0o600);
  assert.equal(forced.stdout.trim(), lib.pairingCode(readFileSync(file)));
});

test('pair --force recovers from a leftover temp file', () => {
  const file = join(tempDir(), 'secret');
  assert.equal(pairUnderUmask000(file).status, 0);
  writeFileSync(`${file}.tmp`, 'junk from a crashed run');
  const forced = pairUnderUmask000(file, '--force');
  assert.equal(forced.status, 0, forced.stderr);
  assert.equal(readFileSync(file).length, 32);
});

test('refuses a secret that other users can read', () => {
  const file = join(tempDir(), 'secret');
  lib.writeSecret(file, Buffer.alloc(32, 1));
  chmodSync(file, 0o640);
  const res = cli('code', '--secret', file);
  assert.notEqual(res.status, 0);
  assert.match(res.stderr, /readable by other users/);
  assert.equal(res.stdout, '');
});

test('rejects configs that would misbehave', () => {
  const dir = tempDir();
  lib.writeSecret(join(dir, 'secret'), Buffer.alloc(32, 2));
  const base = { secretFile: 'secret', name: 'x', command: ['claude', '-p', '--', '{prompt}'], stateFile: 'state.json' };
  const load = cfg => {
    writeFileSync(join(dir, 'c.json'), JSON.stringify(cfg));
    return () => lib.loadConfig(join(dir, 'c.json'));
  };
  const ok = load(base)();
  assert.equal(ok.relay, 'https://ntfy.sh');
  assert.equal(ok.maxJobSecs, 900);
  assert.equal(ok.stateFile, join(dir, 'state.json'));
  assert.throws(load({ ...base, maxJobSec: 5 }), /unknown key "maxJobSec"/);
  assert.throws(load({ ...base, command: ['claude', '-p'] }), /must contain a "\{prompt\}" element/);
  assert.throws(load({ ...base, command: ['{prompt}'] }), /must not run the prompt as the program/);
  assert.throws(load({ ...base, command: 'claude -p {prompt}' }), /array of strings/);
  assert.throws(load({ ...base, env: { PATH: 1 } }), /"env"/);
  assert.throws(load({ ...base, relay: 'ntfy.sh' }), /http\(s\) URL/);
  assert.throws(load({ ...base, relay: 'https://user:pw@ntfy.example' }), /must not carry a user name or password/);
  assert.throws(load({ ...base, maxJobSecs: 3e6 }), /"maxJobSecs" must be between 0 and 86400/);
});

test('the shipped systemd example loads and agrees with its unit', () => {
  const dir = tempDir();
  lib.writeSecret(join(dir, 'secret'), Buffer.alloc(32, 3));
  const example = JSON.parse(readFileSync(new URL('../deploy/systemd/connector.json', import.meta.url), 'utf8'));
  writeFileSync(join(dir, 'connector.json'), JSON.stringify({ ...example, secretFile: join(dir, 'secret') }));
  const config = lib.loadConfig(join(dir, 'connector.json'));
  assert.deepEqual(config.command.slice(-2), ['--', '{prompt}'], 'a task starting with "-" must not become a claude option');
  assert.equal(config.cwd, '/srv/agent/workspace');

  const unit = readFileSync(new URL('../deploy/systemd/kloudputer-connector.service', import.meta.url), 'utf8');
  assert.match(unit, /^ExecStart=\S+ \S+\/kloudputer-connector\.mjs run --config \/etc\/kloudputer\/connector\.json$/m);
  const stateDir = /^StateDirectory=(\S+)$/m.exec(unit)[1];
  assert.equal(config.stateFile, `/var/lib/${stateDir}/state.json`, 'the state file must sit in the directory systemd makes writable');
  assert.ok(unit.includes(`EnvironmentFile=-/etc/kloudputer/agent.env`));
  assert.equal(example.secretFile, '/etc/kloudputer/connector.secret');
});

test('run serves a paired card from the real CLI and stops on SIGTERM', async t => {
  const dir = tempDir();
  const ntfy = await startMockNtfy();
  t.after(() => ntfy.close());
  const secretFile = join(dir, 'secret');
  const code = execFileSync(process.execPath, [CONNECTOR, 'pair', '--secret', secretFile], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
  const configFile = join(dir, 'cardputer.json');
  writeFileSync(configFile, JSON.stringify({
    relay: `${ntfy.url}/`, secretFile, name: 'CLI agent', cwd: dir, command: agent('echo'), stateFile: join(dir, 'state.json'),
  }));
  const proc = spawn(process.execPath, [CONNECTOR, 'run', '--config', configFile], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  proc.stdout.on('data', d => { output += d; });
  proc.stderr.on('data', d => { output += d; });
  const exited = new Promise(r => proc.on('exit', (status, signal) => r({ status, signal })));
  t.after(() => proc.kill('SIGKILL'));

  const card = new Card(ntfy.url, code);
  await until(() => ntfy.subscribers(card.keys.topicUp) === 1, { what: 'CLI subscription' });
  await card.send({ k: 'ping' });
  const hello = await card.waitFor(inbox => inbox.find(m => m.k === 'hello'));
  assert.equal(hello.name, 'CLI agent');
  const j = jobId();
  await card.send({ k: 'ask', j, text: 'from the CLI' });
  await card.waitEnd(j);
  assert.deepEqual(card.of(j).map(m => m.k), ['ack', 'part', 'done']);

  proc.kill('SIGTERM');
  assert.deepEqual(await exited, { status: 0, signal: null });
  assert.match(output, /listening on 127\.0\.0\.1/);
  assert.ok(!output.includes(code.slice(4)), 'run never prints the secret');
});
