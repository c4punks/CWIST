/* Drives the jco-transpiled cwist-guest component through the cwist-wasm
 * component adapter and asserts the same fetch-shaped API as
 * tests/wasm_wrapper_test.js (assertions kept in sync on purpose).
 *
 * Run by `make component-smoke` (wasip2, preview2-shim) and
 * `make component-smoke-p3` (wasip3, preview3-shim under JSPI) after the
 * guest is built, componentized, and transpiled. JCO_DIR selects the
 * transpiled output; the Makefile points it at .jco-guest/<target>. */
'use strict';

const path = require('path');
const fs = require('fs');
const { createCwistFromComponent } = require(path.join(__dirname, '..', 'wasm', 'npm', 'component.js'));

const JCO_DIR = path.resolve(process.cwd(), process.env.JCO_DIR || '.jco-guest');

/* jco emits ESM; make node treat the generated dir as a module package. */
if (!fs.existsSync(path.join(JCO_DIR, 'package.json'))) {
  fs.writeFileSync(path.join(JCO_DIR, 'package.json'), JSON.stringify({ type: 'module' }));
}

(async () => {
  /* The jco module self-instantiates on import and exports the component's
   * interfaces directly: `guest` ({dispatch, useSession}) and `run`. WASI
   * imports are satisfied by @bytecodealliance/preview2-shim, installed into
   * .jco-guest/node_modules by `make component-smoke`. */
  const component = await import(path.join(JCO_DIR, 'guest.component.js'));

  /* Command component: run() executes main() (route registration). A
   * non-zero exit or error result surfaces as a thrown ComponentError. */
  await component.run.run();

  const handle = createCwistFromComponent({
    dispatch: component.guest.dispatch,
    useSession: component.guest.useSession,
  });

  /* 1. GET with no body. */
  const res = await handle({ method: 'GET', path: '/hello' });
  if (res.status !== 200) throw new Error('status: ' + res.status);
  if (res.headers['Content-Type'] !== 'text/plain') {
    throw new Error('Content-Type: ' + res.headers['Content-Type']);
  }
  if (res.headers['X-Wrapper-Test'] !== 'yes') {
    throw new Error('X-Wrapper-Test missing: ' + JSON.stringify(res.headers));
  }
  const text = new TextDecoder().decode(res.body);
  if (text !== 'hello-from-wrapper-test') throw new Error('body: ' + JSON.stringify(text));

  /* 2. POST with a Uint8Array body: echo plumbing into C handlers. */
  const payload = new Uint8Array([0x01, 0x02, 0xfe, 0xff]);
  const echoed = await handle({ method: 'POST', path: '/echo', body: payload });
  if (echoed.status !== 200) throw new Error('echo status: ' + echoed.status);
  if (echoed.body.length !== payload.length ||
      !payload.every((b, i) => echoed.body[i] === b)) {
    throw new Error('echo body mismatch: ' + Array.from(echoed.body));
  }

  /* 3. Response body must be a copy across dispatches. */
  res.body[0] = 0x58;
  const again = await handle({ method: 'GET', path: '/hello' });
  if (new TextDecoder().decode(again.body) !== 'hello-from-wrapper-test') {
    throw new Error('body view aliasing detected');
  }

  /* 4. Host-injected session secret, then a signed-cookie roundtrip. */
  handle.useSession('wrapper-test-secret-0123456789abcdef');
  const setRes = await handle({ method: 'GET', path: '/session/set' });
  if (setRes.status !== 200) throw new Error('session/set status: ' + setRes.status);
  const setCookie = setRes.headers['Set-Cookie'];
  if (!setCookie || setCookie.indexOf('cwist_session=') !== 0) {
    throw new Error('missing signed session cookie: ' + JSON.stringify(setRes.headers));
  }
  const cookiePair = setCookie.split(';')[0];
  const getRes = await handle({ method: 'GET', path: '/session/get', headers: { Cookie: cookiePair } });
  if (new TextDecoder().decode(getRes.body) !== 'alice') {
    throw new Error('session roundtrip: ' + new TextDecoder().decode(getRes.body));
  }

  /* 5. Unparseable input surfaces as a dispatch-error, not a crash. An
   * unknown-but-parseable route is a serialized 404, matching the
   * Emscripten path, so exercise the error variant with raw bytes. */
  let failed = false;
  try {
    const out = component.guest.dispatch(new Uint8Array([0x00, 0xff, 0x00]));
    if (out && typeof out.then === 'function') await out;
  } catch (e) {
    failed = true;
  }
  if (!failed) throw new Error('expected dispatch error for unparseable input');

  console.log('component-smoke: PASS (jco-transpiled guest served /hello over the component boundary)');
  /* preview3-shim keeps the node event loop alive (stream/future handles),
   * so an explicit exit is required on the wasip3 path. */
  process.exit(0);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
