#!/usr/bin/env bash
# Soak test runner — implements docs/soak-testing.md (ROADMAP v4.0 exit
# criterion: "the soak run is defined and passes on the release commit").
#
# Usage: scripts/ci/soak.sh            # 10-minute soak against a self-built
#                                      # example/simple-server
#        DURATION=1800 scripts/ci/soak.sh   # release-candidate run (30 min)
#        DURATION=60 scripts/ci/soak.sh     # smoke run
#
# Env overrides:
#   DURATION        total run seconds including warmup (default 600)
#   PORT/BASE       target port / base URL (default 8080 / http://127.0.0.1:8080)
#   BASE_HTTPS      optional https base URL; when set, one extra keep-alive
#                   slice runs against it per loop (HTTPS-on configuration)
#   SERVER_BIN      server binary to build and start (default
#                   example/simple-server/simple-server); set SERVER_BIN to an
#                   empty string and the script will not start or stop one
#   CONCURRENCY     wrk keep-alive connections (default 64)
#   SLICE           seconds per workload slice (default 60)
#   RSS_GROWTH_PCT  max RSS growth over baseline, percent (default 10)
#   FD_GROWTH_ABS   max open-fd growth over baseline, absolute fds (default 5)
#   LAT_DRIFT_PCT   max p99 latency drift over baseline slice, percent (20)
#
# Exit status 0 = PASS, 1 = FAIL. The server is killed on every exit path.
set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

DURATION=${DURATION:-600}
PORT=${PORT:-8080}
BASE=${BASE:-http://127.0.0.1:$PORT}
BASE_HTTPS=${BASE_HTTPS:-}
SERVER_BIN=${SERVER_BIN:-example/simple-server/simple-server}
CONCURRENCY=${CONCURRENCY:-64}
SLICE=${SLICE:-60}
RSS_GROWTH_PCT=${RSS_GROWTH_PCT:-10}
FD_GROWTH_ABS=${FD_GROWTH_ABS:-5}
LAT_DRIFT_PCT=${LAT_DRIFT_PCT:-20}
WRK=${WRK:-wrk}

command -v "$WRK" >/dev/null || { echo "soak: wrk not found" >&2; exit 1; }
command -v curl >/dev/null || { echo "soak: curl not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "soak: python3 not found" >&2; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ;;
    *) echo "soak: unsupported environment (need Linux x86_64)" >&2; exit 1 ;;
esac

if [ -n "$SERVER_BIN" ]; then
    if [ ! -x "$SERVER_BIN" ]; then
        echo "soak: building bench server ($SERVER_BIN)..."
        make "$SERVER_BIN"
    fi
fi

# --- helpers ---------------------------------------------------------------

# Sum VmRSS (kB) and count open fds across the server's whole process group,
# keyed off /proc like scripts/ci/benchmark_workload.sh does (prefork workers
# may live in child processes).
pg_stat() {
    python3 - "$1" <<'EOF'
import os, sys
pgid = int(sys.argv[1])
rss_kb = 0
fds = 0
for entry in sorted(os.listdir('/proc')):
    if not entry.isdigit():
        continue
    try:
        fields = open(f'/proc/{entry}/stat').read().rsplit(') ', 1)[1].split()
        if int(fields[2]) != pgid or fields[0] in ('Z', 'X'):
            continue
        rss_kb += int(next(l for l in open(f'/proc/{entry}/status') if l.startswith('VmRSS:')).split()[1])
        fds += len(os.listdir(f'/proc/{entry}/fd'))
    except (FileNotFoundError, ProcessLookupError, PermissionError, StopIteration):
        continue
print(f'{rss_kb} {fds}')
EOF
}

alive() {
    local state
    state=$(ps -o stat= -p "$1" 2>/dev/null || true)
    case "$state" in
        "" | Z*) return 1 ;;
    esac
    return 0
}

