#!/usr/bin/env bash
# RSS gate: resident memory of a real HTTPS deployment.
#
# Starts tests/rss_gate_server (cwist_app_listen over TLS, prefork workers)
# and checks two things:
#
#   1. Idle footprint. After start-up and one request per worker, no single
#      process (master or worker) may exceed RSS_IDLE_MAX_KIB. This is the
#      check that catches fixed per-process costs such as the static
#      ~96 MiB HTTPS task queue fixed in v3.9.1.
#   2. Steady state. The same load (keep-alive, connection churn, a
#      connection burst, large responses) runs twice. Total RSS after the
#      second round may exceed total RSS after the first by at most
#      max(RSS_GROWTH_MIN_KIB, RSS_GROWTH_PCT % of round 1). Round 1 absorbs
#      warm-up (arenas, TLS session caches, ring growth), so growth on an
#      identical second round points at a leak.
#
# All limits are environment overrides. Exit 0 = PASS, 1 = FAIL, 2 = setup
# error. Results are appended to $GITHUB_STEP_SUMMARY when it is set.
set -u

PORT="${PORT:-19480}"
WORKERS="${CWIST_WORKERS:-4}"
SERVER_BIN="${SERVER_BIN:-./rss_gate_server}"
RSS_IDLE_MAX_KIB="${RSS_IDLE_MAX_KIB:-32768}"
RSS_GROWTH_PCT="${RSS_GROWTH_PCT:-10}"
RSS_GROWTH_MIN_KIB="${RSS_GROWTH_MIN_KIB:-8192}"
LOAD_SECONDS="${LOAD_SECONDS:-10}"
BURST_CONNS="${BURST_CONNS:-1000}"
URL="https://127.0.0.1:${PORT}"

for tool in wrk ab h2load curl; do
    command -v "$tool" >/dev/null || { echo "rss_gate: missing $tool" >&2; exit 2; }
done
[ -x "$SERVER_BIN" ] || { echo "rss_gate: $SERVER_BIN not built (make rss_gate_server)" >&2; exit 2; }

ulimit -n 65536 2>/dev/null || true
CWIST_WORKERS="$WORKERS" CWIST_C1M_MODE="${CWIST_C1M_MODE:-1}" "$SERVER_BIN" "$PORT" >rss_gate_server.log 2>&1 &
MASTER=$!
cleanup() { kill -TERM "$MASTER" 2>/dev/null; wait "$MASTER" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 100); do
    curl -sk -o /dev/null -m 1 "$URL/" && break
    sleep 0.1
done
curl -sk -o /dev/null -m 2 "$URL/" || { echo "rss_gate: server did not come up" >&2; cat rss_gate_server.log >&2; exit 2; }

pids() { echo "$MASTER"; pgrep -P "$MASTER"; }
rss_of() { awk '/^VmRSS:/{print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
total_rss() { local t=0 p; for p in $(pids); do t=$((t + $(rss_of "$p"))); done; echo "$t"; }

# Touch every worker once so each has initialized its pool and reactor.
for _ in $(seq 1 $((WORKERS * 8))); do curl -sk -o /dev/null -m 2 "$URL/"; done
sleep 1

fail=0
idle_report=""
max_idle=0
for p in $(pids); do
    r=$(rss_of "$p")
    idle_report+="pid $p: ${r} KiB; "
    [ "$r" -gt "$max_idle" ] && max_idle=$r
done
idle_total=$(total_rss)
echo "idle: total ${idle_total} KiB, largest process ${max_idle} KiB (limit ${RSS_IDLE_MAX_KIB}); ${idle_report}"
if [ "$max_idle" -gt "$RSS_IDLE_MAX_KIB" ]; then
    echo "FAIL: idle process RSS ${max_idle} KiB > ${RSS_IDLE_MAX_KIB} KiB"
    fail=1
fi

load_round() {
    wrk -t4 -c64 -d"${LOAD_SECONDS}s" "$URL/" >/dev/null 2>&1
    wrk -t2 -c16 -d"$((LOAD_SECONDS / 2 + 1))s" "$URL/big" >/dev/null 2>&1
    ab -q -n 2000 -c 32 "$URL/" >/dev/null 2>&1
    h2load --h1 -n "$BURST_CONNS" -c "$BURST_CONNS" -t 4 "$URL/" >/dev/null 2>&1
    h2load -n 4000 -c 32 -m 8 -t 4 "$URL/" >/dev/null 2>&1
    sleep 2
}

load_round
round1=$(total_rss)
load_round
round2=$(total_rss)
growth=$((round2 - round1))
allowed=$((round1 * RSS_GROWTH_PCT / 100))
[ "$allowed" -lt "$RSS_GROWTH_MIN_KIB" ] && allowed=$RSS_GROWTH_MIN_KIB
echo "steady state: after round 1 ${round1} KiB, after round 2 ${round2} KiB, growth ${growth} KiB (allowed ${allowed})"
if [ "$growth" -gt "$allowed" ]; then
    echo "FAIL: RSS grew ${growth} KiB over an identical second load round (allowed ${allowed} KiB)"
    fail=1
fi

curl -sk -o /dev/null -m 2 "$URL/" || { echo "FAIL: server stopped answering after load"; fail=1; }

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### RSS gate"
        echo
        echo "| measurement | value | limit |"
        echo "|---|---|---|"
        echo "| largest idle process | ${max_idle} KiB | ${RSS_IDLE_MAX_KIB} KiB |"
        echo "| idle total (${WORKERS} workers + master) | ${idle_total} KiB | |"
        echo "| total after load round 1 | ${round1} KiB | |"
        echo "| growth over round 2 | ${growth} KiB | ${allowed} KiB |"
        echo
        [ "$fail" -eq 0 ] && echo "**PASS**" || echo "**FAIL**"
    } >>"$GITHUB_STEP_SUMMARY"
fi

[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit "$fail"
