#!/usr/bin/env bash
# Zero-allocation gate (#432): verify the CWIST hot path performs no
# heap allocations after cwist_app_listen() returns from initialization.
#
# Mechanism:
#   1. Build the rss_gate_server (HTTP path) and an LD_PRELOAD shim
#      (tests/zero_alloc_counter.c) that counts post-init malloc/calloc/
#      realloc calls.
#   2. Start the server with the shim injected. After the server signals
#      readiness, send ZERO_ALLOC_WARMUP_REQS "warm-up" requests (not
#      counted against the gate).
#   3. Toggle the shim counter on, send ZERO_ALLOC_GATE_REQS gate
#      requests, toggle it off.
#   4. Fetch the counter value via a /zero_alloc_count endpoint wired
#      into the server when CWIST_ZERO_ALLOC_GATE=1.
#   5. PASS if the count == 0; FAIL otherwise.
#
# Environment overrides:
#   ZERO_ALLOC_PORT            Server listen port (default: 19600)
#   ZERO_ALLOC_WARMUP_REQS     Warm-up request count (default: 200)
#   ZERO_ALLOC_GATE_REQS       Gate request count (default: 1000)
#   SERVER_BIN                 Server binary path (default: ./rss_gate_server)
#   SHIM_LIB                   Path to the compiled LD_PRELOAD shim
#
# Needs: gcc, curl. Exit 0 = PASS, 1 = FAIL, 2 = setup error.
set -u

PORT="${ZERO_ALLOC_PORT:-19600}"
WARMUP="${ZERO_ALLOC_WARMUP_REQS:-200}"
GATE_REQS="${ZERO_ALLOC_GATE_REQS:-1000}"
SERVER_BIN="${SERVER_BIN:-./rss_gate_server}"
SHIM_SRC="tests/zero_alloc_counter.c"
SHIM_LIB="${SHIM_LIB:-/tmp/cwist_zero_alloc_shim.so}"
URL="http://127.0.0.1:${PORT}"

for tool in gcc curl; do
    command -v "$tool" >/dev/null || { echo "zero_alloc: missing $tool" >&2; exit 2; }
done
[ -x "$SERVER_BIN" ] || { echo "zero_alloc: $SERVER_BIN not built" >&2; exit 2; }
[ -f "$SHIM_SRC" ]    || { echo "zero_alloc: $SHIM_SRC not found" >&2; exit 2; }

# Build the LD_PRELOAD shim if not already present or stale.
if [ "$SHIM_SRC" -nt "$SHIM_LIB" ] 2>/dev/null || [ ! -f "$SHIM_LIB" ]; then
    gcc -O2 -fPIC -shared -o "$SHIM_LIB" "$SHIM_SRC" -ldl \
        || { echo "zero_alloc: shim build failed" >&2; exit 2; }
fi

# Start the server with the shim and the zero-alloc endpoint enabled.
LD_PRELOAD="$SHIM_LIB" CWIST_ZERO_ALLOC_GATE=1 CWIST_C1M_MODE=0 \
    "$SERVER_BIN" "$PORT" >zero_alloc_server.log 2>&1 &
MASTER=$!
cleanup() {
    kill -TERM "$MASTER" 2>/dev/null
    wait "$MASTER" 2>/dev/null
}
trap cleanup EXIT

# Wait for the server to come up (up to 10 s).
for _ in $(seq 1 100); do
    curl -s -o /dev/null -m 0.5 "${URL}/" && break
    sleep 0.1
done
curl -s -o /dev/null -m 2 "${URL}/" \
    || { echo "zero_alloc: server did not come up" >&2; cat zero_alloc_server.log >&2; exit 2; }

# Warm-up: let arenas, TLS session caches, ring buffers reach steady state.
for _ in $(seq 1 "$WARMUP"); do
    curl -s -o /dev/null "${URL}/" || true
done

# Arm the counter.
curl -s -o /dev/null "${URL}/zero_alloc_arm" || true

# Gate requests: these must not trigger any heap allocation.
for _ in $(seq 1 "$GATE_REQS"); do
    curl -s -o /dev/null "${URL}/" || true
done

# Disarm and read the count.
alloc_count=$(curl -s "${URL}/zero_alloc_count" 2>/dev/null | tr -d '[:space:]')
alloc_count="${alloc_count:-unknown}"

summary="zero_alloc: warmup=${WARMUP} gate_reqs=${GATE_REQS} post_init_allocs=${alloc_count}"
echo "$summary"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Zero-allocation gate"
        echo
        echo '```'
        echo "$summary"
        echo '```'
    } >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$alloc_count" = "0" ]; then
    echo "zero_alloc: PASS"
    exit 0
else
    echo "zero_alloc: FAIL: ${alloc_count} unexpected heap allocation(s) on hot path" >&2
    tail -20 zero_alloc_server.log >&2
    exit 1
fi
