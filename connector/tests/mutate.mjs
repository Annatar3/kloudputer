// Mutation check: breaks one behavior at a time in a copy of the connector and runs the suite against it.
//   node tests/mutate.mjs            all mutants, 3 at a time
//   node tests/mutate.mjs <name>...  only these
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const SOURCE = readFileSync(join(ROOT, 'kloudputer-connector.mjs'), 'utf8');

const MUTANTS = [
  ['shell', 'prompt goes through a shell', 'detached: true, stdio', 'detached: true, shell: true, stdio'],
  ['ts-window', 'no 15 min staleness check', 'now - msg.ts > TS_PAST_SECS ||', ''],
  ['dedupe', 'no (j, n) replay check', "if (seen.has(id)) return { drop: 'replay' };", ''],
  ['seen-not-persisted', 'replay memory lost on restart', 'seen: new Map(raw.seen)', 'seen: new Map()'],
  ['n-not-persisted', 'down counter restarts at 0', 'n: Number.isSafeInteger(raw.n) ? raw.n : 0', 'n: 0'],
  ['no-throttle', 'steps sent immediately', 'const wait = this.#lastStepAt + this.stepEveryMs - Date.now();', 'const wait = 0;'],
  ['first-step-wins', 'throttle keeps the oldest step', 'this.#step = msg;', 'this.#step ??= msg;'],
  ['stale-step-after-done', 'pending step not dropped at job end', 'this.outbox.dropStep();', ''],
  ['kill-pid-not-group', 'cancel kills only the agent, not its group', 'process.kill(-this.child.pid, signal)', 'process.kill(this.child.pid, signal)'],
  ['no-sigkill', 'no SIGKILL escalation', "this.#signalGroup('SIGKILL')", "this.#signalGroup('SIGTERM')"],
  ['no-busy', 'second ask runs alongside the first', "const refuse = this.job ? 'busy'", "const refuse = false ? 'busy'"],
  ['ignore-retry-after', '429 Retry-After ignored', 'Math.max(retryAfterMs(res), this.#backoff(attempt))', 'this.#backoff(attempt)'],
  ['no-since', 'reconnect without since=', '${resume ? `?since=${encodeURIComponent(resume)}` : \'\'}', ''],
  ['no-idle-refresh', 'idle watchdog ignores keepalives', 'idle.refresh();', ''],
  ['chunk-bytes', 'parts sized by characters only', '|| PART_OVERHEAD + bytes + chBytes > LIMITS.plaintextBytes', ''],
  ['chunk-off-by-one', 'parts one character short', 'chunk.length + ch.length > LIMITS.partChars', 'chunk.length + ch.length >= LIMITS.partChars'],
  ['unscrubbed-stderr', 'stderr reported raw', 'text: safeLine(why, LIMITS.errorChars)', 'text: clip(why, LIMITS.errorChars)'],
  ['secret-0666', 'secret created world-readable', "openSync(target, 'wx', 0o600)", "openSync(target, 'wx', 0o666)"],
  ['secret-overwrite', 'pair overwrites without --force', "openSync(target, 'wx', 0o600)", "openSync(target, 'w', 0o600)"],
  ['no-reap-on-exit', 'leftover process holds the job open', "this.child.on('exit', () => this.#reap());", ''],
  ['spawn-throw', 'an unspawnable task wedges the connector', 'return onFinish(finalMessages({ j, spawnError: err.code ?? err.message }));', 'throw err;'],
  ['subscribe-follows-redirect', 'stream GET follows a relay redirect', "{ signal: conn.signal, redirect: 'error' }", "{ signal: conn.signal, redirect: 'follow' }"],
  ['publish-follows-redirect', 'publish POST follows a relay redirect', "redirect: 'error', signal: request.signal", "redirect: 'follow', signal: request.signal"],
  ['no-line-cap', 'relay can grow the line buffer forever', "if (buffered.length > MAX_RELAY_LINE) throw new Error('relay sent an over-long line');", ''],
  ['retry-after-uncapped', 'a huge Retry-After overflows the timer', 'return Math.min(Math.max(0, ms), MAX_RETRY_AFTER_MS);', 'return Math.max(0, ms);'],
  ['prototype-lookup', 'tool names reach Object.prototype', 'Object.hasOwn(STEP_DETAIL, tool.name) ? firstLine(STEP_DETAIL[tool.name](tool.input ?? {})) : \'\'', 'firstLine(STEP_DETAIL[tool.name]?.(tool.input ?? {}))'],
  ['report-on-close', 'report waits for the agent to exit', 'this.#finish({});', ''],
  ['no-linger-reap', 'an agent that lingers after its result keeps the slot', 'setTimeout(() => this.#reap(), this.killGraceMs).unref();', ''],
  ['no-pipe-destroy', 'an escaped process holding the pipes keeps the job open', 'this.child.stdout.destroy();', ''],
  ['abrupt-stop', 'stop goes offline before reporting the job', 'while ((this.job || !this.outbox.idle) && Date.now() < deadline && !this.failure) await delay(50);', ''],
  ['persist-ignored', 'acts on a message whose state was not saved', 'if (this.#persist() && checked.msg)', 'if ((this.#persist(), checked.msg))'],
  ['no-start-since', 'first reconnect has no since', 'this.state.since ??= String(Math.floor(nowSecs()));', ''],
  ['approval-timeout-allows', 'a request nobody answers is allowed', "this.#settle(req, false, 'No answer from the Cardputer in time", "this.#settle(req, true, 'No answer from the Cardputer in time"],
  ['approval-always-allow', 'every answer allows', 'this.#settle(req, allow === true, allow === true ?', 'this.#settle(req, true, allow === true ?'],
  ['approval-args-last', 'prompt tool options go after the prompt', 'argv.splice(Math.max(at, 1), 0, ...extraArgs);', 'argv.push(...extraArgs);'],
  ['approval-folder-kept', 'the socket folder outlives the job', '    rmSync(this.#dir, { recursive: true, force: true });\n', ''],
  ['approval-open-on-close', 'requests still waiting at job end are left hanging', "    for (const req of [...this.#pending.keys()]) this.#settle(req, false, 'The job ended before this was answered.');\n", ''],
  ['approval-any-job', 'an answer for another job is accepted', 'if (this.job?.j === msg.j) this.#approvals?.answer(msg.req, msg.allow);', 'this.#approvals?.answer(msg.req, msg.allow);'],
  ['approval-fails-open', 'an unreachable connector allows the call', "finish({ allow: false, message: `The connector could not be reached", "finish({ allow: true, message: `The connector could not be reached"],
  ['approval-no-redaction', 'a secret in a command is shown on the screen', 'const shown = scrub(clip(flat, max * 4));', 'const shown = clip(flat, max * 4);'],
  ['approval-hidden-tail', 'a cut command does not say it was cut', 'return clip(shown, kept) + markerFor(kept);', 'return clip(shown, max);'],
];
const SUITE_DEADLINE_MS = 180_000;

