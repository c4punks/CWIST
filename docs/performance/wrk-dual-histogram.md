# Opt-in wrk raw/corrected latency histograms

This diagnostic tool addresses measurement ambiguity in issue #25. It does not change CWIST, normal CI workload flags, or runtime defaults. CI's existing Python test discovery exercises its offline contract tests; benchmarks use it only when an operator explicitly builds the generated wrk source.

wrk 4.2.0 calls `stats_correct` before its normal reporting and Lua `done` callback. Consequently Lua `latency:percentile(99.999)` is not the unmodified observed histogram. This exporter emits both distributions from the same run, after all worker joins and after the measured duration has been frozen, bracketing the existing correction call. No request-loop or timing changes are introduced.

## Build in a disposable wrk checkout

Use wg/wrk tag `4.2.0`. The generator accepts the exact upstream `wrk.c`, `stats.c`, and `stats.h`, or the explicitly fingerprinted CLOCK_MONOTONIC timing variant used in the #25 experiments. It rejects other contents. That alternate variant changes only `time_us` to CLOCK_MONOTONIC and adds its time header; the exporter does not itself change clocks.

```sh
python3 scripts/ci/instrument_wrk_histogram.py \
  --source-dir /path/to/disposable-wrk/src \
  --output-dir /path/to/new-output
# Inspect new-output/manifest.json and retain it with the run evidence.
cp /path/to/new-output/wrk.c /path/to/disposable-wrk/src/
cp /path/to/new-output/wrk_dual_histogram.h /path/to/disposable-wrk/src/
make -C /path/to/disposable-wrk
```

The input source directory is read-only to the generator. The output directory must not exist, its parent must exist, and generation is not resumable or transactional: an I/O failure can leave a partial new output directory. Inspect that directory after failure; do not use partially generated files. Use private operator-owned directories. The generated header is only for wrk's build and is not a CWIST public header. Re-generation against already patched wrk is deliberately rejected.

## Output and interpretation

Retain stdout/stderr, exits, workload errors, topology, source/build manifest and binary hash. In addition to unchanged standard output, there are two JSON lines prefixed `CWIST_HISTOGRAM `:

```text
CWIST_HISTOGRAM {"kind":"raw","count":2,"unit":"us","bins":[[40,2]]}
CWIST_HISTOGRAM {"kind":"corrected","count":6,"unit":"us","bins":[[20,2],[30,2],[40,2]]}
```

These illustrative bins are sparse `[latency_us, sample_count]` pairs. Always require each kind exactly once, strictly increasing integer bucket positions, positive counts, and sum(bins)==count. The exporter scans below the old `min` because correction can populate lower buckets without updating that field. Empty distributions have count0 and bins[]; percentiles are unavailable, not zero.

The raw distribution is wrk's **recorded** latency histogram, not proof that all requests/arrivals were observed. Preserve request totals and error/timeout counts separately and account for any count gap, pipeline batching, out-of-range samples and in-flight requests. Neither histogram repairs coordinated omission in the offered workload or establishes an open-loop SLO. Document the percentile rank convention when reducing bins; do not silently equate a nearest-rank statistic with wrk's own rounding algorithm. Never infer raw quantiles from old corrected summary scalars.

Printing happens after timing and worker joins but still costs CPU, I/O, output volume and wall-clock time. Isolate runs through process cleanup and allow enough supervisor timeout for final output. Do not interleave another load while export/cleanup is active. Compare raw and corrected results from the same run; a prior uninstrumented run is not a paired performance control.

## Verification

```sh
python3 scripts/ci/test_wrk_dual_histogram.py
python3 -O scripts/ci/test_wrk_dual_histogram.py
```

Tests compile the exporter with explicit sparse/empty/below-original-minimum distributions, check placement and preservation of the correction block, reject repeated/missing hooks and already instrumented input, and reject unknown source without writes. A C compiler and Python3 are required. Real wrk compilation and public-listener integration are separate gates, not implied by these offline tests.
