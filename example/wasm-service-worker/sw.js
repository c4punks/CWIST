/*
 * CWIST WASM service-worker host (issue #93 Phase 4, component pipeline since
 * issue #203).
 *
 * Intercepts same-origin GET/POST fetches (everything except the static
 * assets listed in STATIC_PATHS) and dispatches them into the CWIST app,
 * built as a cwist-guest component (app.c, wit/cwist.wit) by
 * `make wasm-sw-example`. This file is the source: esbuild bundles it with the
 * cwist-wasm adapter, the jco transpile and the preview2-shim browser imports
 * into build/site/sw.js, a classic worker script.
 *
 * jco transpiles with --instantiation async: a service worker may not use
 * top-level await, which jco's default self-instantiating output needs, so the
 * component is instantiated here on first use instead.
 *
 * Cookies: a Service Worker does not get the page's document cookie jar, so
 * this worker keeps its own. Set-Cookie from a dispatch response is captured
 * into the jar (and is NOT forwarded to the page - Set-Cookie is a forbidden
 * response-header name in constructed SW responses) and the stored Cookie
 * header is attached to subsequent dispatches. See "The host carries the
 * cookie" in docs/api/wasm-component.md.
 */
import { instantiate } from './build/browser/app.component.js';
import * as wasiShim from '@bytecodealliance/preview2-shim';
import { createCwistFromComponent } from '../../wasm/npm/index.js';

/*
 * Pinned session signing secret, injected before the first dispatch so signed
 * session cookies survive instance restarts. In a real deployment store this
 * outside the bundle (KV, env var) and rotate it to force sign-out; anyone who
 * knows it can forge sessions. Must be long enough for
 * cwist_app_use_session() to accept it.
 */
const CWIST_SESSION_SECRET = 'cwist-sw-demo-secret-change-me-0123456789abcdef';

/* Precached at install and served cache-first; all other same-origin
 * GET/POST requests go to the CWIST app in WASM. */
const CACHE_NAME = 'cwist-sw-demo-v2';
const CORE_MODULES = ['app.component.core.wasm', 'app.component.core2.wasm',
                      'app.component.core3.wasm', 'app.component.core4.wasm'];
const STATIC_PATHS = new Set(['/index.html', '/sw.js', ...CORE_MODULES.map((m) => '/' + m)]);

let handlePromise = null;
const cookieJar = new Map(); /* cookie name -> value (this worker's own jar) */

/* The import object jco expects: 'wasi:<package>/<interface>' (or with @0.2.0) ->
 * the matching preview2-shim namespace (wasiShim.cli.terminalStdout, ...). */
function wasiImports() {
    const imports = {};
    for (const [pkg, ifaces] of Object.entries(wasiShim)) {
        for (const [name, impl] of Object.entries(ifaces)) {
            const kebab = name.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
            imports['wasi:' + pkg + '/' + kebab] = impl;
            imports['wasi:' + pkg + '/' + kebab + '@0.2.0'] = impl;
        }
    }
    return imports;
}

function getHandle() {
    if (!handlePromise) {
        handlePromise = (async () => {
            const root = await instantiate(
                (name) => WebAssembly.compileStreaming(fetch('./' + name)), wasiImports());
            root.run.run(); /* main(): routes, db schema, components */
            const handle = createCwistFromComponent(root.guest);
            handle.useSession(CWIST_SESSION_SECRET);
            return handle;
        })();
    }
    return handlePromise;
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
    const handle = await getHandle();
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

    const res = handle({
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
        await cache.addAll(['./index.html', './sw.js', ...CORE_MODULES.map((m) => './' + m)]);
        /* Instantiation is lazy (first fetch); doing it here removes that
         * latency from the first dispatch. */
        await getHandle();
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
