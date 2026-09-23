/* Host implementation of the c4punks:cwist/host import for
 * `make component-stream-smoke-p3`. esbuild aliases the bare specifier
 * 'c4punks:cwist/host' to this file when bundling the transpiled stream
 * guest, which is how a bundler consumer wires a custom host import.
 *
 * Chunks are collected on globalThis so the test driver can read them
 * through its own (esbuild-bundled) copy of this module. */

export const chunks = [];
globalThis.__cwistStreamChunks = chunks;

export async function sendChunk(chunk) {
    /* chunk arrives as a Uint8Array copy owned by the canonical ABI. */
    chunks.push(Buffer.from(chunk));
}