function runSuite(connector) {
  return new Promise(resolve => {
    const proc = spawn(process.execPath, ['--test', '--test-reporter=tap', '--test-timeout=60000', '--test-force-exit'], {
      cwd: ROOT, env: { ...process.env, CONNECTOR_UNDER_TEST: connector }, stdio: ['ignore', 'pipe', 'ignore'], detached: true,
    });
    let out = '';
    let hung = false;
    const deadline = setTimeout(() => {
      hung = true;
      try { process.kill(-proc.pid, 'SIGKILL'); } catch { /* already gone */ }
    }, SUITE_DEADLINE_MS);
    proc.stdout.on('data', d => { out += d; });
    proc.on('close', status => {
      clearTimeout(deadline);
      const failed = [...out.matchAll(/^not ok \d+ - (.+)$/gm)].map(m => m[1]);
      resolve({ status: hung ? 'hung' : status, failed: hung ? [...failed, `(suite hung past ${SUITE_DEADLINE_MS / 1000} s)`] : failed });
    });
  });
}

const wanted = process.argv.slice(2);
const chosen = wanted.length ? MUTANTS.filter(([name]) => wanted.includes(name)) : MUTANTS;
const dir = mkdtempSync(join(tmpdir(), 'kloudputer-mutants-'));

const baseline = await runSuite(join(ROOT, 'kloudputer-connector.mjs'));
if (baseline.status !== 0) throw new Error(`baseline fails: ${baseline.failed.join('; ')}`);

const results = [];
const queue = [...chosen];
await Promise.all([1, 2, 3].map(async () => {
  for (let m = queue.shift(); m; m = queue.shift()) {
    const [name, behavior, from, to] = m;
    if (SOURCE.split(from).length !== 2) throw new Error(`${name}: pattern must occur exactly once`);
    const file = join(dir, `${name}.mjs`);
    writeFileSync(file, SOURCE.replace(from, () => to));
    const { status, failed } = await runSuite(file);
    results.push({ name, behavior, killed: status !== 0, failed });
  }
}));

for (const r of chosen.map(([name]) => results.find(x => x.name === name))) {
  console.log(`${r.killed ? 'KILLED  ' : 'SURVIVED'} ${r.name.padEnd(22)} ${r.behavior}`);
  for (const t of r.failed) console.log(`           - ${t}`);
}
const survived = results.filter(r => !r.killed).length;
console.log(`\n${results.length - survived}/${results.length} mutants killed`);
process.exitCode = survived ? 1 : 0;
