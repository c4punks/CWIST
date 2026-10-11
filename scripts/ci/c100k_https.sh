#!/usr/bin/env bash
# HTTPS C100k gate: 100,000 concurrent TLS connections on one server.
#
# Starts tests/rss_gate_server (cwist_app_listen over TLS, prefork workers)
# with a throwaway ECDSA P-256 certificate and drives it with
# tests/c100k_https_client, which:
#
#   1. opens C100K_CONNS connections (TCP + TLS handshake) and keeps all of
#      them open at the same time,
#   2. leaves them idle for C100K_HOLD_S seconds (the server must not drop
#      any: idle HTTPS connections live in the park set),
#   3. runs C100K_ROUNDS keep-alive rounds, one GET / per connection per
#      round, every request written before any response is awaited.
#
# PASS requires every connection established, none dropped, every request
# answered with 200 "ok", the ramp finished within C100K_RAMP_MAX_S, and the
# server's total RSS with all connections open at most
# C100K_RSS_PER_CONN_MAX_BYTES per connection.
#
# The client spreads connections over C100K_HOSTS (default 127.0.0.1-4): each
# destination address has its own ephemeral-port space, so 100k connections
# fit in the default 32768-60999 range (28k ports per address) with headroom.
# Near-full port spaces make connect() search for a free port for longer and
# longer, which is what slows the ramp, not the server.
#
# Needs: RLIMIT_NOFILE hard limit >= C100K_CONNS + 4096 (client and server are
# separate processes, each needs one fd per connection), openssl, curl.
# Exit 0 = PASS, 1 = FAIL, 2 = setup error. Results are appended to
# $GITHUB_STEP_SUMMARY when it is set.
set -u

CONNS="${C100K_CONNS:-100000}"
HOSTS="${C100K_HOSTS:-127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4}"
PORT="${C100K_PORT:-19600}"
WORKERS="${CWIST_WORKERS:-4}"
THREADS="${C100K_CLIENT_THREADS:-4}"
INFLIGHT="${C100K_INFLIGHT:-512}"
HOLD_S="${C100K_HOLD_S:-5}"
ROUNDS="${C100K_ROUNDS:-2}"
RAMP_MAX_S="${C100K_RAMP_MAX_S:-180}"
RSS_PER_CONN_MAX_BYTES="${C100K_RSS_PER_CONN_MAX_BYTES:-16384}"
SERVER_BIN="${SERVER_BIN:-./rss_gate_server}"
CLIENT_BIN="${CLIENT_BIN:-./c100k_https_client}"

for tool in openssl curl; do
    command -v "$tool" >/dev/null || { echo "c100k: missing $tool" >&2; exit 2; }
done
[ -x "$SERVER_BIN" ] || { echo "c100k: $SERVER_BIN not built (make rss_gate_server)" >&2; exit 2; }
[ -x "$CLIENT_BIN" ] || { echo "c100k: $CLIENT_BIN not built (make c100k_https_client)" >&2; exit 2; }

need_fds=$((CONNS + 4096))
ulimit -n "$need_fds" 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
if [ "$(ulimit -n)" != unlimited ] && [ "$(ulimit -n)" -lt "$need_fds" ]; then
    echo "c100k: RLIMIT_NOFILE $(ulimit -n) < $need_fds (raise the hard limit)" >&2
    exit 2
fi
read -r port_lo port_hi </proc/sys/net/ipv4/ip_local_port_range
nhosts=$(echo "$HOSTS" | tr ',' '\n' | grep -c .)
if [ $(((port_hi - port_lo + 1) * nhosts)) -lt "$CONNS" ]; then
    echo "c100k: port range $port_lo-$port_hi x $nhosts hosts < $CONNS connections" >&2
    exit 2
fi

WORK="$(mktemp -d)"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
    -subj /CN=localhost -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 \
    || { echo "c100k: certificate generation failed" >&2; exit 2; }

# The idle budget must outlast ramp + hold, or the server would rightly close
# the earliest connections before the request rounds.
CWIST_WORKERS="$WORKERS" CWIST_HTTPS_IDLE_TIMEOUT_MS="${CWIST_HTTPS_IDLE_TIMEOUT_MS:-600000}" \
    "$SERVER_BIN" "$PORT" "$WORK/cert.pem" "$WORK/key.pem" >c100k_server.log 2>&1 &
