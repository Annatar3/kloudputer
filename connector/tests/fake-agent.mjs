// Stands in for `claude -p ... --output-format stream-json --verbose`. The prompt is always the last argument.
//   replay <fixture.jsonl> <gapMs>   print the captured stream line by line
//   echo                             report the prompt exactly as received
//   steps <count> <gapMs> <tailMs>   emit Bash tool_use steps, then report "ok"
//   report <file>                    report the file's contents
//   fail                             write to stderr and exit 3 without a result
//   slow <pidfile>                   start a grandchild, emit a step, then hang
//   stubborn <pidfile>               ignore SIGTERM and hang
//   linger <pidfile>                 report, then exit while a grandchild keeps stdout open
//   escape <pidfile>                 exit without a report while a setsid grandchild keeps stdout open
//   resultHang <pidfile>             report, then keep running
//   badtool                          use tools named like Object.prototype methods, then report
//   mark <file>                      create the file, then report
//   perm <tool> <json> [<tool> <json>...]  ask the --permission-prompt-tool for each call at once, then report the answers
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';

const [mode, ...rest] = process.argv.slice(2);
const prompt = rest.at(-1);
const emit = event => process.stdout.write(JSON.stringify(event) + '\n');
const init = () => emit({ type: 'system', subtype: 'init', cwd: process.cwd(), session_id: 'fake', tools: ['Bash', 'Read'], model: 'fake' });
const result = (text, extra = {}) => emit({
  type: 'result', subtype: 'success', is_error: false, result: text, num_turns: 1, total_cost_usd: 0.0123, ...extra,
});
const bash = command => emit({ type: 'assistant', message: { role: 'assistant', content: [{ type: 'tool_use', id: 'toolu_x', name: 'Bash', input: { command } }] } });
const grandchild = stdio => spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { stdio });
const hang = () => setInterval(() => {}, 1000);

// Speaks MCP to the server named in --mcp-config the way claude does for its permission prompt tool.
async function askPermissions(pairs) {
  const server = JSON.parse(rest[rest.indexOf('--mcp-config') + 1]).mcpServers.kloudputer;
  const toolName = rest[rest.indexOf('--permission-prompt-tool') + 1];
  const mcp = spawn(server.command, server.args, { stdio: ['pipe', 'pipe', 'inherit'] });
  const waiting = new Map();
  let buffered = '';
  mcp.stdout.on('data', chunk => {
    buffered += chunk;
    const lines = buffered.split('\n');
    buffered = lines.pop();
    for (const line of lines.filter(Boolean)) {
      const m = JSON.parse(line);
      waiting.get(m.id)?.(m);
    }
  });
  let id = 0;
  const call = (method, params) => new Promise(resolve => {
    waiting.set(++id, resolve);
    mcp.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
  });
  await call('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'fake-claude', version: '0' } });
  mcp.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  const listed = await call('tools/list');
  const answers = await Promise.all(pairs.map(([tool, input], i) => call('tools/call', { name: 'approve', arguments: { tool_name: tool, input: JSON.parse(input), tool_use_id: `toolu_${i}` } })));
  mcp.kill();
  return { toolName, listed: listed.result.tools.map(t => t.name), decisions: answers.map(a => JSON.parse(a.result.content[0].text)) };
}

switch (mode) {
  case 'argv':
    init();
    result(JSON.stringify(process.argv.slice(3)));
    break;
  case 'perm': {
    const flag = rest.indexOf('--mcp-config');
    const pairs = [];
    for (let i = 0; i < flag; i += 2) pairs.push([rest[i], rest[i + 1]]);
    init();
    result(JSON.stringify(await askPermissions(pairs)));
    break;
  }
  case 'replay':
    for (const line of readFileSync(rest[0], 'utf8').split('\n').filter(Boolean)) {
      process.stdout.write(line + '\n');
      await sleep(Number(rest[1]));
    }
    break;
  case 'echo':
    init();
    result(prompt);
    break;
  case 'steps': {
    const [count, gapMs, tailMs] = rest.slice(0, 3).map(Number);
    init();
    for (let i = 1; i <= count; i++) {
      bash(`step-${i}`);
      await sleep(gapMs);
    }
    await sleep(tailMs);
    result('ok');
    break;
  }
  case 'report':
    init();
    result(readFileSync(rest[0], 'utf8'));
    break;
  case 'fail':
    process.stderr.write('starting\n');
    process.stderr.write('Error: API key sk-ant-api03-AAAAbbbbCCCCdddd1234567890 rejected, token=hunter2secret\n\n');
    process.exit(3);
    break;
  case 'slow': {
    const child = grandchild('ignore');
    writeFileSync(rest[0], JSON.stringify([process.pid, child.pid]));
    init();
    bash('sleep 600');
    hang();
    break;
  }
  case 'stubborn':
    process.on('SIGTERM', () => {});
    writeFileSync(rest[0], JSON.stringify([process.pid]));
    init();
    hang();
    break;
  case 'linger': {
    const child = grandchild(['ignore', 'inherit', 'inherit']);
    writeFileSync(rest[0], JSON.stringify([process.pid, child.pid]));
    init();
    result('lingered');
    child.unref();
    break;
  }
  case 'escape': {
    const child = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { stdio: ['ignore', 'inherit', 'inherit'], detached: true });
    writeFileSync(rest[0], JSON.stringify([process.pid, child.pid]));
    init();
    child.unref();
    break;
  }
  case 'resultHang':
    writeFileSync(rest[0], JSON.stringify([process.pid]));
    init();
    result('still here');
    hang();
    break;
  case 'mark':
    writeFileSync(rest[0], 'ran');
    init();
    result('marked');
    break;
  case 'badtool':
    init();
    for (const name of ['__defineGetter__', 'constructor', 'toString']) {
      emit({ type: 'assistant', message: { content: [{ type: 'tool_use', id: 'toolu_y', name, input: {} }] } });
    }
    result('survived');
    break;
  default:
    throw new Error(`unknown mode ${mode}`);
}
