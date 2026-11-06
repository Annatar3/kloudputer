// In-process stand-in for an ntfy server: publish, poll, stream, the 4,096-byte body limit, and injected 429s.
import { randomBytes } from 'node:crypto';
import http from 'node:http';

const TOPIC = /^\/([-_A-Za-z0-9]{1,64})(\/json)?$/;
const MAX_BODY = 4096;

export async function startMockNtfy({ keepaliveMs = 0 } = {}) {
  const topics = new Map();
  const streams = new Set();
  const requests = [];
  const rejections = { oversize: 0, noFirebaseHeader: 0 };
  const inject = { GET: [], POST: [] };
  const arrivals = new Map();

  const messagesOf = topic => topics.get(topic) ?? topics.set(topic, []).get(topic);
  const after = (topic, since) => {
    const list = messagesOf(topic);
    if (since === undefined || since === null || since === 'all') return list;
    if (/^\d+$/.test(since)) return list.filter(m => m.time >= Number(since));
    const i = list.findIndex(m => m.id === since);
    return i === -1 ? list : list.slice(i + 1);
  };
  const store = (topic, body) => {
    const msg = { id: randomBytes(9).toString('base64url'), time: Math.floor(Date.now() / 1000), event: 'message', topic, message: body };
    messagesOf(topic).push(msg);
    arrivals.set(msg.id, Date.now());
    for (const s of streams) if (s.topic === topic) s.res.write(JSON.stringify(msg) + '\n');
    return msg;
  };

  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://mock');
    const match = TOPIC.exec(url.pathname);
    requests.push({ method: req.method, path: url.pathname, query: Object.fromEntries(url.searchParams), headers: req.headers });
    const queue = inject[req.method] ?? [];
    const at = queue.findIndex(i => i.path === undefined || i.path === url.pathname);
    const injected = at === -1 ? null : queue.splice(at, 1)[0];
    if (injected) {
      res.writeHead(429, injected.retryAfter === undefined ? {} : { 'Retry-After': String(injected.retryAfter) });
      return res.end('{"code":42901,"http":429,"error":"limit reached"}');
    }
    if (!match) return res.writeHead(404).end();
    const [, topic, json] = match;

    if (req.method === 'POST' && !json) {
      const chunks = [];
      req.on('data', c => chunks.push(c));
      req.on('end', () => {
        const body = Buffer.concat(chunks);
        if (body.length > MAX_BODY) {
          rejections.oversize++;
          return res.writeHead(413).end();
        }
        if (req.headers['x-firebase'] !== 'no') rejections.noFirebaseHeader++;
        res.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(store(topic, body.toString('utf8'))));
      });
      return;
    }

    if (req.method === 'GET' && json) {
      const since = url.searchParams.get('since') ?? undefined;
      if (url.searchParams.get('poll') === '1') {
        res.writeHead(200, { 'Content-Type': 'application/x-ndjson' });
        return res.end(after(topic, since ?? 'all').map(m => JSON.stringify(m) + '\n').join(''));
      }
      res.writeHead(200, { 'Content-Type': 'application/x-ndjson' });
      res.write(JSON.stringify({ id: randomBytes(9).toString('base64url'), time: Math.floor(Date.now() / 1000), event: 'open', topic }) + '\n');
      if (since !== undefined) for (const m of after(topic, since)) res.write(JSON.stringify(m) + '\n');
      const stream = { topic, res };
      streams.add(stream);
      const keepalive = keepaliveMs && setInterval(() => res.write(JSON.stringify({ event: 'keepalive', topic }) + '\n'), keepaliveMs);
      res.on('close', () => {
        clearInterval(keepalive);
        streams.delete(stream);
      });
      return;
    }
    res.writeHead(405).end();
  });

  await new Promise(r => server.listen(0, '127.0.0.1', r));
  return {
    url: `http://127.0.0.1:${server.address().port}`,
    requests,
    rejections,
    messages: topic => [...messagesOf(topic)],
    store,
    arrivedAt: id => arrivals.get(id),
    subscribers: topic => [...streams].filter(s => s.topic === topic).length,
    inject429(method, count, { retryAfter, path } = {}) {
      for (let i = 0; i < count; i++) inject[method].push({ retryAfter, path });
    },
    dropStreams() {
      for (const s of streams) {
        streams.delete(s);
        s.res.destroy();
      }
    },
    close() {
      for (const s of streams) s.res.destroy();
      server.closeAllConnections?.();
      return new Promise(r => server.close(r));
    },
  };
}
