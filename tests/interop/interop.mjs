// Interop: the Node connector (connector/kloudputer-connector.mjs) against the Cardputer's own protocol code
// (src/agent_proto.h via ./proto_tool), through the mock ntfy relay. Nothing leaves the laptop.
import { execFileSync, spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
const C = fileURLToPath(new URL('../../connector', import.meta.url));
const { startMockNtfy } = await import(`${C}/tests/mock-ntfy.mjs`);
const TOOL = process.env.PROTO_TOOL;
if (!TOOL) throw new Error('run tests/interop/run.sh, which builds proto_tool and sets PROTO_TOOL');
const tool = (...a) => execFileSync(TOOL, a, { encoding: 'utf8' }).trim();
const sleep = ms => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); if (!ok) failures++; };

const relay = await startMockNtfy();
const dir = mkdtempSync(join(tmpdir(), 'interop-'));
const secret = join(dir, 'secret');
const code = execFileSync(process.execPath, [`${C}/kloudputer-connector.mjs`, 'pair', '--secret', secret], { encoding: 'utf8' })
  .split('\n').map(s => s.trim()).find(s => s.startsWith('cp1-'));
check(/^cp1-[A-Za-z0-9_-]{43}$/.test(code ?? ''), 'connector pairing code parses as cp1-');
const [up, down] = tool('topics', code).split(' ');
writeFileSync(join(dir, 'cfg.json'), JSON.stringify({
  relay: relay.url, secretFile: secret, name: 'Interop', cwd: dir, maxJobSecs: 60, stateFile: join(dir, 'state.json'),
  command: [process.execPath, `${C}/tests/fake-agent.mjs`, 'replay', `${C}/tests/fixtures/stream-read.jsonl`, '20', '{prompt}'],
}));
const conn = spawn(process.execPath, [`${C}/kloudputer-connector.mjs`, 'run', '--config', join(dir, 'cfg.json')], { stdio: ['ignore', 'pipe', 'pipe'] });
let connLog = ''; conn.stdout.on('data', d => connLog += d); conn.stderr.on('data', d => connLog += d);
await sleep(800);

const post = body => fetch(`${relay.url}/${up}`, { method: 'POST', body, headers: { 'X-Firebase': 'no' } });
const opened = () => relay.messages(down).map(m => { try { return tool('open', code, m.message); } catch { return 'UNOPENABLE'; } });
const now = () => Math.floor(Date.now() / 1000);
const job = randomBytes(4).toString('hex');

await post(tool('ping', code, '', String(now()), String(now())));
await sleep(600);
check(opened().some(l => l.startsWith('hello||') && l.includes('|Interop|')), 'device ping -> connector hello, opened by device code');

const wrong = 'cp1-' + randomBytes(32).toString('base64url');
await post(tool('ask', wrong, 'deadbeef', '1', String(now()), 'forged'));
await post(tool('ask', code, job, '1', String(now() - 3600), 'too old'));
await sleep(600);
check(!opened().some(l => l.includes('|deadbeef|')), 'ask sealed with another pairing code is ignored');
check(!opened().some(l => l.startsWith('ack|' + job)), 'ask with an hour-old ts is ignored');

await post(tool('ask', code, job, '2', String(now()), 'say the first line of fruits.txt'));
for (let i = 0; i < 100 && !opened().some(l => l.startsWith(`done|${job}`)); i++) await sleep(200);
const lines = opened();
const mine = lines.filter(l => l.split('|')[1] === job);
console.log(mine.map(l => '      ' + l).join('\n'));
check(!lines.includes('UNOPENABLE'), 'every down message opens with the device code');
check(mine[0]?.startsWith(`ack|${job}`), 'first job message is ack');
check(mine.some(l => l.startsWith(`step|${job}`)), 'at least one step');
const parts = mine.filter(l => l.startsWith(`part|${job}`));
check(parts.length >= 1 && parts.map(p => p.split('|')[3]).join('').includes('pineapple'), 'report parts carry the agent result ("pineapple")');
check(mine.at(-1)?.startsWith(`done|${job}`) && mine.at(-1).split('|')[7] === '1', 'job ends with done ok');
const ns = mine.map(l => Number(l.split('|')[2]));
check(ns.every((n, i) => i === 0 || n > ns[i - 1]), 'connector counters rise within the job');


// ---- approvals: the connector asks, the device answers with its own approve message ----
{
  const dir2 = mkdtempSync(join(tmpdir(), 'interop2-'));
  const secret2 = join(dir2, 'secret');
  const code2 = execFileSync(process.execPath, [`${C}/kloudputer-connector.mjs`, 'pair', '--secret', secret2], { encoding: 'utf8' })
    .split('\n').map(s => s.trim()).find(s => s.startsWith('cp1-'));
  const [up2, down2] = tool('topics', code2).split(' ');
  writeFileSync(join(dir2, 'cfg.json'), JSON.stringify({
    relay: relay.url, secretFile: secret2, name: 'Interop approvals', cwd: dir2, maxJobSecs: 60, stateFile: join(dir2, 'state.json'), approvals: true,
    command: [process.execPath, `${C}/tests/fake-agent.mjs`, 'perm', 'Bash', '{"command":"kubectl get pods -n cms"}', '{prompt}'],
  }));
  const conn2 = spawn(process.execPath, [`${C}/kloudputer-connector.mjs`, 'run', '--config', join(dir2, 'cfg.json')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let log2 = ''; conn2.stdout.on('data', d => log2 += d); conn2.stderr.on('data', d => log2 += d);
  await sleep(800);
  const post2 = body => fetch(`${relay.url}/${up2}`, { method: 'POST', body, headers: { 'X-Firebase': 'no' } });
  const opened2 = () => relay.messages(down2).map(m => { try { return tool('open', code2, m.message); } catch { return 'UNOPENABLE'; } });
  const job2 = randomBytes(4).toString('hex');
  await post2(tool('ask', code2, job2, '1', String(now()), 'check the pods'));
  for (let i = 0; i < 50 && !opened2().some(l => l.startsWith(`approval|${job2}`)); i++) await sleep(200);
  const ask = opened2().find(l => l.startsWith(`approval|${job2}`));
  console.log('      ' + ask);
  const [, , , , , , , , req, toolName, detail] = (ask ?? '').split('|');
  check(/^[0-9a-f]{8}$/.test(req ?? ''), 'device parses the approval request id');
  check(toolName === 'Bash' && detail === 'kubectl get pods -n cms', 'device reads the tool and what it will run');
  await post2(tool('approve', code2, job2, '2', String(now()), req ?? '00000000', '1'));
  for (let i = 0; i < 50 && !opened2().some(l => l.startsWith(`done|${job2}`)); i++) await sleep(200);
  const report = opened2().filter(l => l.startsWith(`part|${job2}`)).map(l => l.split('|')[3]).join('');
  check(report.includes('"behavior":"allow"') && report.includes('kubectl get pods -n cms'), "the device's approve message made the agent's request allowed, with its input");
  check(!opened2().includes('UNOPENABLE'), 'every approvals-run message opens with the device code');
  conn2.kill('SIGTERM');
  if (failures) console.log(log2.slice(-1500));
}

conn.kill('SIGTERM');
await sleep(300);
await relay.close?.();
console.log(`${failures ? 'FAILED' : 'all interop checks passed'} (${failures} failed)`);
if (failures) console.log(connLog.slice(-1500));
process.exit(failures ? 1 : 0);
