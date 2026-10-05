"""Evaluate the HTTPS gate row produced by https_perf_gates.sh.

Gate policy mirrors runner_baseline.py (which this imports): an absolute
backstop that must hold on any hardware, plus a same-runner-CPU comparison
against the median of earlier runs once at least ``min_samples`` exist on
that CPU. Absolute numbers move a lot between CI CPUs, so the backstops are
wide; the same-CPU check is what catches real regressions early.

Throughput metrics are "min" gates (fail when below backstop / median
divided by the allowance); the RTT metric is a "max" gate.

The absolute throughput numbers swing ~1.8x between the CPUs GitHub hands
out, so their backstops only catch breakage that is wrong on any hardware.
The TLS-specific gates are the https/http *ratios*: both sides are measured
on the same machine in the same run, so the hardware cancels out. Recorded
healthy range (7 CI runs on EPYC 7763 / 9V74 / 9V45, plus Ryzen 5600X):

    ratio              healthy        backstop   pre-#307 tree (Ryzen)
    churn_ratio        0.111-0.127    0.08       0.052
    keepalive_ratio    0.552-0.656    0.45       0.705 (unaffected)
    big_ratio          0.337-0.519    0.25       0.300 (unaffected)

Regression check, Ryzen 5600X, with #307's TCP_QUICKACK re-arm disabled:
tls_rtt_p50_ms 0.083 -> 43.0 and churn_ratio 0.118 -> 0.052, so both the
RTT gate and the churn-ratio gate fail it; https_churn_rps (677/s) also
falls under its backstop, but that one alone would not catch it on a fast
CPU. test_https_gates_eval.py replays both rows.

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
#
# Absolute backstops sit at ~60% of the slowest healthy runner seen (EPYC
# 7763: churn 1155/9459/s, keep-alive 75k/126k/s, 1MiB 1.9/5.0 GB/s
# https/http). The first calibration took them from a Ryzen and an EPYC 9V45
# run, and every run on the 7763 then failed keep-alive and 1MiB on healthy
# code (runs 37204551332, 37293341381), which also meant the history below
# never received a row and the same-CPU check never switched on.
#
# Same-CPU allowances: the four EPYC 7763 runs spread 4-7% on throughput
# and under 3% on the ratios.
GATES = [
    ("tls_rtt_p50_ms", "max", 5.0, 2.0),
    # TLS-path gates, hardware-normalised (see module docstring).
    ("https_churn_ratio", "min", 0.08, 1.20),
    ("https_keepalive_ratio", "min", 0.45, 1.20),
    ("https_big_ratio", "min", 0.25, 1.20),
    # Absolute throughput: catastrophic-only backstops.
    ("https_churn_rps", "min", 700.0, 1.25),
    ("https_keepalive_rps", "min", 45000.0, 1.25),
    ("https_big_gbps", "min", 1.2, 1.30),
    # Plaintext controls: collateral damage to the plain path.
    ("http_churn_rps", "min", 5500.0, 1.25),
    ("http_keepalive_rps", "min", 75000.0, 1.25),
    ("http_big_gbps", "min", 3.0, 1.30),
]

# Ratios derived from each row: (name, numerator, denominator).
RATIOS = [
    ("https_churn_ratio", "https_churn_rps", "http_churn_rps"),
    ("https_keepalive_ratio", "https_keepalive_rps", "http_keepalive_rps"),
    ("https_big_ratio", "https_big_gbps", "http_big_gbps"),
]


def with_ratios(row):
    """Copy of `row` with the https/http ratios filled in from its raw
    numbers (older history rows only carry some of them)."""
    out = dict(row)
    for name, num, den in RATIOS:
        a, b = out.get(num), out.get(den)
        if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b > 0:
            out[name] = round(a / b, 3)
    return out


def evaluate(history, row):
    row = with_ratios(row)
    history = [with_ratios(r) for r in history]
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
                failures.append(f"{metric}: {value:.3f} under the {backstop} "
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
                failures.append(f"{metric}: {value:.3f} on {key} under {limit:.3f} "
                                f"(median {base:.3f} of {len(past)} earlier "
                                f"runs / {allowance})")
                continue
        details.append(f"{metric}: {value:.3f} on {key}, within same-CPU gate")
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
        history.append(with_ratios(row))
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
