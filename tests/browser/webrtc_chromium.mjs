// Browser interop check for the WebRTC DataChannel stack.
//
// Drives headless Chromium over the DevTools protocol (no Playwright needed;
// Node >= 22 for the global WebSocket) against the running example server
// (example/webrtc, POST /offer signaling, DataChannel echo) and checks:
//   - the DataChannel opens (ICE-lite + DTLS + SCTP + DCEP with a real browser),
//   - a text message comes back as a string with the same content,
//   - a 200 000-byte binary message comes back byte for byte,
//   - an empty string comes back empty,
// then reports the average round trip over 200 small messages.
//
// Usage: node tests/browser/webrtc_chromium.mjs [url]
//   CHROMIUM=/path/to/chromium overrides the browser binary.
// `make test_webrtc_browser` builds and starts the example, then runs this.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const url = process.argv[2] || 'http://localhost:8080/';
const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), 'cwist-webrtc-cdp-'));
const chrome = spawn(process.env.CHROMIUM || 'chromium', [
  '--headless=new', `--remote-debugging-port=${port}`, '--no-first-run',
  '--no-default-browser-check', `--user-data-dir=${profile}`, 'about:blank',
], { stdio: 'ignore' });

async function pageTarget() {
  for (let i = 0; i < 50; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page;
    } catch {
      /* not up yet */
    }
    await sleep(200);
  }
  throw new Error('chromium did not start');
}

function cdpClient(ws) {
  let nextId = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      pending.get(msg.id)(msg);
      pending.delete(msg.id);
    }
  };
  return (method, params = {}) =>
    new Promise((resolve) => {
      const id = ++nextId;
      pending.set(id, resolve);
      ws.send(JSON.stringify({ id, method, params }));
    });
}

// Runs inside the page.
const pageScript = `(async () => {
  const t0 = performance.now();
  const pc = new RTCPeerConnection();
  const dc = pc.createDataChannel('echo');
  dc.binaryType = 'arraybuffer';
  const opened = new Promise((res, rej) => {
    dc.onopen = res;
    setTimeout(() => rej(new Error('open timeout: ice=' + pc.iceConnectionState +
                                   ' conn=' + pc.connectionState)), 15000);
  });
  await pc.setLocalDescription(await pc.createOffer());
  const r = await fetch('/offer', { method: 'POST', body: pc.localDescription.sdp });
  if (!r.ok) throw new Error('offer rejected: HTTP ' + r.status);
  await pc.setRemoteDescription({ type: 'answer', sdp: await r.text() });
  await opened;
  const openMs = performance.now() - t0;

  const inbox = [];
  let wake = null;
  dc.onmessage = (ev) => { inbox.push(ev.data); if (wake) wake(); };
  const next = () => new Promise((res, rej) => {
    if (inbox.length) return res(inbox.shift());
    const timer = setTimeout(() => rej(new Error('echo timeout')), 10000);
    wake = () => { wake = null; clearTimeout(timer); res(inbox.shift()); };
  });

  dc.send('hello cwist');
  const text = await next();

  const big = new Uint8Array(200000);
  for (let i = 0; i < big.length; i++) big[i] = (i * 31) & 255;
  dc.send(big);
  const bigEcho = await next();
  const bigView = bigEcho instanceof ArrayBuffer ? new Uint8Array(bigEcho) : null;
  let bigOk = !!bigView && bigView.length === big.length;
  for (let i = 0; bigOk && i < big.length; i++) if (bigView[i] !== big[i]) bigOk = false;

  dc.send('');
  const empty = await next();

  const rt0 = performance.now();
  for (let i = 0; i < 200; i++) { dc.send('p' + i); await next(); }
  const rttMs = (performance.now() - rt0) / 200;
  pc.close();
  return JSON.stringify({
    open_ms: Math.round(openMs),
    text_type: typeof text, text,
    big_ok: bigOk, big_len: bigView ? bigView.length : -1,
    empty_type: typeof empty, empty_len: typeof empty === 'string' ? empty.length : -1,
    avg_rtt_ms: Number(rttMs.toFixed(3)),
  });
})()`;

let failed = false;
try {
  const page = await pageTarget();
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  const send = cdpClient(ws);
  await send('Page.enable');
  await send('Page.navigate', { url });
  await sleep(1000);
  const res = await send('Runtime.evaluate', {
    expression: pageScript, awaitPromise: true, returnByValue: true,
  });
  ws.close();
  if (res.result.exceptionDetails) {
    const d = res.result.exceptionDetails;
    throw new Error(d.exception?.description || d.text);
  }
  const out = JSON.parse(res.result.result.value);
  console.log('webrtc_chromium:', JSON.stringify(out));
  const checks = [
    ['text echoed as a string', out.text_type === 'string' && out.text === 'hello cwist'],
    ['200000-byte binary echoed intact', out.big_ok],
    ['empty string echoed', out.empty_type === 'string' && out.empty_len === 0],
  ];
  for (const [name, ok] of checks) {
    console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}`);
    if (!ok) failed = true;
  }
} catch (err) {
  console.log('webrtc_chromium: FAIL', err.message);
  failed = true;
} finally {
  chrome.kill('SIGKILL');
  rmSync(profile, { recursive: true, force: true });
}
process.exitCode = failed ? 1 : 0;
