// Prints the fixed test vector in PROTOCOL.md from Node's built-in crypto. Rerun to check it.
import { hkdfSync, createCipheriv } from 'node:crypto';
const S = Buffer.from('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f', 'hex');
const derive = (label, n) => Buffer.from(hkdfSync('sha256', S, Buffer.from('claudeputer-v1'), Buffer.from(label), n));
const topic = label => 'cp-' + derive(label, 16).toString('base64url');
const up = topic('topic-up'), down = topic('topic-down');
const keyUp = derive('key-up', 32), keyDown = derive('key-down', 32);
const nonce = Buffer.from('a0a1a2a3a4a5a6a7a8a9aaab', 'hex');
const plain = '{"v":1,"k":"ask","j":"0badc0de","n":1,"ts":1762444800,"text":"is cms-api up?"}';
const c = createCipheriv('aes-256-gcm', keyUp, nonce);
c.setAAD(Buffer.from(up));
const body = Buffer.concat([nonce, c.update(plain, 'utf8'), c.final(), c.getAuthTag()]).toString('base64url');
console.log(JSON.stringify({ pairingCode: 'cp1-' + S.toString('base64url'), topicUp: up, topicDown: down,
  keyUp: keyUp.toString('hex'), keyDown: keyDown.toString('hex'), nonce: nonce.toString('hex'), plaintext: plain, body }, null, 2));
