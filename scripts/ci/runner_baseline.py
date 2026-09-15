"""Compare a benchmark run against earlier runs on the same runner CPU.

The web-server benchmark lands on whatever CPU GitHub hands out, and the
model changes run to run. That moves the numbers far more than most code
changes do: across the recorded history the same CWIST build measures a
C1M average latency of about 3.0ms on an EPYC 7763 and about 1.9ms on a
Xeon 6973P-C, and its latency relative to Axum flips from 0.68 to 1.13
depending purely on which CPU the run landed on.

So a single absolute threshold is a lottery. At a 3.5ms gate, a run on the
slow CPU sits at 87% of the limit while the same code on the fast CPU sits
at 54%: a real regression can pass on one and noise can fail on the other.
Comparing a run only against earlier runs on the same CPU model removes
that variable.

The absolute ceiling is kept as a backstop, for a CPU with no history yet
and for a regression large enough to be wrong on any hardware.

Picking the allowance
---------------------
Replaying the recorded history, each run against the median of the earlier
runs on its own CPU, gives the run-to-run noise these gates have to sit
above:

    metric             p50     p90     p95     p99
    cwist_lat_ms      1.01x   1.06x   1.12x   1.17x
    cwist_c1m_lat_ms  1.15x   1.33x   1.48x   1.52x

The classic pool is steady; the C1M reactor is not, which matches its own
design note about batching making it sensitive to scheduling jitter. One
shared allowance would therefore either fail constantly on C1M or never
fire on classic, so callers pass one per metric.

Be honest about what this buys: with C1M noise reaching 1.5x, a gate that
does not cry wolf cannot catch a small regression from a single run. It
catches large ones. Spotting a 10% drift in C1M needs several runs on one
CPU compared together, not this check.
"""

from statistics import median


def runner_key(row):
    """CPU model for a history row, or None when it was not recorded.

    runner_hw looks like "4 vCPU | AMD EPYC 7763 64-Core Processor"; the
    core count before the bar is a property of the runner size, which is
    fixed for this workflow, so the model after it is the whole key.
    """
    hw = row.get('runner_hw')
    if not isinstance(hw, str) or not hw.strip():
        return None
    return hw.split('|')[-1].strip() or None


def same_runner_values(history, key, metric):
    """Values of `metric` from history rows recorded on runner `key`."""
    out = []
    for row in history:
        if runner_key(row) != key:
            continue
        value = row.get(metric)
        if isinstance(value, (int, float)) and value > 0:
            out.append(float(value))
    return out


def evaluate(history, row, metric, ceiling_ms, min_samples=5, allowance=1.25):
    """Judge `row`'s `metric` against same-CPU history.

    Returns (ok, detail). `detail` always names which rule decided, so a
    failing CI log says whether it tripped the same-CPU comparison or the
    absolute ceiling, and what it was compared against.
    """
    value = row.get(metric)
    if not isinstance(value, (int, float)) or value <= 0:
        return True, f'{metric}: not recorded, skipped'

    value = float(value)
    if value > ceiling_ms:
        return False, (f'{metric}: {value:.2f}ms over the {ceiling_ms:.2f}ms '
                       f'absolute ceiling')

    key = runner_key(row)
    if key is None:
        return True, f'{metric}: {value:.2f}ms, runner not recorded, ceiling only'

    past = same_runner_values(history, key, metric)
    if len(past) < min_samples:
        return True, (f'{metric}: {value:.2f}ms on {key}, only {len(past)} earlier '
                      f'run(s) on this CPU, ceiling only')

    base = median(past)
    limit = base * allowance
    if value > limit:
        return False, (f'{metric}: {value:.2f}ms on {key} is over {limit:.2f}ms '
                       f'({allowance:.2f}x the {base:.2f}ms median of {len(past)} '
                       f'earlier runs on this CPU)')
    return True, (f'{metric}: {value:.2f}ms on {key}, within {limit:.2f}ms '
                  f'({allowance:.2f}x the {base:.2f}ms median of {len(past)} '
                  f'earlier runs on this CPU)')


def summarize(history, metrics):
    """Per-CPU medians, so a report can show what each CPU actually yields.

    Returns [(runner, sample count, {metric: median})], most-sampled first.
    """
    keys = {}
    for row in history:
        key = runner_key(row)
        if key is not None:
            keys.setdefault(key, []).append(row)

    out = []
    for key, rows in keys.items():
        stats = {}
        for metric in metrics:
            values = [float(r[metric]) for r in rows
                      if isinstance(r.get(metric), (int, float)) and r[metric] > 0]
            if values:
                stats[metric] = median(values)
        if stats:
            out.append((key, len(rows), stats))
    out.sort(key=lambda item: (-item[1], item[0]))
    return out
