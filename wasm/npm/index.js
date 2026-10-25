/*
 * cwist-wasm: fetch-shaped JS API over a CWIST app built as a cwist-guest
 * WASI component (wit/cwist.wit) and transpiled with jco. See README.md and
 * docs/api/wasm-component.md.
 *
 *   const { createCwistFromComponent } = require('cwist-wasm');
 *   const handle = createCwistFromComponent(component.guest);
 *   const res = handle({ method: 'GET', path: '/hello' });
 *
 * 'cwist-wasm/component' resolves to the same adapter, for code written
 * before the component path became the package's main entry.
 */
'use strict';

const { createCwistFromComponent, createStreamHost } = require('./component.js');
const { buildRequestBytes, parseResponseBytes } = require('./http.js');

module.exports = { createCwistFromComponent, createStreamHost, buildRequestBytes, parseResponseBytes };
