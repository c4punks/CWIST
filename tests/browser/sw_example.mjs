// Run example/wasm-service-worker in Chromium: the bundled worker must install,
// take control, and serve the page, a JSON POST and the session counter from
// the CWIST component. Run by `make wasm-sw-browser-test`.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { chromium } from 'playwright';

const here = fileURLToPath(new URL('.', import.meta.url));
const siteDir = resolve(here, '../../example/wasm-service-worker/build/site');
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

// Fixed allowlist read up front; a request URL never becomes a filesystem path.
const files = new Map();
for (const name of await readdir(siteDir)) {
  const ext = name.slice(name.lastIndexOf('.'));
  if (TYPES[ext]) files.set('/' + name, [await readFile(resolve(siteDir, name)), TYPES[ext]]);
}
assert.ok(files.has('/sw.js') && files.has('/index.html'), 'run make wasm-sw-example first');
files.set('/', files.get('/index.html'));

const server = createServer((req, res) => {
  const entry = files.get(new URL(req.url, 'http://x').pathname);
  if (!entry) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': entry[1] }).end(entry[0]);
});
await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
const origin = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch();
try {
  const context = await browser.newContext();
  const errors = [];
  context.on('weberror', (e) => errors.push(String(e.error())));
  context.on('serviceworker', (worker) => worker.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  }));
  const page = await context.newPage();

  // First load registers the worker and reloads once it controls the page.
  await page.goto(origin + '/');
  await page.waitForFunction(() => navigator.serviceWorker.controller !== null, null,
                             { timeout: 30000 });
  await page.waitForSelector('text=Session visits', { timeout: 30000 });
  assert.match(await page.content(), /Session visits: <strong>1<\/strong>/, 'first visit');

  const created = await page.evaluate(async () => {
    const r = await fetch('/items', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'coffee beans', qty: 3 }),
    });
    return { status: r.status, body: await r.json() };
  });
  assert.equal(created.status, 201, 'POST /items through the worker');
  assert.equal(created.body.ok, true);

  const items = await page.evaluate(async () => (await fetch('/items')).json());
  assert.ok(JSON.stringify(items).includes('coffee beans'), 'GET /items sees the insert');

  // The worker's cookie jar carries the signed session across navigations.
  await page.reload();
  await page.waitForSelector('text=Session visits');
  assert.match(await page.content(), /Session visits: <strong>2<\/strong>/, 'session cookie');

  assert.deepEqual(errors, [], 'no worker or page errors');
  console.log('wasm-sw-browser-test: PASS (worker installed; page, POST, GET and session served in Chromium)');
} finally {
  await browser.close();
  server.close();
}
