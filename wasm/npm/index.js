/*
 * cwist-wasm: thin fetch-shaped wrapper over CWIST's WASM dispatch path.
 *
 * The consumer compiles their own app (routes + handlers) against
 * libcwist_wasm.a with Emscripten, exporting the standard entry points:
 *
 *   - _cwist_wasm_dispatch(req_ptr, req_len, out_len_ptr) -> res_ptr
 *   - _cwist_wasm_dispose(ptr)
 *   - malloc/free exported (-sEXPORTED_FUNCTIONS=_malloc,_free,...)
 *   - HEAPU8 and HEAPU32 in EXPORTED_RUNTIME_METHODS
 *
 * This module hides all of that: build the request bytes, hand them over,
 * get back {status, headers, body} with body as a Uint8Array copy.
 */
'use strict';

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder();

function requireFn(mod, names, what) {
  for (const n of names) {
    if (typeof mod[n] === 'function') return mod[n];
  }
  throw new Error(
    'cwist-wasm: module does not expose ' + what + ' (looked for: ' + names.join(', ') + '). ' +
      'Compile the app with the CWIST WASM entry points exported; see wasm/npm/README.md.'
  );
}

/**
 * Build a raw HTTP/1.1 request from a fetch-like init object.
 * @param {{method?: string, path?: string, headers?: Object, body?: (string|Uint8Array)}} init
 * @returns {Uint8Array}
 */
function buildRequestBytes(init) {
  const method = (init.method || 'GET').toUpperCase();
  const path = init.path || '/';
  const headers = init.headers || {};
  let body = init.body;
  if (typeof body === 'string') body = ENCODER.encode(body);
  if (body == null) body = new Uint8Array(0);

  let head = method + ' ' + path + ' HTTP/1.1\r\nHost: wasm\r\n';
  let hasLength = false;
  for (const [k, v] of Object.entries(headers)) {
    if (k.toLowerCase() === 'content-length') hasLength = true;
    head += k + ': ' + v + '\r\n';
  }
  if (body.length > 0 && !hasLength) head += 'Content-Length: ' + body.length + '\r\n';
  head += 'Connection: close\r\n\r\n';

  const headBytes = ENCODER.encode(head);
  const out = new Uint8Array(headBytes.length + body.length);
  out.set(headBytes, 0);
  out.set(body, headBytes.length);
  return out;
}

/**
 * Parse a serialized HTTP/1.1 response into parts.
 * @param {Uint8Array} bytes
 * @returns {{status: number, statusText: string, headers: Object, body: Uint8Array}}
 */
function parseResponseBytes(bytes) {
  // Header/body split: find CRLFCRLF. Responses from dispatch_memory always
  // carry the full header block before the body.
  let sep = -1;
  for (let i = 0; i + 3 < bytes.length; i++) {
    if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 && bytes[i + 3] === 10) {
      sep = i;
      break;
    }
  }
  if (sep < 0) throw new Error('cwist-wasm: malformed response (no header terminator)');
  const headText = DECODER.decode(bytes.subarray(0, sep));
  const lines = headText.split('\r\n');
  const statusMatch = /^HTTP\/\d\.\d (\d{3}) ?(.*)$/.exec(lines[0] || '');
  if (!statusMatch) throw new Error('cwist-wasm: malformed status line: ' + JSON.stringify(lines[0]));
  const headers = {};
  for (let i = 1; i < lines.length; i++) {
    const idx = lines[i].indexOf(':');
    if (idx > 0) headers[lines[i].slice(0, idx).trim()] = lines[i].slice(idx + 1).trim();
  }
  // Copy: the WASM heap may be reused by the next dispatch.
  const body = bytes.slice(sep + 4);
  return {
    status: parseInt(statusMatch[1], 10),
    statusText: statusMatch[2],
    headers,
    body,
  };
}

/**
 * Create a CWIST request handler bound to an instantiated Emscripten module.
 * @param {Object} mod instantiated Emscripten module (Module global of the
 *   consumer's compiled app).
 * @returns {(init: Object) => Object} handler taking a fetch-like init and
 *   returning {status, statusText, headers, body(Uint8Array)}.
 */
function createCwist(mod) {
  if (!mod || typeof mod !== 'object') throw new Error('cwist-wasm: module object required');
  const malloc = requireFn(mod, ['_malloc'], 'malloc');
  const free = requireFn(mod, ['_free'], 'free');
  const dispatch = requireFn(mod, ['_cwist_wasm_dispatch'], 'the CWIST WASM dispatch entry point');
  const cwistFree = requireFn(mod, ['_cwist_wasm_dispose'], 'the CWIST WASM dispose entry point');
  if (typeof mod.HEAPU8 !== 'object' || typeof mod.HEAPU32 !== 'object') {
    throw new Error(
      'cwist-wasm: module must export HEAPU8 and HEAPU32 (add them to -sEXPORTED_RUNTIME_METHODS)'
    );
  }

  /* Session secret (issue #93): the module only exports the setter when it
   * was built with a current-enough CWIST_WASM_DEFINE_ENTRY; treat it as
   * optional so older modules keep working. */
  const useSessionFn =
    typeof mod._cwist_wasm_use_session === 'function' ? mod._cwist_wasm_use_session : null;

  function useSession(secret) {
    if (!useSessionFn) {
      throw new Error(
        'cwist-wasm: module does not export _cwist_wasm_use_session; rebuild with an up-to-date wasm_entry.h'
      );
    }
    const bytes = ENCODER.encode(secret == null ? '' : String(secret));
    const ptr = malloc(bytes.length + 1);
    if (!ptr) throw new Error('cwist-wasm: malloc failed (session secret)');
    try {
      mod.HEAPU8.set(bytes, ptr);
      mod.HEAPU8[ptr + bytes.length] = 0;
      /* NULL/empty secret -> CWIST generates a per-instance random secret. */
      const rc = useSessionFn(bytes.length > 0 ? ptr : 0);
      if (rc !== 0) throw new Error('cwist-wasm: use_session failed (rc=' + rc + ')');
    } finally {
      free(ptr);
    }
  }

  /* Declarative form: a Module.cwistSessionSecret string is applied once at
   * binding time, before any dispatch. */
  if (useSessionFn && typeof mod.cwistSessionSecret === 'string') {
    useSession(mod.cwistSessionSecret);
  }

  function handle(init) {
    const reqBytes = buildRequestBytes(init || {});
    const reqPtr = malloc(reqBytes.length || 1);
    if (!reqPtr) throw new Error('cwist-wasm: malloc failed (' + reqBytes.length + ' bytes)');
    let resPtr = 0;
    let outLenPtr = 0;
    try {
      mod.HEAPU8.set(reqBytes, reqPtr);
      outLenPtr = malloc(8);
      if (!outLenPtr) throw new Error('cwist-wasm: malloc failed (out-len slot)');
      resPtr = dispatch(reqPtr, reqBytes.length, outLenPtr);
      if (!resPtr) throw new Error('cwist-wasm: dispatch failed (NULL response)');
      const resLen = mod.HEAPU32[outLenPtr >> 2];
      const view = mod.HEAPU8.subarray(resPtr, resPtr + resLen);
      return parseResponseBytes(view);
    } finally {
      if (resPtr) cwistFree(resPtr);
      if (outLenPtr) free(outLenPtr);
      free(reqPtr);
    }
  }

  handle.useSession = useSession;
  return handle;
}

module.exports = { createCwist, buildRequestBytes, parseResponseBytes };
