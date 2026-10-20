# FIXED endpoint caching: current behavior

**Status:** settled. `CWIST_ENDPOINT_FIXED` is a non-caching hint, and the
docs now say so. The opt-in `CWIST_ENDPOINT_PUBLIC_FIXED` cache proposed in
ADR-0001 shipped on 2026-09-14 and was removed on 2026-09-21 after the BDR
cursor fast path (#194) replaced it; the bit is still defined but inert.
**Related:** [issue #25](https://github.com/c4punks/CWIST/issues/25) (closed),
[ADR-0001](decisions/0001-safe-public-fixed-response-cache.md) (superseded).

## What the runtime actually does today

`CWIST_ENDPOINT_FIXED` **does not activate an automatic response cache in
the current revision**. In both classic and C1M modes, every request to a
FIXED route is dispatched to the handler through ordinary routing.
`endpoint_opts.h` and `docs/api/app.md` describe this behavior; older
"cache the response in RAM after the first hit" wording described an
intended contract that the runtime never delivered.

## Evidence

Public TCP observation (see issue #25): 16 FIXED requests invoked the
handler 16 times — eight keep-alive requests and eight fresh connections —
with a DYNAMIC control and independently preseeded cache responses, and
adaptive latency-based learning suppressed. This establishes that the
cache is not populated/used at all (a population failure), independent of
its contribution to tail latency.

## Why the learning block is not simply moved

Two ordering defects explain the population failure: the common serving
function samples endpoint options before routing assigns them, and the
C1M branch returns before the learning block. Moving that block alone is
unsafe:

- existing cache hits precede routing and middleware, including
  authorization;
- parsed cache keys omit query and other representation distinctions,
  and the classic raw shortcut has different key/framing behavior;
- whole-wire blobs preserve the first response's connection headers;
  some hits use a single unchecked `send`, and a classic lookup returns
  an already-unpinned pointer;
- normal sending may release managed pointer bodies before post-send
  learning reads them;
- the stringification helper treated a non-NUL-terminated header span as
  a C string (fixed independently by PR #84, which does not make cached
  replay safe).

## Recommended handling

- Treat FIXED routes as ordinary dispatch. Do not rely on automatic
  cache hits; do not preseed whole-wire BDR blobs expecting HTTP replay.
- Setting `CWIST_ENDPOINT_PUBLIC_FIXED` has no runtime effect. Any future
  cache path should start from the eligibility, representation and
  ownership requirements recorded in ADR-0001 rather than from raw replay.
- A correct response is mandatory even when a cache miss costs
  performance; no performance objective should reactivate the unsafe raw
  replay path.
