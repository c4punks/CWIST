/*
 * CWIST WASM service-worker host (issue #93 Phase 4).
 *
 * Intercepts same-origin GET/POST fetches (everything except the static
 * assets listed in STATIC_PATHS) and dispatches them into the CWIST app
 * compiled to WASM (app.js/app.wasm from build.sh). The request/response
 * serialization below mirrors wasm/npm/index.js (the cwist-wasm wrapper);
 * it is inlined so the worker stays self-contained - no npm install, no
 * network - and loads the Emscripten glue with importScripts().
 *
 * Cookies: a Service Worker does not get the page's document cookie jar, so
 * this worker keeps its own. Set-Cookie from a dispatch response is captured
 * into the jar (and is NOT forwarded to the page - Set-Cookie is a forbidden
 * response-header name in constructed SW responses) and the stored Cookie
 * header is attached to subsequent dispatches. See "The host carries the
 * cookie" in docs/api/wasm.md.
 */
'use strict';

importScripts('./app.js'); /* MODULARIZE output: defines createCwistAppModule() */

/*
 * Pinned session signing secret, injected into the module before the first
 * dispatch so signed session cookies survive module restarts. In a real
 * deployment store this outside the bundle (KV, env var) and rotate it to
 * force sign-out; anyone who knows it can forge sessions. Must be long enough
 * for cwist_app_use_session() to accept it.
 */
const CWIST_SESSION_SECRET = 'cwist-sw-demo-secret-change-me-0123456789abcdef';

/* Precached at install and served cache-first; all other same-origin
 * GET/POST requests go to the CWIST app in WASM. */
const CACHE_NAME = 'cwist-sw-demo-v1';
const STATIC_PATHS = new Set(['/index.html', '/sw.js', '/app.js', '/app.wasm']);

let modulePromise = null;
const cookieJar = new Map(); /* cookie name -> value (this worker's own jar) */

function getModule() {
    if (!modulePromise) {
        modulePromise = createCwistAppModule().then((mod) => {
            injectSessionSecret(mod);
            return mod;
        });
    }
    return modulePromise;
}

/* Pin the session secret: copy the string into the WASM heap and call the
 * exported _cwist_wasm_use_session (the C side copies it again). */
function injectSessionSecret(mod) {
    if (typeof mod._cwist_wasm_use_session !== 'function') return;
    const bytes = new TextEncoder().encode(CWIST_SESSION_SECRET);
    const ptr = mod._malloc(bytes.length + 1);
    if (!ptr) throw new Error('malloc failed (session secret)');
    mod.HEAPU8.set(bytes, ptr);
    mod.HEAPU8[ptr + bytes.length] = 0;
    try {
        if (mod._cwist_wasm_use_session(ptr) !== 0)
            throw new Error('cwist_wasm_use_session failed');
    } finally {
        mod._free(ptr);
    }
}

/* ---- request/response serialization (mirrors wasm/npm/index.js) ---- */

function buildRequestBytes(init) {
    const encoder = new TextEncoder();
    const method = (init.method || 'GET').toUpperCase();
    const path = init.path || '/';
    let body = init.body;
    if (typeof body === 'string') body = encoder.encode(body);
    if (body == null) body = new Uint8Array(0);

    let head = method + ' ' + path + ' HTTP/1.1\r\nHost: wasm\r\n';
    let hasLength = false;
    for (const [k, v] of Object.entries(init.headers || {})) {
        if (k.toLowerCase() === 'content-length') hasLength = true;
        head += k + ': ' + v + '\r\n';
    }
    if (body.length > 0 && !hasLength) head += 'Content-Length: ' + body.length + '\r\n';
    head += 'Connection: close\r\n\r\n';

    const headBytes = encoder.encode(head);
    const out = new Uint8Array(headBytes.length + body.length);
    out.set(headBytes, 0);
    out.set(body, headBytes.length);
    return out;
}

function parseResponseBytes(bytes) {
    const decoder = new TextDecoder();
    let sep = -1;
    for (let i = 0; i + 3 < bytes.length; i++) {
        if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 &&
            bytes[i + 3] === 10) {
            sep = i;
            break;
        }
    }
    if (sep < 0) throw new Error('malformed dispatch response (no header terminator)');
    const lines = decoder.decode(bytes.subarray(0, sep)).split('\r\n');
    const statusMatch = /^HTTP\/\d\.\d (\d{3}) ?(.*)$/.exec(lines[0] || '');
    if (!statusMatch) throw new Error('malformed dispatch status line: ' + lines[0]);
    const headers = {};
    for (let i = 1; i < lines.length; i++) {
        const idx = lines[i].indexOf(':');
        if (idx > 0) headers[lines[i].slice(0, idx).trim()] = lines[i].slice(idx + 1).trim();
    }
    return {
        status: parseInt(statusMatch[1], 10),
        statusText: statusMatch[2],
        headers,
        body: bytes.slice(sep + 4), /* copy: the WASM heap is reused per dispatch */
    };
}

