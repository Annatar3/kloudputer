import { randomBytes } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startMockNtfy } from './mock-ntfy.mjs';

// The mutation runner points this at a mutant copy.
export const CONNECTOR = process.env.CONNECTOR_UNDER_TEST ?? fileURLToPath(new URL('../kloudputer-connector.mjs', import.meta.url));
export const lib = await import(CONNECTOR);
export const FAKE_AGENT = fileURLToPath(new URL('./fake-agent.mjs', import.meta.url));
export const FIXTURES = fileURLToPath(new URL('./fixtures/', import.meta.url));

export const FAST = { stepEveryMs: 300, backoffMs: 50, maxBackoffMs: 400, idleMs: 3000, killGraceMs: 400, dropLogEveryMs: 0 };

export const tempDir = () => mkdtempSync(join(tmpdir(), 'kloudputer-'));
export const wait = ms => new Promise(r => setTimeout(r, ms));

export async function until(check, { timeoutMs = 5000, what = 'condition' } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const value = await check();
    if (value) return value;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await wait(25);
  }
}

// Plays the Cardputer: seals up messages, polls the down topic over HTTP, and decrypts every reply.
export class Card {
  inbox = [];
  #since = 'all';
  #n = 0;

  constructor(relay, code) {
    this.relay = relay;
    this.keys = lib.deriveKeys(Buffer.from(code.slice('cp1-'.length), 'base64url'));
  }

  seal(fields, { key = this.keys.keyUp, topic = this.keys.topicUp } = {}) {
    const msg = { v: 1, n: ++this.#n, ts: Math.floor(Date.now() / 1000), j: '', ...fields };
    return lib.seal(key, topic, JSON.stringify(msg));
  }

  async post(body) {
    const res = await fetch(`${this.relay}/${this.keys.topicUp}`, { method: 'POST', body, headers: { 'X-Firebase': 'no' } });
    if (!res.ok) throw new Error(`card publish: HTTP ${res.status}`);
  }

  send(fields, options) {
    return this.post(this.seal(fields, options));
  }

  async poll() {
    const res = await fetch(`${this.relay}/${this.keys.topicDown}/json?poll=1&since=${this.#since}`);
    for (const line of (await res.text()).split('\n').filter(Boolean)) {
      const event = JSON.parse(line);
      this.#since = event.id;
      const plaintext = lib.unseal(this.keys.keyDown, this.keys.topicDown, event.message);
      if (plaintext === null) throw new Error('a down message failed to decrypt');
      this.inbox.push({ ...JSON.parse(plaintext), relayId: event.id, bytes: Buffer.byteLength(plaintext) });
    }
    return this.inbox;
  }

  of(j) {
    return this.inbox.filter(m => m.j === j);
  }

  waitFor(predicate, options) {
    return until(async () => predicate(await this.poll()), options);
  }

  waitEnd(j, options) {
    return this.waitFor(inbox => inbox.find(m => m.j === j && (m.k === 'done' || m.k === 'error')), { what: `end of job ${j}`, ...options });
  }
}

export const jobId = () => randomBytes(4).toString('hex');

// One connector, a mock relay, and a paired Card, all in a fresh temp dir.
export async function harness({ command, maxJobSecs = 30, options = {}, ntfy, approvals } = {}) {
  const dir = tempDir();
  const relay = ntfy ?? (await startMockNtfy());
  const secretFile = join(dir, 'secret');
  const configFile = join(dir, 'config.json');
  const logs = [];
  const start = () => {
    const connector = new lib.Connector(lib.loadConfig(configFile), { ...FAST, log: line => logs.push(line), ...options });
    const running = connector.start();
    return { connector, running };
  };
  let card;
  let current;
  try {
    lib.writeSecret(secretFile, randomBytes(32));
    writeFileSync(configFile, JSON.stringify({
      relay: relay.url, secretFile, name: 'Test agent', cwd: dir, command, maxJobSecs, stateFile: join(dir, 'state.json'), ...(approvals === undefined ? {} : { approvals }),
    }));
    card = new Card(relay.url, lib.pairingCode(lib.readSecret(secretFile)));
    current = start();
    await until(() => relay.subscribers(card.keys.topicUp) > 0, { what: 'connector subscription' });
  } catch (err) {
    current?.connector.stop();
    if (!ntfy) await relay.close();
    throw err;
  }
  return {
    dir, ntfy: relay, card, logs, configFile,
    get connector() { return current.connector; },
    get running() { return current.running; },
    async restart() {
      await current.connector.stop();
      await current.running;
      await until(() => relay.subscribers(card.keys.topicUp) === 0, { what: 'old subscription to close' });
      current = start();
      await until(() => relay.subscribers(card.keys.topicUp) > 0, { what: 'connector resubscription' });
    },
    async close() {
      await current.connector.stop();
      await current.running.catch(() => {});
      if (!ntfy) await relay.close();
    },
  };
}

// Kills whatever a fake agent recorded in its pidfile, so a broken connector cannot leak processes past the test.
export function reapAfter(t, pidfile) {
  t.after(() => {
    if (!existsSync(pidfile)) return;
    for (const pid of JSON.parse(readFileSync(pidfile, 'utf8'))) {
      try { process.kill(pid, 'SIGKILL'); } catch { /* already gone */ }
    }
  });
  return pidfile;
}

export const agent = (...args) => [process.execPath, FAKE_AGENT, ...args, '{prompt}'];
export const fixture = name => join(FIXTURES, name);
export const writeFile = (dir, name, content) => {
  const file = join(dir, name);
  writeFileSync(file, content);
  return file;
};
