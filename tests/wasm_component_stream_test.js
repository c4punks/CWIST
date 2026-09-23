/* Drives the jco-transpiled cwist-guest-stream component (bundled by
 * `make component-stream-smoke` with tests/wasm_component_stream_host.js
 * aliased in as the c4punks:cwist/host import) and asserts that a
 * streaming dispatch delivers the serialized response head-first through
 * the async host send-chunk.
 *
 * Run under node's JSPI: node --experimental-wasm-jspi
 * tests/wasm_component_stream_test.js with BUNDLE pointing at the esbuild
 * bundle. */
'use strict';

const path = require('path');
const { buildRequestBytes } = require(path.join(__dirname, '..', 'wasm', 'npm', 'index.js'));

const BUNDLE = process.env.BUNDLE;
if (!BUNDLE) {
    console.error('BUNDLE env var (esbuild bundle path) is required');
    process.exit(1);
}

(async () => {
    const component = await import(BUNDLE);

    /* Command component: run() executes main() (route registration). */
    await component.run.run();

    /* Sync WIT exports of a 0.3 component lower to async functions under
     * jco, same as the plain guest. */
    const resBytes = await component.guest.dispatch(
        buildRequestBytes({ method: 'GET', path: '/hello' }));
    const head = new TextDecoder().decode(resBytes);
    if (!head.startsWith('HTTP/1.1 200')) {
        throw new Error('plain dispatch broken in stream world: ' + head.slice(0, 40));
    }

    /* Streaming dispatch: the whole serialized response arrives through
     * host.send-chunk, head first, body last. */
    const chunks = globalThis.__cwistStreamChunks;
    chunks.length = 0;
    await component.guestStream.dispatchStream(
        buildRequestBytes({ method: 'GET', path: '/events' }));

    if (chunks.length < 2) {
        throw new Error('expected head + body chunks, got ' + chunks.length);
    }
    const wire = Buffer.concat(chunks).toString('latin1');
    if (!wire.startsWith('HTTP/1.1 200')) {
        throw new Error('stream did not start with the head: ' + wire.slice(0, 40));
    }
    if (wire.indexOf('Content-Type: text/event-stream') === -1) {
        throw new Error('missing SSE content type: ' + wire.slice(0, 120));
    }
    if (wire.indexOf('data: one\n\ndata: two\n\n') === -1) {
        throw new Error('missing SSE body: ' + JSON.stringify(wire.slice(-80)));
    }

    console.log('component-stream-smoke: PASS (streamed /events through host.send-chunk, ' +
                chunks.length + ' chunks)');
    process.exit(0);
})().catch((e) => {
    console.error(e);
    process.exit(1);
});
