// Type definitions for the cwist-wasm component adapter (component.js).

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

/**
 * Receives one serialized chunk of a streaming response: the head (status
 * line + headers) first, then body slices. Return a Promise to apply
 * backpressure; a throw or rejection aborts the dispatch.
 */
export type CwistChunkCallback = (chunk: Uint8Array) => void | Promise<void>;

export interface CwistHandle {
  /**
   * Synchronous against a 0.2 (wasip2) guest; returns a Promise against a
   * 0.3 (wasip3) guest, whose exports jco lowers to async functions.
   */
  (init?: CwistRequestInit): CwistResponse | Promise<CwistResponse>;
  /**
   * Pin the session signing secret (host-injected contract; issue #93).
   * Pass null to let CWIST generate a per-instance random secret (dev
   * mode). Must be called before the first session-bearing dispatch.
   */
  useSession(secret: string | null): void;
  /**
   * Streaming dispatch through the cwist-guest-stream world (0.3 only):
   * resolves once the last chunk has been delivered to onChunk, rejects
   * with the dispatch-error or with the callback's own error. Needs
   * dispatchStream and streamHost on the bound component.
   */
  stream(init: CwistRequestInit | undefined, onChunk: CwistChunkCallback): Promise<void>;
}

/** The c4punks:cwist/host import of the cwist-guest-stream world. */
export interface CwistStreamHost {
  sendChunk(chunk: Uint8Array): Promise<void>;
}

/** Minimal shape of the guest interface of a jco-transpiled cwist-guest
 * component (docs/api/wasm-component.md, issue #203). */
export interface CwistComponent {
  dispatch(request: Uint8Array): Uint8Array | Promise<Uint8Array>;
  useSession(secret: string | null): void | Promise<void>;
  /** Optional declarative session secret, applied once at binding time. */
  cwistSessionSecret?: string;
  /** guest-stream's dispatch-stream; only cwist-guest-stream guests have it. */
  dispatchStream?(request: Uint8Array): Promise<void>;
  /** The createStreamHost() object wired as the component's host import. */
  streamHost?: CwistStreamHost;
}

/**
 * Bind the wrapper to a jco-transpiled cwist-guest component. Returns the
 * same handle shape as createCwist() from the package root; throws on the
 * dispatch-error variant.
 */
export function createCwistFromComponent(component: CwistComponent): CwistHandle;

/**
 * Create the c4punks:cwist/host import for a cwist-guest-stream component;
 * pass the same object as `streamHost` when binding the component.
 */
export function createStreamHost(): CwistStreamHost;
