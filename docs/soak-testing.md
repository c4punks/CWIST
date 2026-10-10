# Soak testing

Soak testing catches the degradation classes that short benchmarks miss:
memory leaks, file-descriptor growth, and request-latency drift under
sustained load. It exists so regressions of that kind are found **before
the v4.0 cut**, not by users after it. It is one of the v4.0 exit criteria
(ROADMAP.md, "CWIST v4.0 Readiness").

The runnable entry point for this document is `scripts/ci/soak.sh`. The
release-candidate run is that script with the default durations; a quick
smoke run is `DURATION=60 scripts/ci/soak.sh`.

## Environment

- Linux x86_64.
- Release build of the bench server (`make example/simple-server/simple-server`;
  the Makefile's default flags are the release configuration).
- Tools: `wrk` (workload generator), `curl`, `awk`, `/proc` (measurement).
- Run on a quiet machine; nothing else should compete for the cores `wrk`
  and the server use.

## Bench server and workload mix

The bench server is `example/simple-server/simple-server`. It exercises the
high-level app API, dynamic HTML composition, and routing (`/`, `/health`,
`POST /echo`), which covers the allocation paths a long-lived CWIST app
actually runs.

The workload mixes two connection patterns, interleaved per slice:

1. **Keep-alive slice** — `wrk -t4 -c64 --latency` against `/`: sustained
   load on reused connections (the steady-state case).
2. **Churn slice** — `wrk` with `Connection: close` against `/health`:
   constant connection open/close cycling (the leak-detector case; leaks in
   accept/close paths show up here first).

A warmup slice (`min(30s, DURATION/10)`, discarded) runs before any
measurement so startup allocation (TLS context, worker pools) does not
count as growth.

The release-candidate configuration additionally runs with **HTTPS on**
(terminate TLS in front of the same app, or point the script at an HTTPS
base URL via `BASE_HTTPS` so every slice runs once more over TLS) and
touches the **DB-pool and websocket** code paths if a bench app that wires
them is available. Those touchpoints are cheap to add per-app but not part
of the default self-contained run.

Durations: `DURATION=1800` (30 minutes) for the release candidate;
`DURATION=60` for a smoke run. The script's default is 600s (10 minutes).

## Measurement points

All samples are taken over the whole process group (leader plus any
prefork workers), read from `/proc`:

- **RSS** — sum of `VmRSS` across the process group, sampled after the
  warmup and at the end of every slice.
- **Open file descriptors** — count of `/proc/<pid>/fd` entries across the
  process group, sampled with RSS.
- **Request latency** — the 99th-percentile latency of the *first* measured
  keep-alive slice is the baseline; the 99th percentile of the *last*
  keep-alive slice is compared against it.
- **Error rate** — sum of `wrk` socket errors (connect/read/write/timeout)
  and non-2xx responses across all slices. Must be zero.

## PASS/FAIL criteria

The run **FAILS** if any of these hold:

- RSS at the last sample grew more than **10%** over the post-warmup
  baseline.
- The open-fd count at the last sample grew by more than **5 fds** (or 5%,
  whichever is larger) over the post-warmup baseline.
- The final-slice 99th-percentile latency drifted more than **20%** above
  the baseline slice's 99th percentile.
- Any slice reported a non-zero socket-error or non-2xx count, or the
  server process exited before the run completed.

Otherwise the run **PASSES**. All thresholds are env-overridable in
`scripts/ci/soak.sh` (`RSS_GROWTH_PCT`, `FD_GROWTH_ABS`, `LAT_DRIFT_PCT`)
so a justified change to the criteria is a reviewed code change, not a
quiet local tweak.

## Interpreting a failure

- RSS climbing with flat fds: heap growth — run `bench_full_gc_tracking` or
  ASAN/LSAN on the same workload.
- Fd climbing with churn slices: connection leak in accept/close — compare
  with the connection-registry and close-drain tests.
- Latency drift with flat RSS/fds: queueing or timer degradation — see the
  tail-latency work in ROADMAP issue #293.
