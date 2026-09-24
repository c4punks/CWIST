/* Browser-bundle entry for `make component-browser-smoke`.
 *
 * Copied into .jco-guest/<target>/ before bundling; esbuild resolves
 * 'cwist-guest-component' to the --no-nodejs-compat re-transpile via the
 * Makefile's --alias flag. `component-browser-smoke` bundles this file;
 * `component-browser-test` executes it and verifies the result in Chromium. */
import { guest, run } from 'cwist-guest-component';
import { createCwistFromComponent } from '../../wasm/npm/component.js';

await run.run();

const handle = createCwistFromComponent({
  dispatch: guest.dispatch,
  useSession: guest.useSession,
});

const res = await handle({ method: 'GET', path: '/hello' });
if (res.status !== 200) throw new Error('status: ' + res.status);
const text = new TextDecoder().decode(res.body);
if (text !== 'hello-from-wrapper-test') {
  throw new Error('body: ' + JSON.stringify(text));
}

if (res.headers['Content-Type'] !== 'text/plain' || res.headers['X-Wrapper-Test'] !== 'yes') {
  throw new Error('headers: ' + JSON.stringify(res.headers));
}

// Match the Node smoke payload; the fixture's echo handler uses C strings.
const payload = new Uint8Array([0x01, 0x02, 0xfe, 0xff]);
const echoed = await handle({ method: 'POST', path: '/echo', body: payload });
if (echoed.status !== 200 || echoed.body.length !== payload.length ||
    !payload.every((byte, index) => echoed.body[index] === byte)) {
  throw new Error('binary echo mismatch');
}

// The runner requires this exact result, not just a console message or page load.
globalThis.__cwistBrowserResult = { get: true, headers: true, binaryPost: true };
