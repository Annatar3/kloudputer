#!/usr/bin/env node
// Kloudputer agent connector: runs Cardputer tasks through a local agent command over an untrusted relay.
// Protocol: PROTOCOL.md (v1). Node 18.3+ standard library only.
import { spawn } from 'node:child_process';
import { createCipheriv, createDecipheriv, hkdfSync, randomBytes } from 'node:crypto';
import { closeSync, mkdtempSync, openSync, readFileSync, realpathSync, renameSync, rmSync, statSync, writeFileSync, writeSync } from 'node:fs';
import { createServer, connect } from 'node:net';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

export const VERSION = '0.1.0';
export const LIMITS = { plaintextBytes: 2800, askChars: 2000, stepChars: 120, partChars: 2400, maxParts: 20, errorChars: 300, approvalToolChars: 40, approvalDetailChars: 200 };
const APPROVAL_MS = 5 * 60_000;
const REQ_ID = /^[0-9a-f]{8}$/;
const SCRIPT = fileURLToPath(import.meta.url);
const TS_PAST_SECS = 15 * 60;
const TS_FUTURE_SECS = 2 * 60;
const JOB_ID = /^[0-9a-f]{8}$/;
const MAX_JOB_SECS = 24 * 60 * 60;
const MAX_RELAY_LINE = 64 * 1024;
const MAX_RETRY_AFTER_MS = 15 * 60_000;
const nowSecs = () => Date.now() / 1000;

// ---------------------------------------------------------------- keys and envelope

// The salt is a frozen wire-format constant from before the rename; changing it would unpair every Cardputer.
export function deriveKeys(secret) {
  const derive = (label, length) => Buffer.from(hkdfSync('sha256', secret, 'claudeputer-v1', label, length));
  const topic = label => 'cp-' + derive(label, 16).toString('base64url');
  return { topicUp: topic('topic-up'), topicDown: topic('topic-down'), keyUp: derive('key-up', 32), keyDown: derive('key-down', 32) };
}

export const pairingCode = secret => 'cp1-' + secret.toString('base64url');

export function seal(key, topic, plaintext, nonce = randomBytes(12)) {
  const cipher = createCipheriv('aes-256-gcm', key, nonce);
  cipher.setAAD(Buffer.from(topic, 'ascii'));
  return Buffer.concat([nonce, cipher.update(plaintext, 'utf8'), cipher.final(), cipher.getAuthTag()]).toString('base64url');
}

// Returns the plaintext, or null when the body was not sealed with this key for this topic.
export function unseal(key, topic, body) {
  const raw = Buffer.from(String(body), 'base64url');
  if (raw.length < 12 + 16) return null;
  const decipher = createDecipheriv('aes-256-gcm', key, raw.subarray(0, 12), { authTagLength: 16 });
  decipher.setAAD(Buffer.from(topic, 'ascii'));
  decipher.setAuthTag(raw.subarray(raw.length - 16));
  try {
    return Buffer.concat([decipher.update(raw.subarray(12, raw.length - 16)), decipher.final()]).toString('utf8');
  } catch {
    return null;
  }
}

// The receive-side checks from PROTOCOL.md. `seen` maps "j:n" to the message ts.
export function checkInbound(body, { key, topic, seen, now = nowSecs() }) {
  const text = unseal(key, topic, body);
  if (text === null) return { drop: 'decrypt' };
  let msg;
  try { msg = JSON.parse(text); } catch { return { drop: 'malformed' }; }
  const wellFormed = msg && typeof msg === 'object' && msg.v === 1 && typeof msg.k === 'string'
    && (msg.j === '' || JOB_ID.test(msg.j)) && Number.isSafeInteger(msg.n) && Number.isFinite(msg.ts);
  if (!wellFormed) return { drop: 'malformed' };
  if (now - msg.ts > TS_PAST_SECS || msg.ts - now > TS_FUTURE_SECS) return { drop: 'stale' };
  const id = `${msg.j}:${msg.n}`;
  if (seen.has(id)) return { drop: 'replay' };
  return { msg, id };
}

// ---------------------------------------------------------------- text shaping

export function clip(text, max) {
  if (text.length <= max) return text;
  let out = '';
  for (const ch of text) {
    if (out.length + ch.length > max) break;
    out += ch;
  }
  return out;
}

// Keep every pattern linear: an unbounded lazy prefix makes a long agent line cost seconds of blocked event loop.
const SECRET_KEY = '(?:token|secret|password|passwd|api[_-]?key|access[_-]?key|private[_-]?key|authorization|credentials?)';
const SECRET_SHAPES = [
  [/\b(?:sk-ant-|sk-|ghp_|gho_|ghs_|ghu_|github_pat_|glpat-|xox[abprs]-|AKIA|ASIA|AIza|ya29\.)[A-Za-z0-9._-]{8,}/g, '[redacted]'],
  [/\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9._-]+/g, '[redacted]'],
  [/\b(bearer|basic)\s+[A-Za-z0-9._~+/=-]{8,}/gi, '$1 [redacted]'],
  [/\b([a-z][a-z0-9+.-]{0,20}:\/\/[^\s:/@]{1,64}:)[^\s@/]+@/gi, '$1[redacted]@'],
  [new RegExp(`(--?${SECRET_KEY}[=\\s]+)("[^"]*"|'[^']*'|\\S+)`, 'gi'), '$1[redacted]'],
  [new RegExp(`\\b([A-Za-z0-9_-]{0,40}?${SECRET_KEY}["']?\\s*[:=]\\s*)("[^"]*"|'[^']*'|\\S+)`, 'gi'), '$1[redacted]'],
];

