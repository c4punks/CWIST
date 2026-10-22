/* Host implementation of the c4punks:cwist/host import for
 * `make component-stream-smoke-p3`. esbuild aliases the bare specifier
 * 'c4punks:cwist/host' to this file when bundling the transpiled stream
 * guest, which is how a bundler consumer wires a custom host import.
 *
 * The import is the cwist-wasm adapter's createStreamHost(); the host
 * object is published on globalThis so the test driver can bind its
 * handle to the same instance the bundled guest calls into. */
import { createStreamHost } from '../wasm/npm/component.js';

const host = createStreamHost();
globalThis.__cwistStreamHost = host;

export const sendChunk = host.sendChunk;
