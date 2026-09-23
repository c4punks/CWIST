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

export interface CwistHandle {
  (init?: CwistRequestInit): CwistResponse;
  /**
   * Pin the session signing secret (host-injected contract; issue #93).
   * Pass null to let CWIST generate a per-instance random secret (dev
   * mode). Must be called before the first session-bearing dispatch.
   */
  useSession(secret: string | null): void;
}

/** Minimal shape of the guest interface of a jco-transpiled cwist-guest
 * component (docs/api/wasm-component.md, issue #203). */
export interface CwistComponent {
  dispatch(request: Uint8Array): Uint8Array;
  useSession(secret: string | null): void;
  /** Optional declarative session secret, applied once at binding time. */
  cwistSessionSecret?: string;
}

/**
 * Bind the wrapper to a jco-transpiled cwist-guest component. Returns the
 * same handle shape as createCwist() from the package root; throws on the
 * dispatch-error variant.
 */
export function createCwistFromComponent(component: CwistComponent): CwistHandle;