export function scrub(text) {
  const out = SECRET_SHAPES.reduce((s, [re, to]) => s.replace(re, to), text);
  return out.replace(/[A-Za-z0-9+/=_-]{32,}/g, run => (/\d/.test(run) && /[A-Za-z]/.test(run) ? '[redacted]' : run));
}

// Clipping before scrubbing bounds the work; clipping after keeps the promised length.
const safeLine = (text, max) => clip(scrub(clip(text, max * 4)), max);

const firstLine = value => String(value ?? '').trim().split('\n', 1)[0].trim();
const fileName = value => basename(String(value ?? ''));

const STEP_DETAIL = {
  Bash: input => input.command,
  Read: input => fileName(input.file_path),
  Edit: input => fileName(input.file_path),
  Write: input => fileName(input.file_path),
  NotebookEdit: input => fileName(input.notebook_path),
  Grep: input => input.pattern,
  Glob: input => input.pattern,
  WebFetch: input => input.url,
  WebSearch: input => input.query,
  Task: input => input.description,
  Agent: input => input.description,
};

// One stream-json event from `claude -p --output-format stream-json --verbose` to a progress line, or null.
export function stepText(event) {
  if (event?.type === 'system' && event.subtype === 'permission_denied') return `Denied: ${event.tool_name}`;
  const blocks = event?.type === 'assistant' && Array.isArray(event.message?.content) ? event.message.content : [];
  const tool = blocks.findLast(block => block?.type === 'tool_use');
  if (tool) {
    const detail = Object.hasOwn(STEP_DETAIL, tool.name) ? firstLine(STEP_DETAIL[tool.name](tool.input ?? {})) : '';
    return detail ? `${tool.name}: ${detail}` : String(tool.name);
  }
  const text = blocks.find(block => block?.type === 'text' && firstLine(block.text));
  return text ? firstLine(text.text) : null;
}

// What the Cardputer shows when the agent wants permission. Unlike a step it keeps the whole path and the whole command,
// because the owner is deciding on it; a cut one says how much is missing.
const APPROVAL_DETAIL = {
  Bash: input => input.command,
  Read: input => input.file_path,
  Edit: input => input.file_path,
  Write: input => input.file_path,
  NotebookEdit: input => input.notebook_path,
  Grep: input => input.pattern,
  Glob: input => input.pattern,
  WebFetch: input => input.url,
  WebSearch: input => input.query,
  Task: input => input.description,
  Agent: input => input.description,
};

export function describeApproval(toolName, input) {
  const max = LIMITS.approvalDetailChars;
  const given = Object.hasOwn(APPROVAL_DETAIL, toolName) ? APPROVAL_DETAIL[toolName](input ?? {}) : JSON.stringify(input ?? {});
  const flat = String(given ?? '').trim().replace(/\s*\n\s*/g, ' \\n ');
  const shown = scrub(clip(flat, max * 4));
  const detail = shown.length <= max ? shown : (() => {
    const markerFor = kept => ` [+${flat.length - kept} more]`;
    const kept = max - markerFor(max).length;
    return clip(shown, kept) + markerFor(kept);
  })();
  return { tool: safeLine(String(toolName), LIMITS.approvalToolChars), detail };
}

// Worst-case bytes of a sealed `part` plaintext around its text.
const PART_OVERHEAD = Buffer.byteLength(JSON.stringify({
  v: 1, k: 'part', j: '00000000', n: Number.MAX_SAFE_INTEGER, ts: 9999999999, i: LIMITS.maxParts, of: LIMITS.maxParts, text: '',
}));

// Each chunk stays within both limits: partChars characters, and plaintextBytes once JSON-escaped and UTF-8 encoded.
export function chunkReport(text) {
  const chunks = [];
  let chunk = '';
  let bytes = 0;
  for (const ch of text) {
    const chBytes = Buffer.byteLength(JSON.stringify(ch)) - 2;
    if (chunk.length + ch.length > LIMITS.partChars || PART_OVERHEAD + bytes + chBytes > LIMITS.plaintextBytes) {
      chunks.push(chunk);
      chunk = '';
      bytes = 0;
    }
    chunk += ch;
    bytes += chBytes;
  }
  if (chunk) chunks.push(chunk);
  if (chunks.length <= LIMITS.maxParts) return chunks;
  const kept = chunks.slice(0, LIMITS.maxParts - 1);
  const omitted = text.length - kept.reduce((sum, c) => sum + c.length, 0);
  return [...kept, `[report cut: ${omitted} more characters on the agent host]`];
}

