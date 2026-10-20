#!/usr/bin/env bash
# HTTPS C1M gate: 1,000,000 concurrent TLS connections on one server.
#
# Extends the C100K gate (scripts/ci/c100k_https.sh) to one million
# connections. The standard loopback address (127.0.0.1) holds at most ~28K
# ephemeral ports, so this script creates 50 additional loopback addresses in
# the 127.1.0.0/16 range (127.1.x.1, x = 0..49), giving 50 * 28K ~ 1.4M
# ports. When run inside a network namespace those addresses are available
# without affecting the host network.
#
# Network namespace setup (handled here):
#
#   ip netns add cwist_c1m
#   ip netns exec cwist_c1m ip link set lo up
#   ip netns exec cwist_c1m ip addr add 127.1.0.1/16 dev lo   # base
#   ... repeat for each address ...
#
# The script detects whether the extra addresses exist and adds them if not.
# Running as root (or with CAP_NET_ADMIN) inside an existing netns is also
# supported: the CWIST_C1M_NETNS variable controls the namespace name.
#
# PASS requires every connection established, none dropped, every request
# answered with 200 "ok", the ramp finished within C1M_RAMP_MAX_S, and the
# server's RSS + swap <= C1M_RSS_PER_CONN_MAX_BYTES per connection.
#
# Needs: openssl, curl, ip (iproute2), RLIMIT_NOFILE hard limit >= 1M+4K.
# Exit 0 = PASS, 1 = FAIL, 2 = setup error.
set -u

CONNS="${C1M_CONNS:-1000000}"
PORT="${C1M_PORT:-19700}"
WORKERS="${CWIST_WORKERS:-8}"
THREADS="${C1M_CLIENT_THREADS:-12}"
INFLIGHT="${C1M_INFLIGHT:-2048}"
HOLD_S="${C1M_HOLD_S:-5}"
ROUNDS="${C1M_ROUNDS:-2}"
RAMP_MAX_S="${C1M_RAMP_MAX_S:-600}"
RSS_PER_CONN_MAX_BYTES="${C1M_RSS_PER_CONN_MAX_BYTES:-8192}"
SERVER_BIN="${SERVER_BIN:-./rss_gate_server}"
CLIENT_BIN="${CLIENT_BIN:-./c100k_https_client}"

# Number of loopback addresses to create; each holds ~28K ephemeral ports.
# 50 * 28K = 1.4M > 1M needed.
ADDR_COUNT="${C1M_ADDR_COUNT:-50}"
ADDR_BASE="127.1"
NETNS="${CWIST_C1M_NETNS:-}"

for tool in openssl curl ip; do
    command -v "$tool" >/dev/null || { echo "c1m: missing $tool" >&2; exit 2; }
done
[ -x "$SERVER_BIN" ] || { echo "c1m: $SERVER_BIN not built (make rss_gate_server)" >&2; exit 2; }
[ -x "$CLIENT_BIN" ] || { echo "c1m: $CLIENT_BIN not built (make c100k_https_client)" >&2; exit 2; }

need_fds=$((CONNS + 4096))
ulimit -n "$need_fds" 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
if [ "$(ulimit -n)" != unlimited ] && [ "$(ulimit -n)" -lt "$need_fds" ]; then
    echo "c1m: RLIMIT_NOFILE $(ulimit -n) < $need_fds (raise the hard limit)" >&2
    exit 2
fi

# --- Network namespace setup -----------------------------------------------

NETNS_CREATED=0

ns_exec() {
    if [ -n "$NETNS" ]; then
        ip netns exec "$NETNS" "$@"
    else
        "$@"
    fi
}

setup_netns() {
    if [ -n "$NETNS" ]; then
        if ! ip netns list | grep -qw "$NETNS"; then
            ip netns add "$NETNS"
            ip netns exec "$NETNS" ip link set lo up
            NETNS_CREATED=1
        fi
    fi

    # Add C1M_ADDR_COUNT loopback addresses (127.1.0.1 .. 127.1.49.1) unless
    # the first one is already present.
    if ! ns_exec ip addr show lo 2>/dev/null | grep -qF "${ADDR_BASE}.0.1"; then
        for i in $(seq 0 $((ADDR_COUNT - 1))); do
            ns_exec ip addr add "${ADDR_BASE}.${i}.1/16" dev lo 2>/dev/null || true
        done
    fi

    # Widen the ephemeral port range; each address now covers ~55K ports.
    ns_exec sysctl -qw net.ipv4.ip_local_port_range="1024 65535" 2>/dev/null || true
}

cleanup_netns() {
    if [ "$NETNS_CREATED" = 1 ] && [ -n "$NETNS" ]; then
        ip netns del "$NETNS" 2>/dev/null || true
    else
        # Remove the addresses we added (if any).
        for i in $(seq 0 $((ADDR_COUNT - 1))); do
            ns_exec ip addr del "${ADDR_BASE}.${i}.1/16" dev lo 2>/dev/null || true
        done
    fi
}

