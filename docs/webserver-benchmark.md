# Web Server Benchmark Methodology

The `web-server-benchmark` job in `.github/workflows/bsd-kqueue-benchmarks.yml`
builds the checked-out source and runs minimal `GET / -> Hello, World!` servers.
This is CWIST's own GitHub Actions comparison, not the separate
`the-benchmarker/web-frameworks` suite. A release build passing its correctness
suite does not establish a latency SLO.

## Measurement contract v2

New results use `schema_version: 2` and
`benchmark_contract: isolated-http1-wrk-corrected-v2`.
Each measured server runs in a private Linux process group owned by
`benchmark_session.py`. Its workload uses a separate private group. Leaders
remain unreaped until group cleanup finishes, preventing PGID reuse while
signals are sent. The next case refuses an already-listening port. Cleanup
covers descendants, including early-exiting leaders, failure, timeout and
cancellation. This is supervision of trusted benchmark commands, not a sandbox
against a program deliberately escaping its session.

Cases run sequentially. Readiness must return HTTP 200 within the recorded
startup budget; warmup must exit successfully, complete requests and report no
request errors. A failed warmup or measurement stops the job. Partial raw logs
remain artifacts but cannot be published as successful measurements.

- Main profile: `wrk -t12 -c400 -d10s`, after a discarded 10-second warmup.
- Separate tuned profile: `wrk -t4 -c100 -d10s`, also after a discarded
  10-second warmup. CWIST classic and Spring each get a fresh server process.
- The main cases are CWIST classic, C1M, C1M with `arena_max=1`, C1M with
  `drain_chunk=8`, explicit C1M `PUBLIC_FIXED`, Axum, Gin and Spring.
- The PUBLIC_FIXED leg sets only `CWIST_BENCH_PUBLIC_FIXED=1` in the benchmark
  fixture to select explicit public-cache registration. Other legs retain bare
  FIXED registration. This is a configuration screen, not proof of a universal
  cache speedup. The plain handler does not exercise asynchronous completion
  posting, so the drain-chunk leg cannot demonstrate that mechanism's benefit.
- Clients and servers share the visible runner CPU budget. Runtime worker
  sizing differs: CWIST has its own worker affinity, Tokio/Go use their runtime
  defaults, and Netty's worker count is explicitly set to the visible CPU count.
  The runner model and CPU count are recorded; there is no isolated-CPU claim.

## Admission and latency semantics

`tail_latency.lua` emits one structured `CWIST_METRICS` record containing
requests, duration, all five wrk error counters, min/mean/max and seven
percentiles. `webserver_result.py` rejects absent, duplicate or malformed
records, empty measurements, invalid/nonfinite numbers, request errors,
missing percentiles, inconsistent ordering and unproven cleanup. It never
turns missing data into a zero-latency success. The complete ten-case matrix is
required before publication.

These latency values are **wrk's corrected latency distribution**. P99.999 is
a direct `latency:percentile(99.999)` call, not an average of other percentiles.
They are not an uncorrected per-request histogram or wrk2's fixed-rate
planned-arrival histogram. Request count is retained; the approximate number
of original requests in the top 0.001% is only a resolution aid, not a
confidence interval or the corrected histogram's sample count.

The existing mean-latency gates remain 3.0 ms for classic and 3.5 ms for C1M.
Finite/ordered tail values and request/error counts are validated, but there
is no universal P99.999 SLO gate. A short, shared-runner screen cannot supply
that guarantee. Raw and corrected histogram instrumentation is separately
available in [the opt-in tool](performance/wrk-dual-histogram.md); its mere
presence does not mean every default CI run captured both histograms.

## Resource measurements

Snapshots bracket the measured interval, after warmup. They include only the
supervised server process group, with PID/start-time identities and per-thread
identity, affinity and context-switch counters from Linux `/proc`.

- **Group RSS end sample:** sum of available process RSS readings at the end.
  This is not a peak, PSS or unique physical memory; shared pages can be counted
  more than once. If a required reading is missing, the value is unavailable.
- **Context-switch delta:** sum of voluntary plus involuntary deltas for an
  identical set of PID/start-time and TID/start-time identities. Task churn,
  missing readings or counter regression produces **N/A**, never a fabricated
  zero. This excludes the load-generator group.
- Process identity changes during a case stop acceptance. The cleanup receipt
  binds the workload's server PGID and confirms no live group survivors before
  the leader is reaped.


### Interpreting the context-switch column

CWIST classic (`CWIST_C1M_MODE=0`) is the only blocking thread-per-connection server in
the matrix; every other row multiplexes connections onto a small thread pool. That
difference shows up almost entirely in this column, and it is a property of the
concurrency model, not of wasted work:

- Classic pays one voluntary switch per request — a worker blocked in `recv` wakes when
  its connection's request arrives. That is the floor for the blocking model; nothing in
  the serve path adds a second one (response sends use `MSG_DONTWAIT` with an inline
  poll fallback that fires only when the client is genuinely slow).
- CWIST C1M and the async runtimes batch many requests per wake, so their per-request
  switch count is much lower.

This was attributed by re-running the workload under a throttled CPU quota and splitting
per-thread voluntary/non-voluntary counters: nearly all classic switches are voluntary
recv wakeups (the model floor), and pool scaling reaches one worker per connection
without spawn failures. Low-context-switch operation is what C1M — the default
profile — is for; classic trades that for its latency behavior at low concurrency (the
tuned `wrk -t4 -c100` run).

## Runtime configuration

CWIST uses the checked-out source and generated constant-body fixture.
Axum uses `axum = 0.7` and Tokio; Gin uses v1.10.0 and Go 1.22.
Spring uses Spring Boot 3.2.3 WebFlux/Reactor Netty on JDK 25, native epoll,
virtual threads **disabled**, and a fixed **1024 MiB** initial/maximum heap.
Its recorded JVM arguments include G1GC and the actual runtime tuning flags.

Spring's separate preparation boot generates a Leyden AOT cache with
`-XX:+AOTClassLinking -XX:AOTCacheOutput=...`; measured boots use
`-XX:+AOTClassLinking -XX:AOTCache=...`. This is not the former JDK 21 CDS
`ArchiveClassesAtExit`/`SharedArchiveFile` configuration. Do not infer that all
JIT warmup has disappeared. Both Spring profiles use the same prepared cache.

## Provenance, presentation and legacy results

Every new result records the measured commit, GitHub run ID/attempt/link,
ref, tag when running a tag, server artifact and wrk SHA-256 values, wrk
version, profiles, runtime facts, per-case request/error counts, snapshots and
cleanup receipts. A moving branch result is identified by its commit; it is
not silently labeled as the latest release.

Raw measurements, warmup logs, resource JSON and cleanup receipts are retained
as artifacts, including partial failures. Successful schema-v2 records alone
enter the new report contract. Old rows remain historical evidence; their
missing provenance, zero-default parsing and incomplete process cleanup are
not retroactively repaired. Different contracts are not pooled into a trend.

The README values are generated from JSON rather than duplicated in fixed
marketing prose. Resource N/A stays N/A in tables and SVGs. The latency density
SVG is reconstructed by interpolating reported percentiles and applying KDE;
it is **not a measured request histogram**. It must not be used to invent
sub-percentile detail, confidence intervals or a causal explanation of jitter.
