/* Drives the jco-transpiled cwist-guest-stream component (bundled by
 * `make component-stream-smoke` with tests/wasm_component_stream_host.js
 * aliased in as the c4punks:cwist/host import) through the cwist-wasm
 * component adapter, and asserts that handle.stream() delivers the
 * serialized response head-first through the async host send-chunk.
 *
 * Run under node's JSPI: node --experimental-wasm-jspi
 * tests/wasm_component_stream_test.js with BUNDLE pointing at the esbuild
 * bundle. */
'use strict';

const path = require('path');
const { createCwistFromComponent } = require(path.join(__dirname, '..', 'wasm', 'npm', 'component.js'));

const BUNDLE = process.env.BUNDLE;
if (!BUNDLE) {
    console.error('BUNDLE env var (esbuild bundle path) is required');
    process.exit(1);
}

(async () => {
    const component = await import(BUNDLE);

    /* Command component: run() executes main() (route registration). */
    await component.run.run();

    const handle = createCwistFromComponent({
        dispatch: component.guest.dispatch,
        useSession: component.guest.useSession,
        dispatchStream: component.guestStream.dispatchStream,
        streamHost: globalThis.__cwistStreamHost,
    });

    /* Sync WIT exports of a 0.3 component lower to async functions under
     * jco, same as the plain guest. */
    const res = await handle({ method: 'GET', path: '/hello' });
    if (res.status !== 200) {
        throw new Error('plain dispatch broken in stream world: ' + res.status);
    }

    /* Streaming dispatch: the whole serialized response arrives through
     * host.send-chunk, head first, body last. The callback awaits, so the
     * guest's pump waits on it (backpressure). */
    const chunks = [];
    await handle.stream({ method: 'GET', path: '/events' }, async (chunk) => {
        await new Promise((resolve) => setTimeout(resolve, 1));
        chunks.push(Buffer.from(chunk));
    });

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

    /* A failing callback aborts the dispatch, and stream() rejects with the
     * callback's own error rather than the guest's dispatch-error. */
    const boom = new Error('client went away');
    let seen = 0;
    let rejected = null;
    try {
        await handle.stream({ method: 'GET', path: '/events' }, () => {
            seen++;
            throw boom;
        });
    } catch (e) {
        rejected = e;
    }
    if (rejected !== boom) {
        throw new Error('expected the callback error, got: ' + rejected);
    }
    if (seen !== 1) {
        throw new Error('dispatch kept pumping after the callback failed: ' + seen + ' chunks');
    }

    /* The instance stays usable after an aborted stream. */
    const after = await handle({ method: 'GET', path: '/hello' });
    if (after.status !== 200) throw new Error('dispatch after aborted stream: ' + after.status);

    console.log('component-stream-smoke: PASS (streamed /events through host.send-chunk, ' +
                chunks.length + ' chunks)');
    process.exit(0);
})().catch((e) => {
    console.error(e);
    process.exit(1);
});