# Run one wrk slice; parse p99 latency (us), socket errors, non-2xx count.
# Sets: WRK_P99_US WRK_ERRORS
run_slice() {
    local url=$1; shift || true
    local out
    out=$("$WRK" -t4 -c"$CONCURRENCY" -d"${SLICE}s" --latency "$@" "$url" 2>&1)
    WRK_P99_US=$(printf '%s\n' "$out" | awk '
        / 99%/ {
            tok=$2; val=tok; sub(/[^0-9.].*$/, "", val);
            unit=tok; sub(/^[0-9.]+/, "", unit);
            f = unit=="us" ? 1 : unit=="ms" ? 1000 : unit=="s" ? 1000000 : 0;
            if (f) printf "%d", val*f }')
    WRK_ERRORS=$(printf '%s\n' "$out" | python3 -c '
import re, sys
text = sys.stdin.read()
errs = 0
m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
if m: errs += sum(int(g) for g in m.groups())
m = re.search(r"Non-2xx or 3xx responses: (\d+)", text)
if m: errs += int(m.group(1))
print(errs)')
    printf '%s\n' "$out" | grep -E 'Latency|99%|Requests/sec|Socket errors|Non-2xx' || true
}

# --- server startup --------------------------------------------------------

pid=""
if [ -n "$SERVER_BIN" ]; then
    setsid "$SERVER_BIN" >/tmp/cwist-soak-server.log 2>&1 &
    pid=$!
fi
pgid=$pid

cleanup() {
    [ -n "$pid" ] && kill -KILL -- "-$pgid" 2>/dev/null || true
}
trap cleanup EXIT

if [ -n "$pid" ]; then
    for i in $(seq 1 30); do
        curl -sf "$BASE/health" >/dev/null 2>&1 && break
        alive "$pid" || { echo "soak: server exited during startup:" >&2; cat /tmp/cwist-soak-server.log >&2; exit 1; }
        sleep 1
    done
    curl -sf "$BASE/health" >/dev/null || { echo "soak: server did not become healthy" >&2; exit 1; }
fi

# --- warmup ----------------------------------------------------------------

warmup=$SLICE
[ $((DURATION / 10)) -lt "$warmup" ] && warmup=$((DURATION / 10))
[ "$warmup" -lt 5 ] && warmup=5
echo "soak: warmup ${warmup}s"
run_slice "$BASE/health" >/dev/null || { echo "soak: warmup slice failed" >&2; exit 1; }

read -r rss0 fd0 <<<"$(pg_stat "$pgid")"
echo "soak: baseline RSS=${rss0}kB fds=${fd0}"
lat_first=""
lat_last=""
errors_total=0
slices=$(((DURATION - warmup) / SLICE))
[ "$slices" -lt 1 ] && slices=1

# --- measured slices --------------------------------------------------------

i=0
while [ $i -lt "$slices" ]; do
    i=$((i + 1))
    if [ $((i % 2)) -eq 1 ]; then
        echo "soak: [$i/$slices] keep-alive slice ${SLICE}s -> $BASE/"
        run_slice "$BASE/"
    else
        echo "soak: [$i/$slices] churn slice ${SLICE}s (Connection: close) -> $BASE/health"
        run_slice "$BASE/health" '-H "Connection: close"'
    fi
    if [ -n "$BASE_HTTPS" ]; then
        echo "soak: [$i/$slices] https keep-alive slice -> $BASE_HTTPS/health"
        run_slice "$BASE_HTTPS/health"
    fi
    if [ -n "$pid" ] && ! alive "$pid"; then
        echo "soak: FAIL — server exited mid-run" >&2
        cat /tmp/cwist-soak-server.log >&2 || true
        exit 1
    fi
    [ -n "$WRK_ERRORS" ] && errors_total=$((errors_total + WRK_ERRORS))
    if [ -n "$WRK_P99_US" ]; then
        [ -z "$lat_first" ] && lat_first=$WRK_P99_US
        lat_last=$WRK_P99_US
    fi
    read -r rss fds <<<"$(pg_stat "$pgid")"
    echo "soak: [$i/$slices] RSS=${rss}kB fds=${fds} p99=${WRK_P99_US:-?}us errors=${WRK_ERRORS:-?}"
done

# --- verdict -----------------------------------------------------------------

read -r rss fds <<<"$(pg_stat "$pgid")"
fail=0

rss_growth_pct=$(awk -v a="$rss0" -v b="$rss" 'BEGIN{ if (a<=0) print 0; else printf "%.1f", (b-a)*100/a }')
fd_growth=$((fds - fd0))
lat_drift_pct=0
if [ -n "$lat_first" ] && [ "$lat_first" -gt 0 ] && [ -n "$lat_last" ]; then
    lat_drift_pct=$(awk -v a="$lat_first" -v b="$lat_last" 'BEGIN{ printf "%.1f", (b-a)*100/a }')
fi

echo "soak: --- summary ---"
echo "soak: RSS ${rss0}kB -> ${rss}kB (+${rss_growth_pct}%, limit ${RSS_GROWTH_PCT}%)"
echo "soak: fds ${fd0} -> ${fds} (+${fd_growth}, limit +${FD_GROWTH_ABS})"
echo "soak: p99 ${lat_first:-?}us -> ${lat_last:-?}us (drift ${lat_drift_pct}%, limit ${LAT_DRIFT_PCT}%)"
echo "soak: errors total ${errors_total} (limit 0)"

if [ -n "$pid" ] && ! alive "$pid"; then
    echo "soak: FAIL — server not alive at end of run"
    fail=1
fi
awk -v g="$rss_growth_pct" -v l="$RSS_GROWTH_PCT" 'BEGIN{ exit !(g > l) }' && { echo "soak: FAIL — RSS growth above limit"; fail=1; }
[ "$fd_growth" -gt "$FD_GROWTH_ABS" ] && { echo "soak: FAIL — fd growth above limit"; fail=1; }
awk -v g="$lat_drift_pct" -v l="$LAT_DRIFT_PCT" 'BEGIN{ exit !(g > l) }' && { echo "soak: FAIL — p99 latency drift above limit"; fail=1; }
[ "$errors_total" -ne 0 ] && { echo "soak: FAIL — non-zero errors"; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "soak: PASS"
    exit 0
fi
echo "soak: FAIL"
exit 1
