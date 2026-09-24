#!/usr/bin/env python3
"""Run reproducible CWIST microbenchmarks and render tracked SVG trends."""
from __future__ import annotations
import json, math, platform, re, resource, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path

# Import the sibling module by location, not by cwd: this script is run as
# scripts/ci/benchmark.py from the repository root in CI and from a copied
# tree in the tests.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from runner_baseline import summarize

ROOT = Path(__file__).resolve().parents[2]
HISTORY = ROOT / "benchmarks" / "db.json"
SVG = ROOT / "docs" / "benchmark-trends.svg"
README = ROOT / "README"
ROADMAP = ROOT / "ROADMAP.md"

def child_usage():
    u = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (u.ru_utime, u.ru_stime, u.ru_maxrss, u.ru_nvcsw, u.ru_nivcsw)

def run_measurement() -> dict:
    subprocess.run(["make", "bench_security_pool"], cwd=ROOT, check=True, stdout=subprocess.PIPE, text=True)
    samples, output = [], ""
    for _ in range(3):
        before = child_usage(); started = time.monotonic()
        completed = subprocess.run(["./bench_security_pool"], cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        elapsed = time.monotonic() - started; after = child_usage(); output = completed.stdout
        max_rss = max(0, after[2] - before[2]) or after[2]
        if sys.platform == "darwin": max_rss //= 1024
        samples.append({"elapsed": elapsed, "cpu": (after[0]-before[0])+(after[1]-before[1]), "rss_kib": max_rss,
                        "context_switches": (after[3]-before[3])+(after[4]-before[4])})
    waf = re.search(r"waf: ([0-9.]+) M checks/s", output); pool = re.search(r"pool: ([0-9.]+) M acquire-release/s", output)
    median = sorted(samples, key=lambda x: x["elapsed"])[1]
    return {"timestamp": datetime.now(timezone.utc).isoformat(), "os": platform.system(), "arch": platform.machine(),
            "cpu_percent": round(100 * median["cpu"] / max(median["elapsed"], .001), 2), "rss_kib": median["rss_kib"],
            "context_switches": median["context_switches"], "memory_recovery_kib": samples[-1]["rss_kib"] - samples[0]["rss_kib"],
            "waf_mchecks_s": float(waf.group(1)) if waf else 0, "throughput_mleases_s": float(pool.group(1)) if pool else 0}

def svg(history: list[dict]) -> str:
    rows = history[-20:]; metrics = [("CPU utilization (%)", "cpu_percent", "#ef4444"), ("Throughput (M leases/s)", "throughput_mleases_s", "#22c55e"), ("RSS (KiB)", "rss_kib", "#3b82f6"), ("Memory recovery drift (KiB)", "memory_recovery_kib", "#a855f7"), ("Context switches", "context_switches", "#f59e0b")]
    blocks = []
    for n, (label, key, color) in enumerate(metrics):
        vals = [float(x.get(key, 0)) for x in rows]; low, high = min(vals, default=0), max(vals, default=1)
        if high == low: high = low + 1
        pts = " ".join(f"{40+i*25},{55+n*95-(v-low)/(high-low)*45:.1f}" for i,v in enumerate(vals))
        blocks.append(f'<text x="40" y="{20+n*95}" class="label">{label}</text><text x="640" y="{20+n*95}" text-anchor="end" class="value">latest: {vals[-1] if vals else 0:.2f}</text><line x1="40" y1="{55+n*95}" x2="640" y2="{55+n*95}" class="axis"/><polyline points="{pts}" stroke="{color}" class="series"/>')
    return '<svg xmlns="http://www.w3.org/2000/svg" width="680" height="500" viewBox="0 0 680 500"><style>.label{font:14px sans-serif;fill:#e5e7eb}.value{font:12px sans-serif;fill:#9ca3af}.axis{stroke:#374151}.series{fill:none;stroke-width:2}</style><rect width="100%" height="100%" fill="#111827"/>'+''.join(blocks)+'</svg>\n'

def replace(path: Path, begin: str, end: str, content: str) -> None:
    text = path.read_text(); start = text.index(begin) + len(begin); finish = text.index(end, start)
    path.write_text(text[:start] + "\n" + content.rstrip() + "\n" + text[finish:])

WEBSERVER_HISTORY = ROOT / "benchmarks" / "webserver.json"
WEBSERVER_LATENCY_SVG = ROOT / "docs" / "webserver-latency-distribution.svg"

# Servers compared in the KDE-style latency distribution chart: only the ones
# run under the identical wrk -t12 -c400 profile every CI cycle (the
# *_tuned entries use a different, lower-concurrency profile and aren't
# comparable here; cwist_c1m_arena1/cwist_c1m_drainchunk/cwist_sharded are
# targeted A/B legs against the plain cwist_c1m row, not part of the
# standing cross-framework comparison set).
_LATENCY_KDE_SERVERS = [
    ("CWIST Classic", "cwist", "#22c55e"),
    ("CWIST", "cwist_c1m", "#10b981"),
    ("Axum", "axum", "#3b82f6"),
    ("Gin", "gin", "#06b6d4"),
    ("Spring Boot", "spring", "#ef4444"),
]


def _percentile_anchors(ws_latest: dict, prefix: str) -> list[tuple[float, float]]:
    """(fraction, latency_ms) anchor points from whatever percentile fields
    are present for this server - gracefully degrades to the older
    avg/p90/p99/p99.999-only schema for history rows predating the fuller
    min/p50/p75/p999/p9999/max fields (see the workflow's parse_wrk())."""
    fields = [
        (0.0, "min_ms"), (0.5, "p50_ms"), (0.75, "p75_ms"), (0.90, "p90_ms"),
        (0.99, "p99_ms"), (0.999, "p999_ms"), (0.9999, "p9999_ms"),
        (0.99999, "p99_999_ms"), (1.0, "max_ms"),
    ]
    anchors = []
    for frac, suffix in fields:
        val = ws_latest.get(f"{prefix}_{suffix}")
        if val is None or val <= 0:
            continue
        anchors.append((frac, float(val)))
    # Strictly increasing in both fraction and latency - drop a point that
    # doesn't add new information (e.g. p999 == p99 when wrk rounds equal).
    cleaned: list[tuple[float, float]] = []
    for frac, val in anchors:
        if cleaned and (frac <= cleaned[-1][0] or val < cleaned[-1][1]):
            continue
        cleaned.append((frac, val))
    return cleaned


def _inverse_cdf_samples(anchors: list[tuple[float, float]], n: int) -> list[float]:
    """n synthetic latency samples by linearly interpolating the inverse CDF
    (quantile function) built from the known percentile anchors - the
    standard trick for reconstructing an approximate distribution shape from
    a handful of percentiles rather than raw per-request samples (wrk/the
    CI pipeline only ever gives us percentiles, never the raw latencies)."""
    if len(anchors) < 2:
        return [anchors[0][1]] * n if anchors else []
    samples = []
    for i in range(n):
        frac = (i + 0.5) / n
        for j in range(1, len(anchors)):
            f0, v0 = anchors[j - 1]
            f1, v1 = anchors[j]
            if frac <= f1 or j == len(anchors) - 1:
                t = 0.0 if f1 == f0 else (frac - f0) / (f1 - f0)
                t = min(1.0, max(0.0, t))
                samples.append(v0 + t * (v1 - v0))
                break
    return samples


def _gaussian_kde(samples: list[float], grid: list[float]) -> list[float]:
    """Textbook Gaussian KDE, pure stdlib (no numpy/scipy dependency here -
    matches this script's existing zero-dependency style). Bandwidth via
    Silverman's rule of thumb, degrading to a small fixed bandwidth when the
    sample is degenerate (e.g. every anchor collapsed to one value)."""
    n = len(samples)
    if n == 0:
        return [0.0] * len(grid)
    mean = sum(samples) / n
    var = sum((s - mean) ** 2 for s in samples) / n
    std = math.sqrt(var)
    sorted_s = sorted(samples)
    iqr = sorted_s[int(0.75 * (n - 1))] - sorted_s[int(0.25 * (n - 1))]
    spread = min(std, iqr / 1.34) if iqr > 0 else std
    if spread <= 0:
        spread = max(sorted_s[-1] - sorted_s[0], 1e-6) / 4 or 0.1
    bandwidth = max(0.9 * spread * n ** (-0.2), 1e-3)
    density = []
    norm = 1.0 / (n * bandwidth * math.sqrt(2 * math.pi))
    for x in grid:
        total = 0.0
        for s in samples:
            z = (x - s) / bandwidth
            total += math.exp(-0.5 * z * z)
        density.append(total * norm)
    return density


def render_latency_kde_svg(ws_latest: dict) -> str:
    """Latency *distribution* chart, distinct from the bar-chart summary in
    render_webserver_svg(): reconstructs an approximate density curve per
    server from its known percentiles (see _inverse_cdf_samples/_gaussian_kde)
    so the shape of the tail - not just its P99.999 number - is visible at a
    glance. X-axis uses log1p(ms) so a long Gin/Spring tail doesn't compress
    the CWIST/Axum curves into an unreadable spike at the left edge."""
    width, height = 1000, 460
    plot_x0, plot_x1 = 60, 940
    plot_y0, plot_y1 = 60, 380

    per_server = []
    max_ms_overall = 1.0
    for label, prefix, color in _LATENCY_KDE_SERVERS:
        anchors = _percentile_anchors(ws_latest, prefix)
        if len(anchors) < 2:
            continue
        samples = _inverse_cdf_samples(anchors, 400)
        per_server.append((label, color, samples))
        max_ms_overall = max(max_ms_overall, anchors[-1][1])

    def to_x(ms: float) -> float:
        span = math.log1p(max_ms_overall)
        return plot_x0 + (math.log1p(max(ms, 0.0)) / span) * (plot_x1 - plot_x0)

    blocks = [
        f'<text x="30" y="30" class="title">Latency Distribution (density, log scale) - {ws_latest.get("wrk_profile", "wrk 12t 400c")}</text>',
        f'<rect x="{plot_x0}" y="{plot_y0}" width="{plot_x1-plot_x0}" height="{plot_y1-plot_y0}" fill="#1f2937" rx="6" stroke="#374151"/>',
    ]

    tick_ms = [0, 1, 2, 5, 10, 20, 50, 100, 200, 500]
    tick_ms = [t for t in tick_ms if t <= max_ms_overall * 1.05] or [0, 1]
    for t in tick_ms:
        x = to_x(t)
        blocks.append(f'<line x1="{x:.1f}" y1="{plot_y0}" x2="{x:.1f}" y2="{plot_y1}" stroke="#374151" stroke-dasharray="2,3"/>')
        blocks.append(f'<text x="{x:.1f}" y="{plot_y1+16}" text-anchor="middle" class="tick">{t}ms</text>')

    legend_x = plot_x0
    for idx, (label, color, _samples) in enumerate(per_server):
        lx = legend_x + idx * 170
        blocks.append(f'<rect x="{lx}" y="34" width="11" height="11" fill="{color}" rx="2"/>')
        blocks.append(f'<text x="{lx+16}" y="43" class="legend">{label}</text>')

    grid_n = 240
    grid = [math.expm1((i / (grid_n - 1)) * math.log1p(max_ms_overall)) for i in range(grid_n)]
    for label, color, samples in per_server:
        density = _gaussian_kde(samples, grid)
        peak = max(density) or 1.0
        pts = []
        for ms, d in zip(grid, density):
            x = to_x(ms)
            y = plot_y1 - (d / peak) * (plot_y1 - plot_y0 - 10)
            pts.append(f"{x:.1f},{y:.1f}")
        blocks.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{color}" stroke-width="2.2" opacity="0.9"/>')

    blocks.append(f'<text x="{(plot_x0+plot_x1)//2}" y="{plot_y1+34}" text-anchor="middle" class="axis-label">Latency (ms, log scale)</text>')
    blocks.append(f'<text x="30" y="{(plot_y0+plot_y1)//2}" text-anchor="middle" class="axis-label" transform="rotate(-90 30 {(plot_y0+plot_y1)//2})">Relative density</text>')
    blocks.append(f'<text x="30" y="{height-14}" class="footer">Reconstructed from wrk percentiles (min/p50/p75/p90/p99/p99.9/p99.99/p99.999/max), not raw per-request samples - shape is representative, not exact.</text>')

    svg_style = (
        '<style>'
        '.title{font:15px sans-serif;font-weight:bold;fill:#f9fafb}'
        '.legend{font:12px sans-serif;fill:#d1d5db}'
        '.tick{font:10px sans-serif;fill:#9ca3af}'
        '.axis-label{font:12px sans-serif;fill:#9ca3af}'
        '.footer{font:10px sans-serif;fill:#6b7280}'
        '</style>'
    )
    return (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
        f'{svg_style}<rect width="100%" height="100%" fill="#111827"/>' + ''.join(blocks) + '</svg>\n'
    )

WEBSERVER_SVG = ROOT / "docs" / "webserver-benchmark-trends.svg"
README_MD = ROOT / "README.md"

def render_webserver_svg(history: list[dict]) -> str:
    ws_latest = history[-1] if history else {}
    metrics = [
        ("Throughput (req/s)", [("CWIST Classic", "cwist_rps", "#22c55e"), ("CWIST", "cwist_c1m_rps", "#10b981"), ("Axum", "axum_rps", "#3b82f6"), ("Gin", "gin_rps", "#06b6d4"), ("Spring", "spring_rps", "#ef4444")]),
        ("Avg Latency (ms)", [("CWIST Classic", "cwist_lat_ms", "#22c55e"), ("CWIST", "cwist_c1m_lat_ms", "#10b981"), ("Axum", "axum_lat_ms", "#3b82f6"), ("Gin", "gin_lat_ms", "#06b6d4"), ("Spring", "spring_lat_ms", "#ef4444")]),
        ("Group RSS end sample (KiB)", [("CWIST Classic", "cwist_rss_kib", "#22c55e"), ("CWIST", "cwist_c1m_rss_kib", "#10b981"), ("Axum", "axum_rss_kib", "#3b82f6"), ("Gin", "gin_rss_kib", "#06b6d4"), ("Spring", "spring_rss_kib", "#ef4444")]),
        ("Context Switches", [("CWIST Classic", "cwist_csw", "#22c55e"), ("CWIST", "cwist_c1m_csw", "#10b981"), ("Axum", "axum_csw", "#3b82f6"), ("Gin", "gin_csw", "#06b6d4"), ("Spring", "spring_csw", "#ef4444")])
    ]

    width = 1280
    height = 540
    blocks = []

    # Title & Legend
    blocks.append('<text x="30" y="35" class="title">Web Server Performance Comparison (wrk 12t 400c)</text>')
    blocks.append('<rect x="640" y="20" width="12" height="12" fill="#22c55e" rx="2"/><text x="658" y="31" class="legend">CWIST Classic</text>')
    blocks.append('<rect x="790" y="20" width="12" height="12" fill="#10b981" rx="2"/><text x="808" y="31" class="legend">CWIST</text>')
    blocks.append('<rect x="890" y="20" width="12" height="12" fill="#3b82f6" rx="2"/><text x="908" y="31" class="legend">Axum</text>')
    blocks.append('<rect x="965" y="20" width="12" height="12" fill="#06b6d4" rx="2"/><text x="983" y="31" class="legend">Gin</text>')
    blocks.append('<rect x="1030" y="20" width="12" height="12" fill="#ef4444" rx="2"/><text x="1048" y="31" class="legend">Spring Boot</text>')

    # Render 4 grid subpanels (2x2 layout)
    panel_w = 600
    panel_h = 200
    offsets = [(30, 60), (670, 60), (30, 290), (670, 290)]
    
    for idx, (m_title, series_list) in enumerate(metrics):
        px, py = offsets[idx]
        blocks.append(f'<rect x="{px}" y="{py}" width="{panel_w}" height="{panel_h}" fill="#1f2937" rx="6" stroke="#374151"/>')
        blocks.append(f'<text x="{px+15}" y="{py+28}" class="panel-title">{m_title}</text>')
        
        vals = [float(ws_latest[key]) for _, key, _ in series_list if type(ws_latest.get(key)) in (int, float) and math.isfinite(ws_latest[key])]
        max_val = max(vals, default=1.0)
        if max_val <= 0: max_val = 1.0
        
        bar_y_base = py + 44
        for s_idx, (label, key, color) in enumerate(series_list):
            available = type(ws_latest.get(key)) in (int, float) and math.isfinite(ws_latest[key])
            val = float(ws_latest[key]) if available else 0.0
            ratio = min(1.0, max(0.0, val / max_val))
            bar_len = int(ratio * 320)
            by = bar_y_base + s_idx * 27

            # Format value label
            if not available:
                val_str = "N/A"
            elif "ms" in m_title:
                val_str = f"{val:.2f} ms"
            elif "KiB" in m_title:
                val_str = f"{val:,.0f} KiB"
            elif "req/s" in m_title:
                val_str = f"{val:,.0f} req/s"
            else:
                val_str = f"{val:,.0f}"

            blocks.append(f'<text x="{px+15}" y="{by+16}" class="bar-label">{label}</text>')
            blocks.append(f'<rect x="{px+120}" y="{by}" width="320" height="22" fill="#374151" rx="3"/>')
            if bar_len > 0:
                blocks.append(f'<rect x="{px+120}" y="{by}" width="{bar_len}" height="22" fill="{color}" rx="3"/>')
            blocks.append(f'<text x="{px+450}" y="{by+16}" class="bar-val">{val_str}</text>')

    # Footer: recorded Spring/JVM & Go runtime environment & benchmark profile
    env = ws_latest.get("spring_env", {}) or {}
    go_env = ws_latest.get("go_env", {}) or {}
    if env or go_env:
        stack = env.get('stack')
        stack_part = f" ({stack})" if stack else ""
        vt = env.get('virtual_threads')
        vt_part = f" | virtual threads: {'on' if vt else 'off'}" if vt else ""
        footer1 = (f"Spring Boot {env.get('spring_boot_version','n/a')}{stack_part} | {env.get('java_version','n/a')} | "
                   f"JVM: {env.get('jvm_opts','n/a')}{vt_part}")
        footer2 = f"Profile: {ws_latest.get('wrk_profile', 'wrk 12t 400c')}"
        runner = ws_latest.get('runner_hw')
        if runner:
            footer2 += f" | Runner: {runner}"
        if go_env:
            footer2 += f" | {go_env.get('go_version', 'Go')} + {go_env.get('framework', 'Gin')}"
        blocks.append(f'<text x="30" y="512" class="footer">{footer1}</text>')
        blocks.append(f'<text x="30" y="530" class="footer">{footer2}</text>')

    svg_style = (
        '<style>'
        '.title{font:18px sans-serif;font-weight:bold;fill:#f9fafb}'
        '.panel-title{font:14px sans-serif;font-weight:600;fill:#9ca3af}'
        '.legend{font:13px sans-serif;fill:#d1d5db}'
        '.bar-label{font:13px sans-serif;font-weight:500;fill:#e5e7eb}'
        '.bar-val{font:12px sans-serif;font-weight:bold;fill:#f3f4f6}'
        '.footer{font:11px sans-serif;fill:#6b7280}'
        '</style>'
    )
    return f'<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{width}" height="{height}" viewBox="0 0 {width} {height}">{svg_style}<rect width="100%" height="100%" fill="#111827"/>' + ''.join(blocks) + '</svg>\n'

def compatible_history(history):
    if not history:
        return []
    contract = history[-1].get('benchmark_contract')
    return [row for row in history if row.get('benchmark_contract') == contract]


def webserver_summary(row):
    def metric(key, digits=3, scale=1):
        value = row.get(key)
        if type(value) not in (int, float) or not math.isfinite(value):
            return 'N/A'
        return f'{value / scale:,.{digits}f}'
    commit = row.get('commit', 'not recorded')
    run = row.get('run_url', 'not recorded')
    release = row.get('release_tag') or 'not recorded; identify this run by commit'
    lines = ['## Latest isolated HTTP benchmark', '',
             f"Measured commit: `{commit}`. Release tag: `{release}`.",
             f"Run: {run}. Timestamp: `{row.get('timestamp', 'not recorded')}`.",
             '', 'Latency columns use the **wrk corrected distribution**. Memory columns are process-group end samples, not peaks: Group RSS counts a page shared between worker processes once per process, Group PSS divides it by its mapper count, so compare a single-process server against PSS. Context switches are same-thread counter deltas over threads live at both ends of the window; N/A means unavailable.',
             '', '| Profile | Req/s | Mean ms | P99.999 ms | Group PSS MiB | Group RSS MiB | Context-switch delta |',
             '|---|---:|---:|---:|---:|---:|---:|']
    names = [('cwist','CWIST Classic'), ('cwist_c1m','CWIST'),
             ('cwist_c1m_arena1','CWIST arena_max=1'),
             ('cwist_c1m_drainchunk','CWIST drain_chunk=8'),
             ('axum','Axum'), ('gin','Gin'), ('spring','Spring Boot')]
    for key, name in names:
        lines.append(f"| {name} | {metric(key+'_rps',0)} | {metric(key+'_lat_ms')} | {metric(key+'_p99_999_ms')} | {metric(key+'_pss_kib',2,1024)} | {metric(key+'_rss_kib',2,1024)} | {metric(key+'_csw',0)} |")
    lines += ['', 'Main profile: `wrk -t12 -c400 -d10s`, after a discarded 10s warmup.',
              '', '### Separate tuned profile', '', '`wrk -t4 -c100 -d10s`, after a discarded 10s warmup. Do not compare these rows as equal-load results against the main table.']
    for key, name in [('cwist_tuned','CWIST Classic'), ('axum_tuned','Axum'), ('spring_tuned','Spring Boot')]:
        lines.append(f"- {name}: {metric(key+'_rps',0)} req/s; mean {metric(key+'_lat_ms')} ms; corrected P99.999 {metric(key+'_p99_999_ms')} ms.")
    spring = row.get('spring_env', {}) or {}
    if spring:
        lines += ['', f"Spring Boot row: {spring.get('java_version','n/a')}, Spring Boot {spring.get('spring_boot_version','n/a')}, {spring.get('stack','n/a')}. Full JVM options are recorded in `benchmarks/webserver.json`."]
    lines += ['', '[Measurement contract](docs/webserver-benchmark.md) · [History](benchmarks/webserver.json)']
    return '\n'.join(lines)


def render() -> None:
    history = json.loads(HISTORY.read_text()) if HISTORY.exists() else []
    SVG.parent.mkdir(parents=True, exist_ok=True); SVG.write_text(svg(history))
    latest = history[-1] if history else {}; summary = f"Latest automated benchmark: **{latest.get('os','n/a')}**: {latest.get('throughput_mleases_s',0)} M leases/s, {latest.get('cpu_percent',0)}% CPU, {latest.get('rss_kib',0)} KiB RSS.\n\n![CWIST benchmark trends](docs/benchmark-trends.svg)"
    replace(README, "<!-- BENCHMARKS:START -->", "<!-- BENCHMARKS:END -->", summary)
    replace(ROADMAP, "<!-- CI-BENCHMARKS:START -->", "<!-- CI-BENCHMARKS:END -->", f"Automated OS benchmark history is published in `docs/benchmark-trends.svg`. Latest platform: **{latest.get('os','n/a')}**.")

    ws_history = json.loads(WEBSERVER_HISTORY.read_text()) if WEBSERVER_HISTORY.exists() else []
    WEBSERVER_SVG.parent.mkdir(parents=True, exist_ok=True)
    WEBSERVER_SVG.write_text(render_webserver_svg(ws_history))
    ws_history = compatible_history(ws_history)
    ws_latest = ws_history[-1] if ws_history else {}
    WEBSERVER_LATENCY_SVG.parent.mkdir(parents=True, exist_ok=True)
    WEBSERVER_LATENCY_SVG.write_text(render_latency_kde_svg(ws_latest))

    def get_lat_part(prefix):
        p90 = ws_latest.get(f"{prefix}_p90_ms")
        p99 = ws_latest.get(f"{prefix}_p99_ms")
        p99_999 = ws_latest.get(f"{prefix}_p99_999_ms")
        if p90 is not None and p99 is not None:
            tail = f" (P90 {p90:.2f}ms, P99 {p99:.2f}ms"
            if p99_999 is not None:
                tail += f", P99.999 {p99_999:.2f}ms"
            tail += ")"
            return tail
        return ""

    cwist_lat_part = get_lat_part("cwist")
    cwist_c1m_lat_part = get_lat_part("cwist_c1m")
    cwist_c1m_arena1_lat_part = get_lat_part("cwist_c1m_arena1")
    cwist_c1m_drainchunk_lat_part = get_lat_part("cwist_c1m_drainchunk")
    axum_lat_part = get_lat_part("axum")
    gin_lat_part = get_lat_part("gin")
    spring_lat_part = get_lat_part("spring")

    if ws_latest.get('schema_version') == 2:
        # webserver_summary() renders the isolated-http1-wrk-corrected-v2
        # measurement-contract table only -- it predates the chart images
        # below and never appended them, which silently dropped the
        # benchmark visualization from README once schema_version:2 rows
        # started landing (docs/webserver-benchmark.md). Keep the
        # reproducibility-contract table but still attach the same charts
        # the pre-contract branch below has always shipped.
        ws_summary = webserver_summary(ws_latest)
        ws_summary += f"\n![Web Server Benchmark Trends](docs/webserver-benchmark-trends.svg)"
        ws_summary += (
            f"\n\nLatency distribution (density curve reconstructed from each "
            f"server's percentiles - shows the shape of the tail, not just its "
            f"P99.999 number):\n\n"
            f"![Web Server Latency Distribution](docs/webserver-latency-distribution.svg)"
        )
    else:
        ws_summary = (
            f"Latest Web Server Benchmark ({ws_latest.get('wrk_profile','wrk 12t 400c')}):\n"
            f"- **CWIST Classic pool**: {ws_latest.get('cwist_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_lat_ms',0):.2f}ms{cwist_lat_part} | RSS {ws_latest.get('cwist_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_csw',0):.0f}\n"
            f"- **CWIST reactor**: {ws_latest.get('cwist_c1m_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_c1m_lat_ms',0):.2f}ms{cwist_c1m_lat_part} | RSS {ws_latest.get('cwist_c1m_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_c1m_csw',0):.0f}\n"
            f"- **CWIST reactor (arena_max=1)** — glibc arena cap adopted in PR #35 after mimalloc was tried and refuted (issue #25); this line confirms the decision on every run: {ws_latest.get('cwist_c1m_arena1_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_c1m_arena1_lat_ms',0):.2f}ms{cwist_c1m_arena1_lat_part} | RSS {ws_latest.get('cwist_c1m_arena1_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_c1m_arena1_csw',0):.0f}\n"
            f"- **CWIST reactor (drain_chunk=8)** — cooperative queuing for cwist_async_defer completions within a big io_uring batch (issue #25, docs/cooperative-queuing.md); this workload has no cwist_async_defer traffic to interleave, so parity with the plain CWIST row above is the expected result, not a null finding — the tail-latency win is isolated directly in tests/bench_cooperative_queuing.c: {ws_latest.get('cwist_c1m_drainchunk_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_c1m_drainchunk_lat_ms',0):.2f}ms{cwist_c1m_drainchunk_lat_part} | RSS {ws_latest.get('cwist_c1m_drainchunk_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_c1m_drainchunk_csw',0):.0f}\n"
            f"- **Axum**: {ws_latest.get('axum_rps',0):.0f} req/s | Latency {ws_latest.get('axum_lat_ms',0):.2f}ms{axum_lat_part} | RSS {ws_latest.get('axum_rss_kib',0):.0f}KiB | Csw {ws_latest.get('axum_csw',0):.0f}\n"
            f"- **Gin (Go)**: {ws_latest.get('gin_rps',0):.0f} req/s | Latency {ws_latest.get('gin_lat_ms',0):.2f}ms{gin_lat_part} | RSS {ws_latest.get('gin_rss_kib',0):.0f}KiB | Csw {ws_latest.get('gin_csw',0):.0f}\n"
            f"- **Spring Boot**: {ws_latest.get('spring_rps',0):.0f} req/s | Latency {ws_latest.get('spring_lat_ms',0):.2f}ms{spring_lat_part} | RSS {ws_latest.get('spring_rss_kib',0):.0f}KiB | Csw {ws_latest.get('spring_csw',0):.0f}\n"
        )
        ws_env = ws_latest.get("spring_env", {}) or {}
        if ws_env:
            ws_summary += (
                "\n**Spring runtime environment**\n\n"
                f"- **JDK:** `{ws_env.get('java_version','n/a')}`\n"
                f"- **Spring Boot:** {ws_env.get('spring_boot_version','n/a')}\n"
            )
            if ws_env.get('stack'):
                ws_summary += f"- **Stack:** {ws_env['stack']}\n"
            ws_vt = ws_env.get('virtual_threads')
            if ws_vt is not None:
                ws_summary += f"- **Virtual threads:** {'enabled' if ws_vt else 'disabled'}\n"
            # Break before options, not within quoted values or historical AOT notes.
            # This is display formatting only; retain the recorded option spelling.
            jvm_opts = re.sub(
                r'''("(?:\\.|[^"\\])*"|'[^']*')|\s+(?=-)''',
                lambda match: match.group(1) if match.group(1) is not None else "\n",
                ws_env.get('jvm_opts', 'n/a').strip(),
            )
            ws_summary += (
                f"\n**JVM options**\n\n```text\n{jvm_opts}\n```\n"
                f"\n**Warmup/profile**\n\n{ws_latest.get('wrk_profile','n/a')}\n"
            )
        ws_summary += f"\n![Web Server Benchmark Trends](docs/webserver-benchmark-trends.svg)"
        ws_summary += (
            f"\n\nLatency distribution (density curve reconstructed from each "
            f"server's percentiles - shows the shape of the tail, not just its "
            f"P99.999 number):\n\n"
            f"![Web Server Latency Distribution](docs/webserver-latency-distribution.svg)"
        )
    # The runner CPU model changes from run to run and moves these numbers
    # more than most code changes do, so the single latest row above cannot
    # be compared against the previous one. Break the history out per CPU
    # so a reader sees which hardware produced what (see runner_baseline.py,
    # which gates regressions on the same split).
    ws_by_runner = summarize(ws_history, ["cwist_lat_ms", "cwist_c1m_lat_ms",
                                          "axum_lat_ms", "cwist_c1m_rps", "axum_rps"])
    if ws_by_runner:
        lines = ["", "### Per runner CPU", "",
                 "GitHub hands out a different CPU model per run, which moves these "
                 "numbers more than most code changes do. Medians of every recorded "
                 "run, split by the CPU it landed on, so rows are only comparable "
                 "down a column:", "",
                 "| Runner CPU | Runs | CWIST Classic ms | CWIST ms | Axum ms | "
                 "CWIST req/s | Axum req/s |",
                 "|---|---:|---:|---:|---:|---:|---:|"]
        for runner, count, stats in ws_by_runner:
            def cell(key, digits=2):
                value = stats.get(key)
                return f"{value:,.{digits}f}" if value is not None else "N/A"
            lines.append(
                f"| {runner} | {count} | {cell('cwist_lat_ms')} | "
                f"{cell('cwist_c1m_lat_ms')} | {cell('axum_lat_ms')} | "
                f"{cell('cwist_c1m_rps', 0)} | {cell('axum_rps', 0)} |")
        ws_summary += "\n" + "\n".join(lines) + "\n"

    if README.exists(): replace(README, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)
    if README_MD.exists(): replace(README_MD, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)

    tuned_rps = ws_latest.get("cwist_tuned_rps")
    axum_tuned_rps = ws_latest.get("axum_tuned_rps")
    if tuned_rps and README_MD.exists() and "<!-- TUNED_BENCHMARK:START -->" in README_MD.read_text():
        tuned_profile = ws_latest.get("tuned_profile") or "wrk -t4 -c100 -d10s"
        # Pair the tuned run with Axum, not Spring Boot. Both are compiled
        # servers with no managed runtime, so the comparison says something
        # about CWIST's own latency floor; beating a JVM server on latency and
        # memory is not informative about that. Axum runs the identical
        # -t4 -c100 profile (see the workflow's "Axum tuned" leg).
        tuned_line = (
            f"**Tuned low-latency run ({tuned_profile}), CWIST vs Axum on identical concurrency:**\n\n"
            f"- **CWIST**: {tuned_rps:,.0f} req/s at {ws_latest.get('cwist_tuned_lat_ms',0):.2f}ms average latency "
            f"(P50 {ws_latest.get('cwist_tuned_p50_ms',0):.2f}ms, P90 {ws_latest.get('cwist_tuned_p90_ms',0):.2f}ms, "
            f"P99 {ws_latest.get('cwist_tuned_p99_ms',0):.2f}ms)\n"
        )
        if axum_tuned_rps:
            tuned_line += (
                f"- **Axum**: {axum_tuned_rps:,.0f} req/s at {ws_latest.get('axum_tuned_lat_ms',0):.2f}ms average latency "
                f"(P50 {ws_latest.get('axum_tuned_p50_ms',0):.2f}ms, P90 {ws_latest.get('axum_tuned_p90_ms',0):.2f}ms, "
                f"P99 {ws_latest.get('axum_tuned_p99_ms',0):.2f}ms), same binary as the main run above\n"
            )
        tuned_line += (
            f"\nThese runs use a different concurrency budget from the main table. "
            f"They do not establish a causal scheduling explanation or a universal tail-latency improvement."
        )
        replace(README_MD, "<!-- TUNED_BENCHMARK:START -->", "<!-- TUNED_BENCHMARK:END -->", tuned_line)

if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "measure": print(json.dumps(run_measurement()))
    elif len(sys.argv) == 2 and sys.argv[1] == "render": render()
    else: raise SystemExit("usage: benchmark.py measure|render")
