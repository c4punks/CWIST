/*
 * cwist-wasm component adapter: binds a jco-transpiled cwist-guest component
 * (issue #203) to a fetch-shaped API. The component exports dispatch and useSession from
 * wit/cwist.wit; the canonical ABI hands request/response buffers across as
 * Uint8Array copies, so no malloc/HEAP plumbing is needed here.
 *
 *   const { instantiate } = await import('./component.js'); // jco output
 *   const component = await instantiate(imports);
 *   const handle = createCwistFromComponent(component);
 *   const res = handle({ method: 'GET', path: '/hello' });
 *   // res.status, res.headers, res.body (Uint8Array)
 *   // Against a 0.3 (wasip3) component handle() returns a Promise: await it.
 *
 * Streaming (cwist-guest-stream world, 0.3 only): the guest pushes chunks
 * through the c4punks:cwist/host import, which is wired when the component
 * is instantiated or bundled. createStreamHost() returns that import;
 * handing the same host to the adapter routes each chunk to the callback
 * of the handle.stream() call in flight:
 *
 *   const host = createStreamHost();      // { sendChunk } -> host import
 *   const handle = createCwistFromComponent({ ...guest,
 *     dispatchStream: guestStream.dispatchStream, streamHost: host });
 *   await handle.stream({ path: '/events' }, (chunk) => { ... });
 */
'use strict';

const { buildRequestBytes, parseResponseBytes } = require('./http.js');

/* Internal: the dispatch runner behind each stream host. Kept off the host
 * object so the host is exactly the shape of the WIT import.
 *
 * The registry must live on globalThis: a bundler (esbuild alias, webpack)
 * inlines a second copy of this module into the guest bundle when the host
 * import references createStreamHost(), while the consumer's adapter comes
 * from the npm package — two module instances, two WeakMaps, and the lookup
 * in stream() would miss every registration made through the bundled copy. */
const STREAM_RUNNERS = (globalThis.__cwistStreamRunners ||= new WeakMap());

/* jco turns a thrown value carrying `payload` into the err arm of the
 * import's result<_, string>; the guest then aborts the dispatch. */
function streamFailure(message) {
  return { payload: message };
}

/**
 * Create the c4punks:cwist/host import for a cwist-guest-stream component.
 * Pass the returned object (or its sendChunk) as the host import, and the
 * same object as `streamHost` to createCwistFromComponent().
 * @returns {{sendChunk: (chunk: Uint8Array) => Promise<void>}}
 */
function createStreamHost() {
  let sink = null;
  let sinkError = null;

  /* Awaited by the guest: delaying resolution is the backpressure. */
  async function sendChunk(chunk) {
    if (!sink) throw streamFailure('cwist-wasm: send-chunk outside a streaming dispatch');
    try {
      await sink(chunk);
    } catch (e) {
      sinkError = { error: e };
      throw streamFailure('cwist-wasm: chunk callback failed: ' + String(e && e.message ? e.message : e));
    }
  }

  const host = { sendChunk };
  STREAM_RUNNERS.set(host, async function run(onChunk, dispatch) {
    sink = onChunk;
    sinkError = null;
    try {
      await dispatch();
    } catch (e) {
      /* A failed callback aborts the guest's pump, which then reports a
       * dispatch-error; surface the callback's own error instead. */
      if (sinkError) throw sinkError.error;
      throw e;
    } finally {
      sink = null;
    }
    if (sinkError) throw sinkError.error;
  });
  return host;
}

/**
 * Create a CWIST request handler bound to an instantiated cwist-guest
 * component (jco transpile output of a guest built for wit/cwist.wit).
 * @param {Object} component instantiated component exports
 * @returns {(init: Object) => Object} handler taking a fetch-like init and
 *   returning {status, statusText, headers, body(Uint8Array)}
 */
function createCwistFromComponent(component) {
  if (!component || typeof component !== 'object') {
    throw new Error('cwist-wasm: component object required');
  }
  if (typeof component.dispatch !== 'function') {
    throw new Error(
      'cwist-wasm: component does not expose dispatch; build the guest against wit/cwist.wit (see docs/api/wasm-component.md)'
    );
  }

  /* dispatch() returns the serialized response bytes; a dispatch-error
   * variant surfaces as a thrown ComponentError from jco. jco lowers
   * exports of 0.3 (wasip3) components to async functions, so handle()
   * returns a Promise there; 0.2 exports are plain synchronous calls. */
  const asyncExports = typeof component.dispatch === 'function' &&
    component.dispatch[Symbol.toStringTag] === 'AsyncFunction';
  let chain = Promise.resolve();

  function useSession(secret) {
    if (typeof component.useSession !== 'function') {
      throw new Error('cwist-wasm: component does not expose useSession');
    }
    /* null/undefined lets CWIST generate a per-instance random secret. */
    const arg = secret == null ? null : String(secret);
    if (asyncExports) {
      /* Serialize against pending dispatches: the secret must be applied
       * before any later dispatch runs. */
      chain = chain.then(() => component.useSession(arg));
      return;
    }
    component.useSession(arg);
  }

  /* Declarative form: component.cwistSessionSecret applied once at binding
   * time, before any dispatch. */
  if (typeof component.useSession === 'function' &&
      typeof component.cwistSessionSecret === 'string') {
    useSession(component.cwistSessionSecret);
  }

  function handle(init) {
    const request = buildRequestBytes(init || {});
    if (asyncExports) {
      return chain.then(() => component.dispatch(request)).then(parseResponseBytes);
    }
    return parseResponseBytes(component.dispatch(request));
  }

  /* One sink per host, so streaming dispatches run one at a time; they also
   * wait for any pending useSession(). */
  let streamQueue = Promise.resolve();

  function stream(init, onChunk) {
    if (typeof component.dispatchStream !== 'function') {
      throw new Error(
        'cwist-wasm: component does not expose dispatchStream; build the guest for the cwist-guest-stream world (wasm32-wasip3)'
      );
    }
    const run = component.streamHost && STREAM_RUNNERS.get(component.streamHost);
    if (!run) {
      throw new Error(
        'cwist-wasm: streaming needs streamHost, the createStreamHost() object wired as the c4punks:cwist/host import'
      );
    }
    if (typeof onChunk !== 'function') throw new Error('cwist-wasm: stream() needs a chunk callback');
    const request = buildRequestBytes(init || {});
    const result = Promise.all([chain, streamQueue])
      .then(() => run(onChunk, () => component.dispatchStream(request)));
    streamQueue = result.then(() => {}, () => {});
    return result;
  }

  handle.useSession = useSession;
  handle.stream = stream;
  return handle;
}

module.exports = { createCwistFromComponent, createStreamHost };
