"""Evaluate the HTTPS gate row produced by https_perf_gates.sh.

Gate policy mirrors runner_baseline.py (which this imports): an absolute
backstop that must hold on any hardware, plus a same-runner-CPU comparison
against the median of earlier runs once at least ``min_samples`` exist on
that CPU. Absolute numbers move a lot between CI CPUs, so the backstops are
wide; the same-CPU check is what catches real regressions early.

Throughput metrics are "min" gates (fail when below backstop / median
divided by the allowance); the RTT metric is a "max" gate. The https/http
keep-alive ratio is recorded as a diagnostic signal, not gated.

Usage:
    https_gates_eval.py RESULT.json [HISTORY.json]   # evaluate, exit 1 on fail
    https_gates_eval.py --update RESULT.json [HISTORY.json]
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runner_baseline import runner_key, same_runner_values

HISTORY = Path(__file__).resolve().parent.parent.parent / "benchmarks" / "https_gates.json"

# (metric, direction, absolute backstop, same-CPU allowance)
# Backstops are calibrated so a healthy tree passes on the noisiest shared
# CI runner we have seen, with real margin for run-to-run variance:
#   local Ryzen 5600X (12-core): churn ~1650/s, keep-alive ~151-190k/s,
#     1MiB ~4.1GB/s (https) / ~12-14GB/s (http), RTT p50 ~0.09ms.
#   shared EPYC 9V45 CI runner (run 37201724744): churn 2013/s https /
#     15793/s http, keep-alive 135570/s https / 206650/s http,
#     1MiB 4.03GB/s https / 7.8GB/s http.
# The pre-#307 tree measured ~600/s churn, ~43ms RTT p50, so these
# backstops still fail it on any hardware. The same-CPU relative check
# (runner_baseline.py convention) is what catches smaller regressions once
# enough history accumulates for a runner CPU.
GATES = [
    ("tls_rtt_p50_ms", "max", 5.0, 2.0),
    ("https_churn_rps", "min", 1000.0, 1.25),
    ("https_keepalive_rps", "min", 110000.0, 1.25),
    ("https_big_gbps", "min", 2.5, 1.30),
    # Plaintext controls: collateral damage to the plain path fails the gate.
    ("http_churn_rps", "min", 6000.0, 1.25),
    ("http_keepalive_rps", "min", 150000.0, 1.25),
    ("http_big_gbps", "min", 5.5, 1.30),
]


def evaluate(history, row):
    failures = []
    details = []
    for metric, direction, backstop, allowance in GATES:
        value = row.get(metric)
        if not isinstance(value, (int, float)) or value <= 0:
            failures.append(f"{metric}: missing or non-positive in result row")
            continue

        if direction == "max":
            if value > backstop:
                failures.append(f"{metric}: {value:.3f} over the {backstop}ms "
                                f"absolute backstop")
                continue
        else:
            if value < backstop:
                failures.append(f"{metric}: {value:.1f} under the {backstop} "
                                f"absolute backstop")
                continue

        key = runner_key(row)
        if key is None:
            details.append(f"{metric}: {value:.3f}, runner not recorded, "
                           f"backstop only")
            continue
        past = same_runner_values(history, key, metric)
        if len(past) < 5:
            details.append(f"{metric}: {value:.3f} on {key}, only "
                           f"{len(past)} earlier run(s), backstop only")
            continue
        from statistics import median
        base = median(past)
        if direction == "max":
            limit = base * allowance
            if value > limit:
                failures.append(f"{metric}: {value:.3f} on {key} over {limit:.3f} "
                                f"({allowance}x the {base:.3f} median of "
                                f"{len(past)} earlier runs)")
                continue
        else:
            limit = base / allowance
            if value < limit:
                failures.append(f"{metric}: {value:.1f} on {key} under {limit:.1f} "
                                f"(median {base:.1f} of {len(past)} earlier "
                                f"runs / {allowance})")
                continue
        details.append(f"{metric}: {value:.3f} on {key}, within same-CPU gate")

    ratio = row.get("https_keepalive_ratio")
    if isinstance(ratio, (int, float)):
        details.append(f"https_keepalive_ratio: {ratio:.3f} (signal only, "
                       f"~0.65 expected; large drops hint at TLS-path damage "
                       f"even when both absolute gates pass)")
    return failures, details


def main():
    args = sys.argv[1:]
    update = args and args[0] == "--update"
    if update:
        args = args[1:]
    result_path = Path(args[0]) if args else Path("https-gates-result.json")
    history_path = Path(args[1]) if len(args) > 1 else HISTORY

    row = json.loads(result_path.read_text())
    try:
        history = json.loads(history_path.read_text())
    except (OSError, ValueError):
        history = []

    if update:
        history = [r for r in history
                   if r.get("timestamp") != row.get("timestamp")]
        history.append(row)
        history_path.write_text(json.dumps(history[-100:], indent=2) + "\n")
        print(f"updated {history_path} ({len(history)} rows)")

    failures, details = evaluate(history, row)
    for line in details:
        print(f"ok: {line}")
    for line in failures:
        print(f"FAIL: {line}")
    print(f"https gates: {len(GATES) - len(failures)}/{len(GATES)} passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
