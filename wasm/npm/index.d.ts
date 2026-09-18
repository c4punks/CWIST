// Type definitions for the cwist-wasm JS wrapper.

export interface CwistRequestInit {
  method?: string;
  path?: string;
  headers?: Record<string, string>;
  body?: string | Uint8Array;
}

export interface CwistResponse {
  status: number;
  statusText: string;
  headers: Record<string, string>;
  /** Copy of the response body; safe to keep across subsequent calls. */
  body: Uint8Array;
}

/** Minimal shape of the instantiated Emscripten module the wrapper needs. */
export interface CwistModule {
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _cwist_wasm_dispatch(reqPtr: number, reqLen: number, outLenPtr: number): number;
  _cwist_wasm_dispose(ptr: number): void;
}

/**
 * Bind the wrapper to an instantiated Emscripten module compiled from a
 * consumer app with CWIST_WASM_DEFINE_ENTRY exported.
 */
export function createCwist(mod: CwistModule): (init?: CwistRequestInit) => CwistResponse;

/** Low-level helpers, exported for testing. */
export function buildRequestBytes(init: CwistRequestInit): Uint8Array;
export function parseResponseBytes(bytes: Uint8Array): CwistResponse;
