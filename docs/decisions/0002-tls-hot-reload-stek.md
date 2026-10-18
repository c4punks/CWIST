# ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged

- **Status:** Implemented.
- **Date:** 2026-10-18
- **Scope:** HTTPS (TLS over TCP) and HTTPS/3 (QUIC) listeners reloaded through
  `cwist_app_reload_tls()`, `cwist_https_reload_context()` and
  `cwist_http3_reload_context()`.
- **Related:** [issue #404](https://github.com/c4punks/CWIST/issues/404),
  the process-lifetime STEK discussion in `src/net/http/https.c`
  ("Shared session ticket key", tracked as issue #98).

## Context

Reloading a certificate in place means the listener swaps its `SSL_CTX`
while old connections finish on the previous one. Both TLS stacks in CWIST
install an explicit session-ticket key callback so that tickets issued by
one pre-fork worker resume on any other; the key material is random,
generated at context creation, and carried in an `SSL_CTX` ex-data slot.

A reload builds a brand-new `SSL_CTX`. The question is whether the new
context reuses the existing STEK or mints a fresh one.

## Decision

**A certificate reload does not rotate the STEK.** The new `SSL_CTX` is
installed with the same ticket key the listener already used. The key is
owned by the wrapper context (`cwist_https_context.ticket_key` /
`cwist_http3_context.ticket_key`), which outlives every `SSL_CTX` it was
ever installed on, and is freed once at wrapper destruction.

Rationale:

- Ticket resumption across a reload keeps working. With a fresh STEK, every
  client holding a ticket would fall back to a full handshake on its next
  connection — a needless latency cliff exactly when an operator is rotating
  certificates on a live fleet.
- TLS 1.3 resumption (PSK mode) does not re-present the server certificate,
  so a resumed session after a reload cryptographically continues the
  identity of the pre-reload connection. That is the standard nginx/HAProxy
  behaviour for ticket keys that outlive a certificate swap, and it is safe:
  the session keys were established under a certificate the operator
  considered valid at the time.
- The STEK only protects session tickets, not bulk traffic. Traffic keys
  remain protected by (EC)DHE forward secrecy in both directions of the
  reload boundary.

Explicitly **out of scope**: STEK rotation on a timer. The existing
process-lifetime STEK trade-off (issue #98) is unchanged; an operator whose
threat model requires STEK rotation still restarts the process, which is
also the only way to rotate the key across the pre-fork worker set.

## Consequences

- A compromised STEK can decrypt tickets issued both before and after any
  number of reloads, for the process lifetime. This is identical to the
  pre-reload exposure; hot reload does not widen the window.
- The alternative (fresh STEK per reload) was rejected: it buys a smaller
  ticket-decryption window only when reloads actually happen, and it breaks
  resumption for all clients on every reload.
- `SSL_CTX` ex-data has no destructor in this codebase, so the shared key
  must be wrapper-owned; per-`SSL_CTX` key copies would leak from retired
  contexts. This is why the HTTP/3 ticket key moved from per-`SSL_CTX`
  ownership to the `cwist_http3_context` wrapper as part of the change.