MASTER=$!
CLIENT=""
cleanup() {
    [ -n "$CLIENT" ] && kill "$CLIENT" 2>/dev/null
    kill -TERM "$MASTER" 2>/dev/null
    wait "$MASTER" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

URL="https://127.0.0.1:${PORT}/"
for _ in $(seq 1 100); do
    curl -sk -o /dev/null -m 1 "$URL" && break
    sleep 0.1
done
curl -sk -o /dev/null -m 2 "$URL" || { echo "c100k: server did not come up" >&2; cat c100k_server.log >&2; exit 2; }

pids() { echo "$MASTER"; pgrep -P "$MASTER"; }
total_rss() {
    local t=0 p r
    for p in $(pids); do
        r=$(awk '/^VmRSS:/{print $2}' "/proc/$p/status" 2>/dev/null)
        t=$((t + ${r:-0}))
    done
    echo "$t"
}
idle_rss=$(total_rss)

C100K_READY_FILE="$WORK/ready" C100K_DONE_FILE="$WORK/done" C100K_LINGER_S=2 \
    "$CLIENT_BIN" --hosts "$HOSTS" --port "$PORT" --conns "$CONNS" --threads "$THREADS" \
    --inflight "$INFLIGHT" --hold "$HOLD_S" --rounds "$ROUNDS" --timeout "$RAMP_MAX_S" \
    >c100k_client.log 2>&1 &
CLIENT=$!

# Sample server RSS once every connection is open (peak idle footprint) and
# again right after the last request round (peak loaded footprint).
open_rss=0
done_rss=0
while kill -0 "$CLIENT" 2>/dev/null; do
    if [ "$open_rss" = 0 ] && [ -e "$WORK/ready" ]; then open_rss=$(total_rss); fi
    if [ "$done_rss" = 0 ] && [ -e "$WORK/done" ]; then done_rss=$(total_rss); fi
    sleep 0.2
done
wait "$CLIENT"
client_rc=$?
CLIENT=""
cat c100k_client.log

val() { awk -F= -v k="$1" '$1==k{print $2}' c100k_client.log; }
ramp_s=$(val ramp_seconds)
peak_rss=$((open_rss > done_rss ? open_rss : done_rss))
per_conn=$(((peak_rss - idle_rss) * 1024 / CONNS))

fail=0
reasons=""
[ "$client_rc" = 0 ] || { fail=1; reasons="${reasons}client=FAIL(rc=$client_rc) "; }
if [ -z "$ramp_s" ] || awk -v r="$ramp_s" -v m="$RAMP_MAX_S" 'BEGIN{exit !(r > m)}'; then
    fail=1
    reasons="${reasons}ramp ${ramp_s:-?}s > ${RAMP_MAX_S}s "
fi
if [ "$open_rss" = 0 ] || [ "$per_conn" -gt "$RSS_PER_CONN_MAX_BYTES" ]; then
    fail=1
    reasons="${reasons}rss/conn ${per_conn}B > ${RSS_PER_CONN_MAX_BYTES}B "
fi
if ! kill -0 "$MASTER" 2>/dev/null; then
    fail=1
    reasons="${reasons}server exited "
fi

summary="c100k: conns=$CONNS workers=$WORKERS ramp=${ramp_s}s established=$(val established) dropped_idle=$(val dropped_idle)"
for r in $(seq 1 "$ROUNDS"); do
    summary="$summary round$r=$(val "round${r}_ok")ok/$(val "round${r}_fail")fail@$(val "round${r}_rps")rps"
done
summary="$summary rss_idle=${idle_rss}KiB rss_open=${open_rss}KiB rss_done=${done_rss}KiB rss_per_conn=${per_conn}B"
echo "$summary"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### HTTPS C100k"
        echo
        echo '```'
        echo "$summary"
        echo '```'
        [ "$fail" = 0 ] && echo "**PASS**" || echo "**FAIL**: $reasons"
    } >>"$GITHUB_STEP_SUMMARY"
fi
if [ "$fail" != 0 ]; then
    echo "c100k: FAIL: $reasons" >&2
    tail -20 c100k_server.log >&2
    exit 1
fi
echo "c100k: PASS"
