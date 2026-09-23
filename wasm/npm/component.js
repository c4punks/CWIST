/*
 * cwist-wasm component adapter: binds a jco-transpiled cwist-guest component
 * (issue #203) to the same fetch-shaped API createCwist() exposes for
 * Emscripten modules. The component exports dispatch and useSession from
 * wit/cwist.wit; the canonical ABI hands request/response buffers across as
 * Uint8Array copies, so no malloc/HEAP plumbing is needed here.
 *
 *   const { instantiate } = await import('./component.js'); // jco output
 *   const component = await instantiate(imports);
 *   const handle = createCwistFromComponent(component);
 *   const res = handle({ method: 'GET', path: '/hello' });
 *   // res.status, res.headers, res.body (Uint8Array) — same as createCwist()
 *   // Against a 0.3 (wasip3) component handle() returns a Promise: await it.
 */
'use strict';

const { buildRequestBytes, parseResponseBytes } = require('./index.js');

/**
 * Create a CWIST request handler bound to an instantiated cwist-guest
 * component (jco transpile output of a guest built for wit/cwist.wit).
 * @param {Object} component instantiated component exports
 * @returns {(init: Object) => Object} same handler shape as createCwist()
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
   * time, before any dispatch (mirrors createCwist's Module.cwistSessionSecret
   * handling). */
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

  handle.useSession = useSession;
  return handle;
}

module.exports = { createCwistFromComponent };
