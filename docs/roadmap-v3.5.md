# v3.5.0 release roadmap

Status: candidate work in PR #91. Not released.

**Release hold:** a separate CVE-fix PR must be included in v3.5.0 and its
associated issue resolved before publication. This is an additional required
gate even if PR #91 and its CI pass. The issue/PR identifiers are pending
confirmation; do not treat the missing identifiers as permission to release.

The release target is v3.5.0, not v3.4.2. This release adds an explicit public
FIXED cache and changes the public API. It also includes HTTP correctness fixes
and benchmark controls. See the [maintainer proposal](https://github.com/c4punks/CWIST/pull/91#issuecomment-5660861288)
and [release decision](https://github.com/c4punks/CWIST/pull/91#issuecomment-5660948894).

## Scope

1. Public FIXED cache: implement ADR-0001 with explicit opt-in, strict admission,
   owned response data, process-wide limits, and cache invalidation. Requests
   that do not qualify must use ordinary dispatch. Keep the BDR storage API.
   See [cache contract and migration](fixed-cache-status.md).
2. HTTP correctness: fix blocking pipeline framing and transfer deferred
   request/response ownership before another thread can use the objects.
   Include middleware, producer response, and inline abort paths.
   See [async ownership](async-gc-ownership.md).
3. Benchmark controls: use isolated process groups, reject failed or incomplete
   runs, and bind each result to its source and workload. Keep PUBLIC_FIXED as
   a separate case. Retain the dual-histogram tools from PR #87.
   See [benchmark contract](webserver-benchmark.md).
4. Packaging: build and test the source archive as version 3.5.0. Publish the
   source commit, tree, submodule identities, and archive checksums.

PRs #85 and #87 are merged into dev. Their integration into main is part of
PR #91. Their merge is not evidence that this release has passed its checks.

## Release gates

- [ ] Close the real TCP regressions on the final candidate. Run classic and
  C1M modes with GC enabled and disabled, including NDEBUG and sanitizer builds.
- [ ] Verify inline abort from a keep-alive request: terminal response, forced
  connection close, and middleware unwind. Verify creator and producer exit
  schedules without use-after-free or duplicate cleanup.
- [ ] Resolve the separate TLS-GC ownership risk in lazy timeout scheduler and
  job/IO-queue allocations. An exchange-graph fix alone does not close this risk.
- [ ] Complete independent source/security review and all applicable CI checks
  on the exact candidate. Do not replace failed runs with source-only claims.
- [ ] Pass source archive build/tests and native platform checks. Rebuild and
  relink applications against the matching headers and library: the app
  structure grows, so this is not a binary-only drop-in update.
- [ ] Verify the main merge commit and its checks before publishing v3.5.0.
  Keep the existing v3.4 and v3.4.1 tags unchanged.
- [ ] Download the published assets and verify their identities and checksums.
- [ ] Remeasure the exact released version. Keep the workload arguments and
  admission rules unchanged. Publish raw results, request/error counts,
  percentile definitions, source identity, and cleanup evidence.

## Evidence and limits

The earlier ASan failure is a real failure, not a passing result for a later
candidate. Cache unit results and static review do not prove full TCP safety.
Failed warmup attempts remain invalid benchmark evidence. Earlier performance
results must keep their original source/version labels.

The version change does not close issue #25 or prove a P99.999 improvement.
Report correctness, benchmark validity, measured latency, and release status
separately. Keep issue #25 open until its acceptance evidence is established.

Do not publish a v3.4.2 backport as part of this roadmap. A separate urgent
patch can be scoped if needed. If a release gate fails, hold v3.5.0 and retain
the last verified release; do not move an existing tag or weaken a test.