// The messages that close a job, from what the job observed.
export function finalMessages({ j, secs, result, stopReason, stderrTail, spawnError, code, signal, maxJobSecs }) {
  if (result) {
    const ok = result.subtype === 'success' && !result.is_error;
    const report = typeof result.result === 'string' && result.result ? result.result : ok ? '' : `stopped: ${result.subtype}`;
    const chunks = chunkReport(report);
    const cost = typeof result.total_cost_usd === 'number' ? result.total_cost_usd : null;
    return [...chunks.map((text, i) => ({ k: 'part', j, i: i + 1, of: chunks.length, text })), { k: 'done', j, ok, secs, cost }];
  }
  const why = stopReason === 'cancel' ? 'cancelled'
    : stopReason === 'timeout' ? `timed out after ${maxJobSecs} s`
    : stopReason === 'shutdown' ? 'connector stopped'
    : spawnError ? `cannot start the agent: ${spawnError}`
    : stderrTail ? stderrTail
    : signal ? `agent killed by ${signal}` : `agent exited with code ${code} and no report`;
  return [{ k: 'error', j, text: safeLine(why, LIMITS.errorChars) }];
}

// ---------------------------------------------------------------- relay

const retryAfterMs = res => {
  const header = res.headers.get('retry-after');
  if (!header) return 0;
  const secs = Number(header);
  const ms = Number.isFinite(secs) ? secs * 1000 : Date.parse(header) - Date.now() || 0;
  return Math.min(Math.max(0, ms), MAX_RETRY_AFTER_MS);
};

export class Relay {
  constructor(base, { signal, log, backoffMs = 1000, maxBackoffMs = 60_000, idleMs = 90_000 }) {
    Object.assign(this, { base, signal, log, backoffMs, maxBackoffMs, idleMs });
  }

