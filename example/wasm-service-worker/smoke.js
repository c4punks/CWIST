'use strict';
/*
 * Node smoke test for the WASM service-worker example app (issue #93 Phase 4).
 *
 * The Service Worker itself only runs in a browser, so this drives the same
 * built module (app.js/app.wasm) through the real cwist-wasm JS wrapper
 * (wasm/npm, referenced by relative path - no external npm dependency) and
 * covers the app's WASM logic: routing, zod validation, template rendering,
 * cwist_db, the serialized-db image endpoint, sessions, and the 404 fallthrough.
 *
 * Run:  ./build.sh && node smoke.js
 */
const path = require('path');
const assert = require('assert');
const { createCwist } = require(path.join(__dirname, '..', '..', 'wasm', 'npm'));
const createCwistAppModule = require(path.join(__dirname, 'app.js'));

const SECRET = 'cwist-sw-demo-secret-change-me-0123456789abcdef';
const decode = (u8) => new TextDecoder().decode(u8);

(async () => {
    const Module = await createCwistAppModule();
    const handle = createCwist(Module);
    handle.useSession(SECRET);

    /* 1. GET / -> 200 HTML, first visit counts as visit #1 and sets a session
     *    cookie. */
    let res = handle({ method: 'GET', path: '/' });
    assert.strictEqual(res.status, 200, 'GET / status: ' + res.status);
    assert.match(res.headers['Content-Type'] || '', /text\/html/);
    let html = decode(res.body);
    assert.ok(html.includes('Session visits: <strong>1</strong>'), 'first visit count');
    assert.ok(html.includes('No items yet'), 'empty item list placeholder');
    const setCookie = res.headers['Set-Cookie'];
    assert.ok(setCookie && setCookie.indexOf('cwist_session=') === 0, 'session cookie set');
    const cookiePair = setCookie.split(';')[0];

    /* 2. GET / with the session cookie -> visit counter survives via the
     *    signed cookie (this is what the SW cookie jar carries). */
    res = handle({ method: 'GET', path: '/', headers: { Cookie: cookiePair } });
    assert.strictEqual(res.status, 200);
    html = decode(res.body);
    assert.ok(html.includes('Session visits: <strong>2</strong>'), 'second visit count');
    const cookiePair2 = (res.headers['Set-Cookie'] || '').split(';')[0];

    /* 3. POST /items with a valid body -> 201 and a db id. */
    res = handle({
        method: 'POST',
        path: '/items',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: 'coffee beans', qty: 3 }),
    });
    assert.strictEqual(res.status, 201, 'valid POST status: ' + res.status);
    const created = JSON.parse(decode(res.body));
    assert.strictEqual(created.ok, true);
    assert.strictEqual(typeof created.id, 'number');

    /* 4. POST /items missing "qty" -> 400 with the zod error list. */
    res = handle({
        method: 'POST',
        path: '/items',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: 'no qty here' }),
    });
    assert.strictEqual(res.status, 400, 'missing-field status: ' + res.status);
    let errs = JSON.parse(decode(res.body));
    assert.strictEqual(errs.ok, false);
    assert.ok(errs.errors.some((e) => e.field === 'qty'), 'qty error reported');

    /* 5. POST /items with the wrong type for "qty" -> 400 as well. */
    res = handle({
        method: 'POST',
        path: '/items',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: 'bad qty', qty: 'many' }),
    });
    assert.strictEqual(res.status, 400, 'wrong-type status: ' + res.status);
    errs = JSON.parse(decode(res.body));
    assert.ok(errs.errors.some((e) => e.field === 'qty'), 'type error reported');

    /* 6. GET /items -> the inserted row (values come back as strings, SQLite
     *    exec-callback semantics). */
    res = handle({ method: 'GET', path: '/items' });
    assert.strictEqual(res.status, 200);
    const items = JSON.parse(decode(res.body));
    assert.strictEqual(items.length, 1, 'exactly one item stored');
    assert.strictEqual(items[0].name, 'coffee beans');
    assert.strictEqual(items[0].qty, '3');

    /* 7. GET /items/image -> serialized SQLite image (edge-persistence blob). */
    res = handle({ method: 'GET', path: '/items/image' });
    assert.strictEqual(res.status, 200);
    assert.ok(res.body.length > 100, 'non-trivial db image: ' + res.body.length + ' bytes');
    assert.strictEqual(res.headers['Content-Type'], 'application/octet-stream');

    /* 7b. GET /items/list -> the component-rendered list: only the fragment
     *     for an htmx request, a full page linking a content-hashed
     *     stylesheet otherwise, and that stylesheet served by the module. */
    res = handle({ method: 'GET', path: '/items/list', headers: { 'HX-Request': 'true' } });
    assert.strictEqual(res.status, 200);
    assert.match(res.headers['Content-Type'] || '', /text\/html/);
    const fragment = decode(res.body);
    assert.ok(fragment.startsWith('<ul id="item-list"><li class="row-'), 'fragment: ' + fragment);
    assert.ok(fragment.includes('>coffee beans (3)</li>'), 'fragment row text');
    assert.ok(!fragment.includes('<html>'), 'fragment is not a page');

    res = handle({ method: 'GET', path: '/items/list' });
    assert.strictEqual(res.status, 200);
    const page = decode(res.body);
    assert.ok(page.startsWith('<!DOCTYPE html><html><head><link rel="stylesheet"'), 'page: ' + page);
    assert.ok(page.includes(fragment), 'page embeds the same list markup');
    const href = page.match(/href="([^"]+)"/)[1];
    assert.match(href, /^\/assets\/list\.[0-9a-f]{16}\.css$/);

    res = handle({ method: 'GET', path: href });
    assert.strictEqual(res.status, 200);
    assert.strictEqual(res.headers['Cache-Control'], 'public, max-age=31536000, immutable');
    assert.match(res.headers['Content-Type'] || '', /text\/css/);
    assert.ok(decode(res.body).includes('.row-'), 'scoped rule in the stylesheet');

    /* 8. GET /items on a second "instance" with the same pinned secret still
     *    sees the data (in-memory db is per-instance; sessions are not). */
    const Module2 = await createCwistAppModule();
    const handle2 = createCwist(Module2);
    handle2.useSession(SECRET);
    res = handle2({ method: 'GET', path: '/', headers: { Cookie: cookiePair2 } });
    assert.ok(decode(res.body).includes('Session visits: <strong>3</strong>'),
              'session survives instance swap with pinned secret');

    /* 9. Unknown route -> the router's default 404. */
    res = handle({ method: 'GET', path: '/no-such-page' });
    assert.strictEqual(res.status, 404, 'fallback status: ' + res.status);

    console.log('wasm-service-worker smoke: OK (routing, zod 400s, template, db, image, components, assets, session, 404)');
})().catch((err) => {
    console.error(err);
    process.exit(1);
});
