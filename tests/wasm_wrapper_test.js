/* Drives tests/wasm_wrapper_test.c (compiled to wrapper_test.js) through the
 * cwist-wasm wrapper and asserts the fetch-shaped API end to end. Run by
 * `make wasm-wrapper-test` after the module is built (MODULARIZE output:
 * the glue exports createCwistModule() instead of populating a global). */
'use strict';

const path = require('path');
const { createCwist } = require(path.join(__dirname, '..', 'wasm', 'npm'));
const createCwistModule = require(path.join(process.cwd(), 'wrapper_test.js'));

(async () => {
  const Module = await createCwistModule();
  const handle = createCwist(Module);

  /* 1. GET with no body: status, headers, payload all verified. */
  const res = handle({ method: 'GET', path: '/hello' });
if (res.status !== 200) throw new Error('status: ' + res.status);
if (res.headers['Content-Type'] !== 'text/plain') {
  throw new Error('Content-Type: ' + res.headers['Content-Type']);
}
if (res.headers['X-Wrapper-Test'] !== 'yes') {
  throw new Error('X-Wrapper-Test missing: ' + JSON.stringify(res.headers));
}
const text = new TextDecoder().decode(res.body);
if (text !== 'hello-from-wrapper-test') throw new Error('body: ' + JSON.stringify(text));

/* 2. POST with a Uint8Array body: request body plumbing into C handlers.
 * Avoid a leading NUL: the echo handler uses cwist_sstring_assign, which
 * is strlen-based. */
const payload = new Uint8Array([0x01, 0x02, 0xfe, 0xff]);
const echoed = handle({ method: 'POST', path: '/echo', body: payload });
if (echoed.status !== 200) throw new Error('echo status: ' + echoed.status);
if (echoed.body.length !== payload.length ||
    !payload.every((b, i) => echoed.body[i] === b)) {
  throw new Error('echo body mismatch: ' + Array.from(echoed.body));
}

/* 3. Response body must be a copy: mutate it, dispatch again, expect the
 * original bytes. */
res.body[0] = 0x58;
const again = handle({ method: 'GET', path: '/hello' });
if (new TextDecoder().decode(again.body) !== 'hello-from-wrapper-test') {
  throw new Error('body view aliasing detected');
}

console.log('wasm_wrapper_test: OK (GET, POST echo, body-copy isolation)');
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