  #backoff(attempt) {
    return Math.min(this.maxBackoffMs, this.backoffMs * 2 ** attempt);
  }

  async #sleep(ms) {
    await delay(ms, undefined, { signal: this.signal }).catch(() => {});
  }

  // Retries until delivered or refused outright; the outbox waits, so order is kept.
  async publish(topic, body) {
    for (let attempt = 0; !this.signal.aborted; attempt++) {
      let wait;
      const request = new AbortController();
      const abort = () => request.abort();
      const timeout = setTimeout(abort, 30_000);
      this.signal.addEventListener('abort', abort, { once: true });
      try {
        // A redirect from an untrusted relay would aim this host's requests at internal addresses.
        const res = await fetch(`${this.base}/${topic}`, {
          method: 'POST', body, headers: { 'X-Firebase': 'no', 'Content-Type': 'text/plain' }, redirect: 'error', signal: request.signal,
        });
        await res.body?.cancel();
        if (res.ok) return true;
        if (res.status === 429) wait = Math.max(retryAfterMs(res), this.#backoff(attempt));
        else if (res.status >= 500) wait = this.#backoff(attempt);
        else {
          this.log(`relay refused a message: HTTP ${res.status}`);
          return false;
        }
        this.log(`relay publish: HTTP ${res.status}, retrying in ${Math.round(wait / 1000)} s`);
      } catch (err) {
        if (this.signal.aborted) return false;
        wait = this.#backoff(attempt);
        this.log(`relay publish failed (${err.cause?.code ?? err.message}), retrying in ${Math.round(wait / 1000)} s`);
      } finally {
        clearTimeout(timeout);
        this.signal.removeEventListener('abort', abort);
      }
      await this.#sleep(wait);
    }
    return false;
  }

  // Streams the topic forever, resuming after `since()` on every reconnect.
  async subscribe(topic, since, onMessage) {
    let attempt = 0;
    while (!this.signal.aborted) {
      const resume = since();
      const url = `${this.base}/${topic}/json${resume ? `?since=${encodeURIComponent(resume)}` : ''}`;
      const conn = new AbortController();
      let reader = null;
      // Cancelling our own reader is what ends the read: with undici 6.29 an aborted fetch signal sometimes left a
      // streaming body waiting forever.
      const abort = () => {
        conn.abort();
        reader?.cancel().catch(() => {});
      };
      this.signal.addEventListener('abort', abort, { once: true });
      // A half-open connection never errors; ntfy keepalives every 45 s, so silence means it is gone.
      const idle = setTimeout(abort, this.idleMs);
      let wait;
      try {
        const res = await fetch(url, { signal: conn.signal, redirect: 'error' });
        if (!res.ok) {
          await res.body?.cancel();
          wait = Math.max(res.status === 429 ? retryAfterMs(res) : 0, this.#backoff(attempt++));
          this.log(`relay subscribe: HTTP ${res.status}, retrying in ${Math.round(wait / 1000)} s`);
        } else {
          const decoder = new TextDecoder();
          let buffered = '';
          reader = res.body.getReader();
          for (let read = await reader.read(); !read.done; read = await reader.read()) {
            idle.refresh();
            buffered += decoder.decode(read.value, { stream: true });
            const lines = buffered.split('\n');
            buffered = lines.pop();
            if (buffered.length > MAX_RELAY_LINE) throw new Error('relay sent an over-long line');
            for (const line of lines) {
              let event;
              try { event = JSON.parse(line); } catch { continue; }
              if (event.event === 'open') attempt = 0;
              if (event.event === 'message' && typeof event.id === 'string') onMessage(event);
            }
          }
          wait = this.#backoff(attempt++);
        }
      } catch (err) {
        if (this.signal.aborted) return;
        wait = this.#backoff(attempt++);
        this.log(`relay subscribe dropped (${err.cause?.code ?? err.message}), reconnecting in ${Math.round(wait / 1000)} s`);
      } finally {
        clearTimeout(idle);
        this.signal.removeEventListener('abort', abort);
      }
      await this.#sleep(wait);
    }
  }
}

// ---------------------------------------------------------------- outbox

// Sends down messages one at a time, in order. Only the newest step waits, outside the queue, and it goes out once
// the queue is empty and the last step was sent at least stepEveryMs ago, so a slow relay cannot make steps pile up.
class Outbox {
  #queue = [];
  #draining = false;
  #step = null;
  #stepTimer = null;
  #lastStepAt = -Infinity;

  constructor(deliver, stepEveryMs, log) {
    Object.assign(this, { deliver, stepEveryMs, log });
  }

  get idle() {
    return !this.#draining && !this.#queue.length;
  }

  push(msg) {
    this.#queue.push(msg);
    this.#pump();
  }

  step(msg) {
    this.#step = msg;
    this.#pump();
  }

  dropStep() {
    clearTimeout(this.#stepTimer);
    this.#stepTimer = null;
    this.#step = null;
  }

  #next() {
    if (this.#queue.length) return this.#queue.shift();
    if (!this.#step) return null;
    const wait = this.#lastStepAt + this.stepEveryMs - Date.now();
    if (wait > 0) {
      this.#stepTimer ??= setTimeout(() => {
        this.#stepTimer = null;
        this.#pump();
      }, wait);
      return null;
    }
    const step = this.#step;
    this.#step = null;
    this.#lastStepAt = Date.now();
    return step;
  }

  async #pump() {
    if (this.#draining) return;
    this.#draining = true;
    for (let msg = this.#next(); msg; msg = this.#next()) {
      await this.deliver(msg).catch(err => this.log(`send failed: ${err.message}`));
    }
    this.#draining = false;
  }
}

// ---------------------------------------------------------------- jobs

// One run of the agent command. `onFinish` gets the closing messages once: on the result event, or when the
// process is gone without one. `onClose` follows when the process and its output pipes are gone.
class Job {
  result = null;
  stopReason = null;
  stderrTail = '';
  spawnError = null;
  #finished = false;
  #closed = false;
  #reaping = false;

  constructor(j, prompt, config, { killGraceMs, onStep, onFinish, onClose, extraArgs = [], extraEnv = {} }) {
    Object.assign(this, { j, config, killGraceMs, onFinish, onClose, startedAt: Date.now() });
    const argv = config.command.map(arg => (arg === '{prompt}' ? prompt : arg));
    // Options go before the `--` that precedes the prompt, or the prompt itself when there is none.
    const at = config.command.includes('--') ? config.command.indexOf('--') : config.command.indexOf('{prompt}');
    argv.splice(Math.max(at, 1), 0, ...extraArgs);
    this.child = spawn(argv[0], argv.slice(1), {
      cwd: config.cwd, env: { ...process.env, ...config.env, ...extraEnv }, detached: true, stdio: ['ignore', 'pipe', 'pipe'],
    });
    this.timer = setTimeout(() => this.stop('timeout'), config.maxJobSecs * 1000);

    createInterface({ input: this.child.stdout, crlfDelay: Infinity }).on('line', line => {
      let event;
      try { event = JSON.parse(line); } catch { return; }
      if (event?.type === 'result' && !this.stopReason) {
        this.result = event;
        this.#finish({});
        // claude exits right after its result; one that lingers is holding the job slot for nothing.
        setTimeout(() => this.#reap(), this.killGraceMs).unref();
      }
      const step = stepText(event);
      if (step && !this.#finished) onStep(safeLine(step, LIMITS.stepChars));
    });
    createInterface({ input: this.child.stderr, crlfDelay: Infinity }).on('line', line => {
      if (line.trim()) this.stderrTail = line.trim();
    });

    this.child.on('error', err => {
      this.spawnError = err.code ?? err.message;
      if (!this.child.pid) this.#close(null, null);
    });
    // Leftovers in the group die with the job; they also hold our pipes open and would delay 'close'.
    this.child.on('exit', () => this.#reap());
    this.child.on('close', (code, signal) => this.#close(code, signal));
  }

  get started() {
    return Boolean(this.child.pid);
  }

  stop(reason) {
    this.stopReason ??= reason;
    this.#reap();
  }

  #finish({ code = null, signal = null }) {
    if (this.#finished) return;
    this.#finished = true;
    clearTimeout(this.timer);
    this.onFinish(finalMessages({
      j: this.j, secs: Math.round((Date.now() - this.startedAt) / 1000), result: this.result, stopReason: this.stopReason,
      stderrTail: this.stderrTail, spawnError: this.spawnError, code, signal, maxJobSecs: this.config.maxJobSecs,
    }));
  }

  #close(code, signal) {
    if (this.#closed) return;
    this.#closed = true;
    this.#finish({ code, signal });
    this.onClose();
  }

  #signalGroup(signal) {
    try { process.kill(-this.child.pid, signal); } catch { /* group already gone */ }
  }

  // SIGTERM to the group, SIGKILL after the grace period, and then stop waiting for pipes that a process which left
  // the group (setsid, a daemonizing tool) may still hold.
  #reap() {
    if (this.#reaping || !this.child.pid) return;
    this.#reaping = true;
    this.#signalGroup('SIGTERM');
    setTimeout(() => this.#signalGroup('SIGKILL'), this.killGraceMs).unref();
    setTimeout(() => {
      this.child.stdout.destroy();
      this.child.stderr.destroy();
    }, this.killGraceMs + 250).unref();
  }
}

// ---------------------------------------------------------------- approvals

// The claude CLI asks a local MCP tool before it uses anything its settings don't already allow. That tool is this script
// in `mcp` mode, which relays each request over a Unix socket to the connector, which asks the Cardputer. Anything that
// goes wrong, or goes unanswered, is a denial.
export class Approvals {
  #server;
  #dir;
  #pending = new Map();
  #closed = false;

  constructor({ j, send, timeoutMs, log }) {
    Object.assign(this, { j, send, timeoutMs, log });
    this.#dir = mkdtempSync(join(tmpdir(), 'kloudputer-'));
    this.socketPath = process.platform === 'win32' ? `\\\\.\\pipe\\kloudputer-${randomBytes(8).toString('hex')}` : join(this.#dir, 'approve.sock');
    this.#server = createServer(socket => this.#accept(socket));
    this.#server.on('error', err => log(`approval socket: ${err.message}`));
    this.#server.listen(this.socketPath);
  }

  get pending() {
    return this.#pending.size;
  }

  claudeArgs() {
    const config = { mcpServers: { kloudputer: { command: process.execPath, args: [SCRIPT, 'mcp', '--socket', this.socketPath] } } };
    return ['--mcp-config', JSON.stringify(config), '--permission-prompt-tool', 'mcp__kloudputer__approve'];
  }

  // True when `req` was waiting; an answer for anything else is ignored.
  answer(req, allow) {
    if (!this.#pending.has(req)) return false;
    this.#settle(req, allow === true, allow === true ? undefined : 'The owner denied this from the Cardputer.');
    return true;
  }

  close() {
    if (this.#closed) return;
    this.#closed = true;
    for (const req of [...this.#pending.keys()]) this.#settle(req, false, 'The job ended before this was answered.');
    this.#server.close();
    rmSync(this.#dir, { recursive: true, force: true });
  }

  #accept(socket) {
    let buffered = '';
    socket.setEncoding('utf8');
    socket.on('error', () => {});
    socket.on('close', () => {
      for (const [req, entry] of this.#pending) {
        if (entry.socket === socket) {
          clearTimeout(entry.timer);
          this.#pending.delete(req);
        }
      }
    });
    socket.on('data', function onData(chunk) {
      buffered += chunk;
      if (buffered.length > MAX_RELAY_LINE) return socket.destroy();
      if (!buffered.includes('\n')) return;
      socket.off('data', onData);
      let request = null;
      try { request = JSON.parse(buffered.split('\n', 1)[0]); } catch { /* handled below */ }
      socket.emit('request', request);
    });
    socket.on('request', request => this.#request(socket, request));
  }

  #request(socket, request) {
    const valid = request && typeof request.tool_name === 'string' && request.input && typeof request.input === 'object' && !Array.isArray(request.input);
    if (!valid || this.#closed) return socket.end(JSON.stringify({ allow: false, message: 'The connector could not take this request, so it was denied.' }) + '\n');
    let req;
    do req = randomBytes(4).toString('hex'); while (this.#pending.has(req));
    const timer = setTimeout(() => this.#settle(req, false, 'No answer from the Cardputer in time, so it was denied.'), this.timeoutMs);
    timer.unref();
    this.#pending.set(req, { socket, timer });
    const { tool, detail } = describeApproval(request.tool_name, request.input);
    this.log(`job ${this.j} asks to use ${tool} (request ${req})`);
    this.send({ k: 'approval', j: this.j, req, tool, detail });
  }

  #settle(req, allow, message) {
    const entry = this.#pending.get(req);
    if (!entry) return;
    clearTimeout(entry.timer);
    this.#pending.delete(req);
    this.log(`request ${req} ${allow ? 'allowed' : 'denied'}${message && !allow ? `: ${message}` : ''}`);
    entry.socket.end(JSON.stringify({ allow, message }) + '\n');
  }
}

// ---------------------------------------------------------------- the MCP side (runs as `mcp --socket <path>`)

const APPROVE_TOOL = {
  name: 'approve',
  description: 'Asks the owner on their Cardputer whether the agent may use a tool.',
  inputSchema: {
    type: 'object',
    properties: { tool_name: { type: 'string' }, input: { type: 'object' }, tool_use_id: { type: 'string' } },
    required: ['tool_name', 'input'],
  },
};

function askConnector(socketPath, request, retryMs) {
  return new Promise(resolve => {
    const deadline = Date.now() + retryMs;
    const attempt = () => {
      const socket = connect(socketPath);
      let buffered = '';
      let done = false;
      const finish = decision => {
        if (done) return;
        done = true;
        resolve(decision);
      };
      socket.setEncoding('utf8');
      socket.on('connect', () => socket.write(JSON.stringify(request) + '\n'));
      socket.on('data', chunk => { buffered += chunk; });
      socket.on('error', err => {
        if (!done && ['ENOENT', 'ECONNREFUSED'].includes(err.code) && Date.now() < deadline) {
          done = true;
          setTimeout(attempt, 50);
          return;
        }
        finish({ allow: false, message: `The connector could not be reached (${err.code ?? err.message}), so this was denied.` });
      });
      socket.on('close', () => {
        let reply = null;
        try { reply = JSON.parse(buffered.split('\n', 1)[0]); } catch { /* denied below */ }
        finish(reply && typeof reply.allow === 'boolean'
          ? { allow: reply.allow, message: typeof reply.message === 'string' ? reply.message : undefined }
          : { allow: false, message: 'No answer came back from the connector, so this was denied.' });
      });
    };
    attempt();
  });
}

export async function runMcp(socketPath, { input = process.stdin, output = process.stdout, retryMs = 2500 } = {}) {
  const send = message => output.write(JSON.stringify({ jsonrpc: '2.0', ...message }) + '\n');
  const lines = createInterface({ input, crlfDelay: Infinity });
  lines.on('line', async line => {
    let m = null;
    try { m = JSON.parse(line); } catch { return; }
    if (!m || typeof m !== 'object') return;
    if (m.method === 'initialize') {
      send({ id: m.id, result: { protocolVersion: m.params?.protocolVersion ?? '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'kloudputer', version: VERSION } } });
    } else if (m.method === 'tools/list') {
      send({ id: m.id, result: { tools: [APPROVE_TOOL] } });
    } else if (m.method === 'tools/call') {
      const args = m.params?.arguments;
      const decision = m.params?.name === 'approve' && args
        ? await askConnector(socketPath, { tool_name: args.tool_name, input: args.input }, retryMs)
        : { allow: false, message: 'Unknown request, so it was denied.' };
      const text = decision.allow
        ? { behavior: 'allow', updatedInput: args.input }
        : { behavior: 'deny', message: decision.message ?? 'Denied.' };
      send({ id: m.id, result: { content: [{ type: 'text', text: JSON.stringify(text) }] } });
    } else if (m.id !== undefined) {
      send({ id: m.id, error: { code: -32601, message: 'method not found' } });
    }
  });
  await new Promise(resolve => lines.on('close', resolve));
}

// ---------------------------------------------------------------- state

function loadState(file, log) {
  try {
    const raw = JSON.parse(readFileSync(file, 'utf8'));
    return { since: typeof raw.since === 'string' ? raw.since : null, n: Number.isSafeInteger(raw.n) ? raw.n : 0, seen: new Map(raw.seen) };
  } catch (err) {
    if (err.code !== 'ENOENT') log(`state file unreadable (${err.message}); starting fresh`);
    return { since: null, n: 0, seen: new Map() };
  }
}

function saveState(file, { since, n, seen }) {
  writeFileSync(`${file}.tmp`, JSON.stringify({ since, n, seen: [...seen] }));
  renameSync(`${file}.tmp`, file);
}

// ---------------------------------------------------------------- connector

export class Connector {
  drops = { decrypt: 0, malformed: 0, stale: 0, replay: 0 };
  job = null;
  failure = null;
  #controller = new AbortController();
  #approvals = null;
  #lastDropLog = -Infinity;
  #stopping = false;

  constructor(config, { stepEveryMs = 8000, backoffMs = 1000, maxBackoffMs = 60_000, idleMs = 90_000, killGraceMs = 5000,
    dropLogEveryMs = 60_000, approvalMs = APPROVAL_MS, log = msg => console.error(`kloudputer: ${msg}`) } = {}) {
    this.approvalMs = approvalMs;
    this.config = config;
    this.keys = deriveKeys(config.secret);
    this.log = log;
    this.killGraceMs = killGraceMs;
    this.dropLogEveryMs = dropLogEveryMs;
    this.relay = new Relay(config.relay, { signal: this.#controller.signal, log, backoffMs, maxBackoffMs, idleMs });
    this.outbox = new Outbox(msg => this.#deliver(msg), stepEveryMs, log);
  }

  // Resolves once stopped; rejects when the state file cannot be written.
  async start() {
    this.state = loadState(this.config.stateFile, this.log);
    // Without a saved id, a reconnect would otherwise skip whatever arrived while it was down.
    this.state.since ??= String(Math.floor(nowSecs()));
    saveState(this.config.stateFile, this.state);
    this.log(`connector ${VERSION} "${this.config.name}" listening on ${new URL(this.config.relay).host}`);
    await this.relay.subscribe(this.keys.topicUp, () => this.state.since, event => this.#receive(event));
    if (this.failure) throw this.failure;
  }

  // Ends a running job and gives the Cardputer a few seconds to hear about it before going offline.
  async stop() {
    this.#stopping = true;
    this.job?.stop('shutdown');
    const deadline = Date.now() + this.killGraceMs + 3000;
    while ((this.job || !this.outbox.idle) && Date.now() < deadline && !this.failure) await delay(50);
    this.#controller.abort();
  }

  // Replay protection and message numbering both live in the state file, so running without it is not safe.
  #persist() {
    try {
      saveState(this.config.stateFile, this.state);
      return true;
    } catch (err) {
      this.failure ??= err;
      this.log(`cannot write ${this.config.stateFile} (${err.code ?? err.message}); stopping`);
      this.#stopping = true;
      this.job?.stop('shutdown');
      this.#controller.abort();
      return false;
    }
  }

  #receive(event) {
    if (this.#stopping) return;
    this.state.since = event.id;
    const { keyUp: key, topicUp: topic } = this.keys;
    const checked = checkInbound(event.message ?? '', { key, topic, seen: this.state.seen });
    if (checked.drop) {
      this.#drop(checked.drop);
    } else {
      this.state.seen.set(checked.id, checked.msg.ts);
      const oldest = nowSecs() - TS_PAST_SECS;
      for (const [id, ts] of this.state.seen) if (ts < oldest) this.state.seen.delete(id);
    }
    if (this.#persist() && checked.msg) this.#dispatch(checked.msg);
  }

  #drop(reason) {
    this.drops[reason]++;
    if (Date.now() - this.#lastDropLog < this.dropLogEveryMs) return;
    this.#lastDropLog = Date.now();
    const total = Object.values(this.drops).reduce((a, b) => a + b, 0);
    this.log(`dropped ${total} messages so far (${Object.entries(this.drops).map(([r, c]) => `${r} ${c}`).join(', ')})`);
  }

  #dispatch(msg) {
    switch (msg.k) {
      case 'ping':
        return this.outbox.push({ k: 'hello', j: '', name: this.config.name, busy: Boolean(this.job), version: VERSION });
      case 'ask':
        return this.#ask(msg);
      case 'cancel':
        if (this.job?.j === msg.j) this.job.stop('cancel');
        return;
      case 'approve':
        if (typeof msg.req !== 'string' || !REQ_ID.test(msg.req) || typeof msg.allow !== 'boolean') return this.#drop('malformed');
        if (this.job?.j === msg.j) this.#approvals?.answer(msg.req, msg.allow);
        return;
      default:
        return this.#drop('malformed');
    }
  }

  #ask({ j, text }) {
    if (!JOB_ID.test(j) || typeof text !== 'string') return this.#drop('malformed');
    const refuse = this.job ? 'busy'
      : !text.trim() ? 'empty task'
      : text.length > LIMITS.askChars ? `task too long (max ${LIMITS.askChars} characters)` : null;
    if (refuse) return this.outbox.push({ k: 'error', j, text: refuse });

    const onFinish = messages => {
      this.outbox.dropStep();
      for (const msg of messages) this.outbox.push(msg);
      const last = messages.at(-1);
      this.log(`job ${j} ${last.k === 'done' ? `done ok=${last.ok} in ${last.secs} s` : `failed: ${last.text}`}`);
    };
    const approvals = this.config.approvals
      ? new Approvals({ j, send: msg => this.outbox.push(msg), timeoutMs: this.approvalMs, log: this.log })
      : null;
    let job;
    try {
      job = new Job(j, text, this.config, {
        killGraceMs: this.killGraceMs,
        onFinish: messages => {
          approvals?.close();
          onFinish(messages);
        },
        onClose: () => {
          approvals?.close();
          this.job = null;
          this.#approvals = null;
        },
        onStep: step => this.outbox.step({ k: 'step', j, text: step }),
        extraArgs: approvals?.claudeArgs() ?? [],
        extraEnv: approvals ? { MCP_TOOL_TIMEOUT: String(this.approvalMs + 60_000) } : {},
      });
    } catch (err) {
      // spawn throws synchronously for argv it cannot pass at all, such as a NUL byte in the task.
      approvals?.close();
      return onFinish(finalMessages({ j, spawnError: err.code ?? err.message }));
    }
    this.job = job;
    this.#approvals = approvals;
    if (job.started) {
      this.log(`job ${j} started (${text.length} characters)`);
      this.outbox.push({ k: 'ack', j });
    }
  }

  async #deliver({ k, j, ...fields }) {
    this.state.n += 1;
    if (!this.#persist()) return;
    const plaintext = JSON.stringify({ v: 1, k, j, n: this.state.n, ts: Math.floor(nowSecs()), ...fields });
    const { keyDown, topicDown } = this.keys;
    await this.relay.publish(topicDown, seal(keyDown, topicDown, plaintext));
  }
}

// ---------------------------------------------------------------- boundary: files and CLI

export function readSecret(file) {
  // Windows reports every file as 0o666, so the mode check only means something elsewhere.
  if (process.platform !== 'win32' && statSync(file).mode & 0o077) throw new Error(`${file} is readable by other users; chmod 600 it`);
  const secret = readFileSync(file);
  if (secret.length !== 32) throw new Error(`${file} is not a 32-byte secret`);
  return secret;
}

export function writeSecret(file, secret, { force = false } = {}) {
  const target = force ? `${file}.tmp` : file;
  if (force) rmSync(target, { force: true });
  let fd;
  try {
    fd = openSync(target, 'wx', 0o600);
  } catch (err) {
    if (err.code === 'EEXIST' && !force) throw new Error(`${file} exists; --force replaces it and unpairs the current Cardputer`);
    throw err;
  }
  writeSync(fd, secret);
  closeSync(fd);
  if (force) renameSync(target, file);
}

const CONFIG_KEYS = new Set(['relay', 'secretFile', 'name', 'cwd', 'command', 'env', 'maxJobSecs', 'stateFile', 'approvals']);

export function loadConfig(file) {
  const raw = JSON.parse(readFileSync(file, 'utf8'));
  const fail = why => { throw new Error(`${file}: ${why}`); };
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) fail('must be a JSON object');
  for (const key of Object.keys(raw)) if (!CONFIG_KEYS.has(key)) fail(`unknown key "${key}"`);
  const text = (key, fallback) => {
    const value = raw[key] ?? fallback;
    if (typeof value !== 'string' || !value) fail(`"${key}" must be a non-empty string`);
    return value;
  };
  const here = dirname(resolve(file));

  const relay = text('relay', 'https://ntfy.sh').replace(/\/+$/, '');
  let url = null;
  try { url = new URL(relay); } catch { /* reported below */ }
  if (!url || !/^https?:$/.test(url.protocol)) fail('"relay" must be an http(s) URL');
  if (url.username || url.password) fail('"relay" must not carry a user name or password');
  const name = text('name');
  if (name.length > 64) fail('"name" must be at most 64 characters');
  const { command } = raw;
  if (!Array.isArray(command) || !command.length || !command.every(arg => typeof arg === 'string')) fail('"command" must be an array of strings');
  if (!command.includes('{prompt}')) fail('"command" must contain a "{prompt}" element');
  if (command[0] === '{prompt}') fail('"command" must not run the prompt as the program');
  const env = raw.env ?? {};
  if (typeof env !== 'object' || Array.isArray(env) || !Object.values(env).every(v => typeof v === 'string')) fail('"env" must map names to strings');
  const maxJobSecs = raw.maxJobSecs ?? 900;
  if (typeof maxJobSecs !== 'number' || !(maxJobSecs > 0 && maxJobSecs <= MAX_JOB_SECS)) fail(`"maxJobSecs" must be between 0 and ${MAX_JOB_SECS}`);

  const approvals = raw.approvals ?? false;
  if (typeof approvals !== 'boolean') fail('"approvals" must be true or false');

  return {
    relay, name, command, env, maxJobSecs, approvals,
    secret: readSecret(resolve(here, text('secretFile'))),
    cwd: resolve(here, text('cwd', process.cwd())),
    stateFile: resolve(here, text('stateFile')),
  };
}

const USAGE = `usage:
  kloudputer-connector.mjs pair --secret <file> [--force]   create the secret and print the pairing code
  kloudputer-connector.mjs code --secret <file>             print the pairing code again
  kloudputer-connector.mjs run --config <file.json>         serve the paired Cardputer`;

export async function main(args) {
  const { positionals, values } = parseArgs({
    args, allowPositionals: true,
    options: { secret: { type: 'string' }, config: { type: 'string' }, socket: { type: 'string' }, force: { type: 'boolean', default: false } },
  });
  const [command] = positionals;
  const need = name => {
    if (!values[name]) throw new Error(`${command} needs --${name}\n${USAGE}`);
    return values[name];
  };
  switch (command) {
    case 'pair':
      writeSecret(need('secret'), randomBytes(32), { force: values.force });
      console.error('Paste this code into the Cardputer setup page. Anyone holding it can task this agent.');
      console.log(pairingCode(readSecret(values.secret)));
      return;
    case 'code':
      console.log(pairingCode(readSecret(need('secret'))));
      return;
    case 'run': {
      const connector = new Connector(loadConfig(need('config')));
      for (const signal of ['SIGTERM', 'SIGINT']) process.once(signal, () => connector.stop());
      await connector.start();
      return;
    }
    case 'mcp':
      await runMcp(need('socket'));
      return;
    default:
      throw new Error(USAGE);
  }
}

if (process.argv[1] && realpathSync(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch(err => {
    console.error(`kloudputer: ${err.message}`);
    process.exit(1);
  });
}