setup_netns

HOSTS_LIST=""
for i in $(seq 0 $((ADDR_COUNT - 1))); do
    HOSTS_LIST="${HOSTS_LIST}${HOSTS_LIST:+,}${ADDR_BASE}.${i}.1"
done

read -r port_lo port_hi < <(ns_exec sysctl -n net.ipv4.ip_local_port_range 2>/dev/null || echo "1024 65535")
nhosts=$(echo "$HOSTS_LIST" | tr ',' '\n' | grep -c .)
if [ $(((port_hi - port_lo + 1) * nhosts)) -lt "$CONNS" ]; then
    echo "c1m: port range $port_lo-$port_hi x $nhosts hosts < $CONNS connections" >&2
    cleanup_netns
    exit 2
fi

# --- Server -----------------------------------------------------------------

WORK="$(mktemp -d)"
ns_exec openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
    -subj /CN=localhost -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 \
    || { echo "c1m: certificate generation failed" >&2; cleanup_netns; exit 2; }

CWIST_WORKERS="$WORKERS" CWIST_HTTPS_IDLE_TIMEOUT_MS="${CWIST_HTTPS_IDLE_TIMEOUT_MS:-900000}" \
    ns_exec "$SERVER_BIN" "$PORT" "$WORK/cert.pem" "$WORK/key.pem" >c1m_server.log 2>&1 &
MASTER=$!
CLIENT=""

cleanup() {
    [ -n "$CLIENT" ] && kill "$CLIENT" 2>/dev/null
    kill -TERM "$MASTER" 2>/dev/null
    wait "$MASTER" 2>/dev/null
    rm -rf "$WORK"
    cleanup_netns
}
trap cleanup EXIT

URL="${ADDR_BASE}.0.1"
for _ in $(seq 1 200); do
    ns_exec curl -sk -o /dev/null -m 1 "https://${URL}:${PORT}/" && break
    sleep 0.1
done
ns_exec curl -sk -o /dev/null -m 2 "https://${URL}:${PORT}/" \
    || { echo "c1m: server did not come up" >&2; cat c1m_server.log >&2; exit 2; }

pids() { echo "$MASTER"; pgrep -P "$MASTER"; }
total_rss() {
    local t=0 p r
    for p in $(pids); do
        r=$(awk '/^(VmRSS|VmSwap):/{s += $2} END{print s + 0}' "/proc/$p/status" 2>/dev/null)
        t=$((t + ${r:-0}))
    done
    echo "$t"
}
idle_rss=$(total_rss)

# --- Client -----------------------------------------------------------------

C100K_READY_FILE="$WORK/ready" C100K_DONE_FILE="$WORK/done" C100K_LINGER_S=2 \
    ns_exec "$CLIENT_BIN" --hosts "$HOSTS_LIST" --port "$PORT" --conns "$CONNS" \
    --threads "$THREADS" --inflight "$INFLIGHT" --hold "$HOLD_S" --rounds "$ROUNDS" \
    --timeout "$RAMP_MAX_S" >c1m_client.log 2>&1 &
CLIENT=$!

open_rss=0
done_rss=0
while kill -0 "$CLIENT" 2>/dev/null; do
    if [ "$open_rss" = 0 ] && [ -e "$WORK/ready" ]; then open_rss=$(total_rss); fi
    if [ "$done_rss" = 0 ] && [ -e "$WORK/done" ]; then done_rss=$(total_rss); fi
    sleep 0.5
done
wait "$CLIENT"
client_rc=$?
CLIENT=""
cat c1m_client.log

val() { awk -F= -v k="$1" '$1==k{print $2}' c1m_client.log; }
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

summary="c1m: conns=$CONNS workers=$WORKERS ramp=${ramp_s}s established=$(val established) dropped_idle=$(val dropped_idle)"
for r in $(seq 1 "$ROUNDS"); do
    summary="$summary round$r=$(val "round${r}_ok")ok/$(val "round${r}_fail")fail@$(val "round${r}_rps")rps"
done
summary="$summary rss_idle=${idle_rss}KiB rss_open=${open_rss}KiB rss_done=${done_rss}KiB rss_per_conn=${per_conn}B"
echo "$summary"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### HTTPS C1M"
        echo
        echo '```'
        echo "$summary"
        echo '```'
        [ "$fail" = 0 ] && echo "**PASS**" || echo "**FAIL**: $reasons"
    } >>"$GITHUB_STEP_SUMMARY"
fi
if [ "$fail" != 0 ]; then
    echo "c1m: FAIL: $reasons" >&2
    tail -20 c1m_server.log >&2
    exit 1
fi
echo "c1m: PASS"
