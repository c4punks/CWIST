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
  /** Optional (issue #93): session secret setter from CWIST_WASM_DEFINE_ENTRY. */
  _cwist_wasm_use_session?(secretPtr: number): number;
  /**
   * Optional declarative session secret: a string here is applied once at
   * binding time, as if handle.useSession() had been called.
   */
  cwistSessionSecret?: string;
}

export interface CwistHandle {
  (init?: CwistRequestInit): CwistResponse;
  /**
   * Pin the session signing secret (host-injected contract; issue #93).
   * Pass null to let CWIST generate a per-instance random secret (dev
   * mode). Must be called before the first session-bearing dispatch.
   */
  useSession(secret: string | null): void;
}

/**
 * Bind the wrapper to an instantiated Emscripten module compiled from a
 * consumer app with CWIST_WASM_DEFINE_ENTRY exported.
 */
export function createCwist(mod: CwistModule): CwistHandle;

/** Low-level helpers, exported for testing. */
export function buildRequestBytes(init: CwistRequestInit): Uint8Array;
export function parseResponseBytes(bytes: Uint8Array): CwistResponse;
