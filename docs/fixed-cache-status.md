# FIXED endpoint caching: current behavior

**Status:** known documentation/runtime mismatch — active investigation.
**Related:** [issue #25](https://github.com/c4punks/CWIST/issues/25),
ADR-0001 (draft PR #85, `CWIST_ENDPOINT_PUBLIC_FIXED`).

## What the runtime actually does today

`CWIST_ENDPOINT_FIXED` **does not activate an automatic response cache in
the current revision**. In both classic and C1M modes, every request to a
FIXED route is dispatched to the handler through ordinary routing. The
"cache the response in RAM and reply instantly after the first hit"
wording in `endpoint_opts.h` and `docs/api/app.md` describes the intended
contract, not the runtime behavior.

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

## Recommended handling until the contract is restored

- Treat FIXED routes as ordinary dispatch. Do not rely on automatic
  cache hits; do not preseed whole-wire BDR blobs expecting HTTP replay.
- The safe restoration path is ADR-0001 (draft PR #85): a new explicit
  `CWIST_ENDPOINT_PUBLIC_FIXED` opt-in with a parsed eligibility
  boundary, representation (not wire-blob) caching, and ownership
  isolation, plus an explicit compatibility change. Review that ADR
  before implementing or enabling any cache path.
- A correct response is mandatory even when a cache miss costs
  performance; no performance objective should reactivate the unsafe raw
  replay path.
