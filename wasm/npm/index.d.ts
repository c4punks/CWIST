// Type definitions for cwist-wasm: the component adapter (component.d.ts)
// plus the HTTP framing helpers.

import type { CwistRequestInit, CwistResponse } from './component';

export * from './component';

/** Low-level helpers, exported for testing. */
export function buildRequestBytes(init: CwistRequestInit): Uint8Array;
export function parseResponseBytes(bytes: Uint8Array): CwistResponse;
