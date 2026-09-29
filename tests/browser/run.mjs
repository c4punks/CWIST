// Execute the generated WASI 0.2 component, not a mock, in Chromium.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { chromium } from 'playwright';

const here = fileURLToPath(new URL('.', import.meta.url));
const artifactDir = resolve(here, '../../.jco-guest/wasm32-wasip2');
const modes = new Set(['none', 'missing-wasm', 'exception']);
const args = process.argv.slice(2);
const selfTest = args.length === 1 && args[0] === '--self-test';
const fault = args.length === 0 || selfTest ? 'none' : args[0].replace(/^--fault=/, '');
if ((!selfTest && args.length > 1) || !modes.has(fault) ||
    (args.length && !selfTest && args[0] !== `--fault=${fault}`)) {
  throw new Error('usage: node run.mjs [--self-test|--fault=missing-wasm|--fault=exception]');
}

async function run() {
  // Preload a fixed allowlist. Never translate a request URL into a filesystem path.
  const files = new Map();
  files.set('/browser_bundle.js', [await readFile(resolve(artifactDir, 'browser_bundle.js')), 'text/javascript']);
  const shards = (await readdir(resolve(artifactDir, 'browser')))
    .filter(name => /^guest\.component\.core\d*\.wasm$/.test(name));
  assert.ok(shards.length, 'no generated WASM shards; run make component-browser-smoke');
  for (const name of shards) {
    files.set(`/${name}`, [await readFile(resolve(artifactDir, 'browser', name)), 'application/wasm']);
  }
  const injected = fault === 'exception'
    ? 'throw new Error("CWIST_INJECTED_BROWSER_EXCEPTION");'
    : 'await import("./browser_bundle.js");';
  files.set('/', [Buffer.from(`<!doctype html><meta charset="utf-8"><link rel="icon" href="data:,"><script type="module">${injected}</script>`), 'text/html']);
  const served = new Set();
  const denied = new Set();
  const server = createServer((req, res) => {
    const entry = files.get(req.url);
    if (req.method !== 'GET' || !entry || (fault === 'missing-wasm' && req.url.endsWith('.wasm'))) {
      denied.add(req.url);
      res.writeHead(404, { 'Cache-Control': 'no-store' });
      res.end('Not found');
      return;
    }
    served.add(req.url);
    res.writeHead(200, { 'Content-Type': entry[1], 'Cache-Control': 'no-store' });
    res.end(entry[0]);
  });
  let browser;
  try {
    await new Promise((ok, fail) => {
      server.once('error', fail);
      server.listen(0, '127.0.0.1', ok);
    });
    const origin = `http://127.0.0.1:${server.address().port}`;
    browser = await chromium.launch({ headless: true, timeout: 15000 });
    const page = await browser.newPage();
    page.setDefaultTimeout(15000);
    // The fixture needs no network beyond this private server.
    await page.route('**/*', route => {
      if (new URL(route.request().url()).origin === origin) return route.continue();
      return route.abort('blockedbyclient');
    });
    const failures = [];
    let rejectFailure;
    const failed = new Promise((_, reject) => { rejectFailure = reject; });
    failed.catch(() => {});
    function fail(message) {
      failures.push(message);
      rejectFailure(new Error(message));
    }
    page.on('pageerror', error => fail(`browser exception: ${error.message}`));
    page.on('requestfailed', request => fail(`request failed: ${request.url()}`));
    page.on('response', response => {
      if (response.status() >= 400) fail(`HTTP ${response.status()}: ${response.url()}`);
    });
    try {
      await Promise.race([
        (async () => {
          await page.goto(origin, { waitUntil: 'load', timeout: 15000 });
          await page.waitForFunction(() => globalThis.__cwistBrowserResult !== undefined);
        })(),
        failed,
      ]);
      const result = await page.evaluate(() => globalThis.__cwistBrowserResult);
      assert.deepEqual(result, { get: true, headers: true, binaryPost: true });
      assert.equal(failures.length, 0, failures.join('\n'));
      assert.equal(denied.size, 0, `unexpected requests: ${[...denied]}`);
      for (const name of shards) assert.ok(served.has(`/${name}`), `WASM shard not fetched: ${name}`);
      // A browser success message without actual assertions/WASM loads cannot pass.
      console.log(`component-browser-runtime: PASS (${browser.version()}, GET + headers + binary POST, ${shards.length} WASM shards)`);
    } catch (error) {
      if (fault === 'missing-wasm' && [...denied].some(path => path.endsWith('.wasm'))) {
        throw new Error('CWIST_MISSING_WASM: browser requested an unavailable WASM shard', { cause: error });
      }
      throw error;
    }
  } finally {
    try {
      if (browser) await browser.close();
    } finally {
      server.closeAllConnections();
      await new Promise(resolveClose => server.close(resolveClose));
    }
  }
}

try {
  await run();
  if (selfTest) {
    for (const [mode, expected] of [
      ['missing-wasm', 'CWIST_MISSING_WASM:'],
      ['exception', 'browser exception: CWIST_INJECTED_BROWSER_EXCEPTION'],
    ]) {
      const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), `--fault=${mode}`], {
        encoding: 'utf8', timeout: 45000, maxBuffer: 1024 * 1024,
      });
      assert.ifError(child.error);
      assert.equal(child.signal, null, `negative control killed by ${child.signal}`);
      assert.equal(child.status, 1, `${mode}: expected exit 1, got ${child.status}\n${child.stdout}\n${child.stderr}`);
      assert.ok(child.stderr.includes(expected), `${mode}: wrong failure: ${child.stderr}`);
      console.log(`component-browser-runtime: PASS (negative control ${mode}, exit 1 + expected diagnostic)`);
    }
  }
} catch (error) {
  console.error(`component-browser-runtime: FAIL: ${error.stack || error}`);
  process.exitCode = 1;
}
