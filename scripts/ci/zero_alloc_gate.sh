#!/usr/bin/env bash
# Hot-path allocation gate (#432): heap allocations per keep-alive request
# after warm-up must not exceed ZERO_ALLOC_BUDGET.
#
# tests/zero_alloc_server.c runs one worker with the LD_PRELOAD shim
# tests/zero_alloc_counter.c.  One curl invocation reuses a single connection
# for GET /zero_alloc/arm, ZERO_ALLOC_GATE_REQS x GET / and GET
# /zero_alloc/count; the same run without the GET / requests measures what
# arm/count cost themselves, and the difference is the hot path's share.
#
# The steady-state path is not allocation-free yet (each response body is a
# fresh sstring: 2 allocations per request), so the budget starts at 2 and
# fails any regression; the deterministic memory mode of #432 brings it to 0.
#
# Environment overrides:
#   ZERO_ALLOC_PORT          Listen port (default: 19600)
#   ZERO_ALLOC_WARMUP_REQS   Warm-up requests, not counted (default: 200)
#   ZERO_ALLOC_GATE_REQS     Counted requests (default: 1000)
#   ZERO_ALLOC_BUDGET        Allowed allocations per request (default: 2)
#   SERVER_BIN               Server binary (default: ./zero_alloc_server)
#   SHIM_LIB                 Shim library (default: ./zero_alloc_shim.so)
#
# Needs: curl. Exit 0 = PASS, 1 = FAIL, 2 = setup error.
set -u

PORT="${ZERO_ALLOC_PORT:-19600}"
WARMUP="${ZERO_ALLOC_WARMUP_REQS:-200}"
GATE_REQS="${ZERO_ALLOC_GATE_REQS:-1000}"
BUDGET="${ZERO_ALLOC_BUDGET:-2}"
SERVER_BIN="${SERVER_BIN:-./zero_alloc_server}"
SHIM_LIB="${SHIM_LIB:-./zero_alloc_shim.so}"
URL="http://127.0.0.1:${PORT}"

command -v curl >/dev/null || { echo "zero_alloc: missing curl" >&2; exit 2; }
[ -x "$SERVER_BIN" ] || { echo "zero_alloc: $SERVER_BIN not built (make zero_alloc_server)" >&2; exit 2; }
[ -f "$SHIM_LIB" ] || { echo "zero_alloc: $SHIM_LIB not built (make zero_alloc_shim.so)" >&2; exit 2; }

CWIST_WORKERS=1 LD_PRELOAD="$(realpath "$SHIM_LIB")" \
    "$SERVER_BIN" "$PORT" >zero_alloc_server.log 2>&1 &
SERVER=$!
trap 'kill -TERM "$SERVER" 2>/dev/null; wait "$SERVER" 2>/dev/null' EXIT

for _ in $(seq 1 100); do
    curl -s -o /dev/null -m 0.5 "${URL}/" && break
    sleep 0.1
done
curl -s -o /dev/null -m 2 "${URL}/" \
    || { echo "zero_alloc: server did not come up" >&2; cat zero_alloc_server.log >&2; exit 2; }

curl -s -o /dev/null "${URL}/?warmup=[1-${WARMUP}]"

# Prints the count read at the end of one keep-alive connection.
measure() {
    if [ "$1" -gt 0 ]; then
        curl -s -o /dev/null "${URL}/zero_alloc/arm" -o /dev/null "${URL}/?gate=[1-$1]" \
            "${URL}/zero_alloc/count"
    else
        curl -s -o /dev/null "${URL}/zero_alloc/arm" "${URL}/zero_alloc/count"
    fi
}
base=$(measure 0)
total=$(measure "$GATE_REQS")
case "$base$total" in
    ''|*[!0-9]*) echo "zero_alloc: bad counter reads (base='$base' total='$total')" >&2; exit 2 ;;
esac

allocs=$((total - base))
per_req=$(awk -v a="$allocs" -v n="$GATE_REQS" 'BEGIN { printf "%.3f", a / n }')
summary="zero_alloc: ${allocs} allocations over ${GATE_REQS} keep-alive requests (${per_req}/request, budget ${BUDGET})"
echo "$summary"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '### Hot-path allocation gate\n\n```\n%s\n```\n' "$summary" >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$allocs" -le $((BUDGET * GATE_REQS)) ]; then
    echo "zero_alloc: PASS"
    exit 0
fi
echo "zero_alloc: FAIL: the hot path allocates more than ${BUDGET} times per request" >&2
exit 1
