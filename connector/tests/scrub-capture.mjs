// Turns a raw `claude -p --output-format stream-json --verbose` capture into a shareable fixture.
//   node tests/scrub-capture.mjs <raw.jsonl> <capture cwd> > tests/fixtures/<name>.jsonl
import { readFileSync } from 'node:fs';
import { userInfo } from 'node:os';

const [raw, cwd] = process.argv.slice(2);
if (!raw || !cwd) throw new Error('usage: scrub-capture.mjs <raw.jsonl> <capture cwd>');

const ids = new Map();
const fake = (kind, real) => {
  const key = `${kind}:${real}`;
  if (!ids.has(key)) ids.set(key, `${kind}_fixture_${ids.size + 1}`);
  return ids.get(key);
};
const user = userInfo().username;
const scrubString = s => s.split(cwd).join('/work').split(user).join('user');

const RESET = {
  session_id: () => '00000000-0000-4000-8000-000000000000',
  uuid: v => fake('uuid', v),
  hook_id: v => fake('hook', v),
  request_id: v => fake('req', v),
  signature: () => '',
  tool_use_id: v => fake('toolu', v),
  output: () => '', stdout: () => '', stderr: () => '',
  mcp_servers: () => [], slash_commands: () => [], skills: () => [], agents: () => [], plugins: () => [],
  memory_paths: () => ({}), messaging_socket_path: () => null, credential_expires_at: () => null,
  tools: v => v.filter(t => !t.startsWith('mcp__')),
  rate_limit_info: () => ({ status: 'allowed' }),
};

function walk(value, key) {
  if (Object.hasOwn(RESET, key) && value !== null && value !== undefined) return RESET[key](value);
  if (key === 'id' && typeof value === 'string') return fake(value.startsWith('toolu_') ? 'toolu' : 'msg', value);
  if (typeof value === 'string') return scrubString(value);
  if (Array.isArray(value)) return value.map(v => walk(v, ''));
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([k, v]) => [k.startsWith('toolu_') ? fake('toolu', k) : k, walk(v, k)]));
  }
  return value;
}

for (const line of readFileSync(raw, 'utf8').split('\n')) {
  if (line.trim()) console.log(JSON.stringify(walk(JSON.parse(line), '')));
}