/* One dispatch: request bytes in, {status, headers, body} out. */
function dispatch(mod, init) {
    const reqBytes = buildRequestBytes(init);
    const reqPtr = mod._malloc(reqBytes.length || 1);
    if (!reqPtr) throw new Error('malloc failed (request)');
    let resPtr = 0;
    let outLenPtr = 0;
    try {
        mod.HEAPU8.set(reqBytes, reqPtr);
        outLenPtr = mod._malloc(8);
        if (!outLenPtr) throw new Error('malloc failed (out-len slot)');
        resPtr = mod._cwist_wasm_dispatch(reqPtr, reqBytes.length, outLenPtr);
        if (!resPtr) throw new Error('cwist dispatch failed (NULL response)');
        const resLen = mod.HEAPU32[outLenPtr >> 2];
        return parseResponseBytes(mod.HEAPU8.subarray(resPtr, resPtr + resLen));
    } finally {
        if (resPtr) mod._cwist_wasm_dispose(resPtr);
        if (outLenPtr) mod._free(outLenPtr);
        mod._free(reqPtr);
    }
}

/* ---- the worker's own cookie jar ---- */

function captureSetCookies(headers) {
    const setCookie = headers['Set-Cookie'];
    if (!setCookie) return;
    const pair = setCookie.split(';')[0];
    const eq = pair.indexOf('=');
    if (eq <= 0) return;
    const name = pair.slice(0, eq).trim();
    const value = pair.slice(eq + 1).trim();
    /* Deletion: cwist_cookie_delete() emits "name=; Path=/; Max-Age=0". */
    if (value === '' || /;\s*Max-Age=0/i.test(setCookie)) {
        cookieJar.delete(name);
        return;
    }
    cookieJar.set(name, value);
}

function cookieHeader() {
    const parts = [];
    for (const [name, value] of cookieJar) parts.push(name + '=' + value);
    return parts.join('; ');
}

/* ---- fetch interception ---- */

async function handleFetch(request) {
    const mod = await getModule();
    const url = new URL(request.url);

    const headers = {};
    request.headers.forEach((value, key) => {
        const lk = key.toLowerCase();
        /* Host/Connection/Content-Length are synthesized or hop-by-hop; the
         * Cookie header comes from our own jar, not the document jar. */
        if (lk === 'host' || lk === 'connection' || lk === 'content-length' || lk === 'cookie')
            return;
        headers[key] = value;
    });
    const jar = cookieHeader();
    if (jar) headers['Cookie'] = jar;

    let body = null;
    if (request.method !== 'GET' && request.method !== 'HEAD')
        body = new Uint8Array(await request.arrayBuffer());

    const res = dispatch(mod, {
        method: request.method,
        path: url.pathname + url.search,
        headers: headers,
        body: body,
    });

    captureSetCookies(res.headers);

    const responseHeaders = new Headers();
    for (const [key, value] of Object.entries(res.headers)) {
        const lk = key.toLowerCase();
        /* Set-Cookie: forbidden on constructed SW responses (the jar carries
         * it); the rest are hop-by-hop or synthesized by the Response. */
        if (lk === 'set-cookie' || lk === 'connection' || lk === 'keep-alive' ||
            lk === 'transfer-encoding' || lk === 'content-length') {
            continue;
        }
        try {
            responseHeaders.append(key, value);
        } catch (e) {
            /* Forbidden/forbidden-response header name: skip it. */
        }
    }
    return new Response(res.body, {
        status: res.status,
        statusText: res.statusText,
        headers: responseHeaders,
    });
}

self.addEventListener('install', (event) => {
    event.waitUntil((async () => {
        const cache = await caches.open(CACHE_NAME);
        await cache.addAll(['./index.html', './sw.js', './app.js', './app.wasm']);
        /* Instantiation of the WASM module is lazy (first fetch); preloading
         * it here removes that latency from the first dispatch. */
        await getModule();
        await self.skipWaiting();
    })());
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    if (url.origin !== self.location.origin) return; /* cross-origin: browser default */
    if (event.request.method !== 'GET' && event.request.method !== 'POST') return;
    if (STATIC_PATHS.has(url.pathname)) {
        /* Static assets: cache-first, network fallback. */
        event.respondWith(
            caches.match(event.request).then((cached) => cached || fetch(event.request))
        );
        return;
    }
    event.respondWith(handleFetch(event.request));
});
