#!/usr/bin/env bash
# C1M orchestration: 1,000,000 concurrent HTTPS connections against one
# rss_gate_server, driven by 8 c100k_https_client processes x 125,000 conns.
# Modeled on scripts/ci/c100k_https.sh; same PASS criteria semantics.
set -u

CONNS_PER_CLIENT="${C1M_CONNS_PER_CLIENT:-125000}"
CLIENTS="${C1M_CLIENTS:-8}"
# Each client gets its own /16 of loopback (127.0.<i+1>.1-3). The destination
# address is part of the TCP 4-tuple: sharing addresses across clients makes
# their ephemeral-port ranges collide in the kernel ehash/conntrack, and the
# ramp deadlocks around ~180k total connections with connect() spinning in
# kernel space.
HOSTS_PER_CLIENT="${C1M_HOSTS_PER_CLIENT:-3}"
PORT="${C1M_PORT:-19601}"
WORKERS="${CWIST_WORKERS:-4}"
THREADS="${C1M_CLIENT_THREADS:-4}"
INFLIGHT="${C1M_INFLIGHT:-512}"
HOLD_S="${C1M_HOLD_S:-5}"
ROUNDS="${C1M_ROUNDS:-2}"
RAMP_MAX_S="${C1M_RAMP_MAX_S:-180}"
# Clients peak in memory during their own ramp (fresh SSL objects and
# handshake buffers, ~35-40 KB per connection); once idle, RELEASE_BUFFERS
# shrinks them to a fraction. Starting them together piles every ramp onto
# the same wall clock, which needs ~40 GB of free RAM for 8x125K. Staggering
# the starts keeps the concurrent peak near one ramp plus the idle fleet.
STAGGER_S="${C1M_STAGGER_S:-0}"
# A client normally exits after its rounds, closing its connections. With a
# stagger, earlier clients must linger so all 1M connections stay open until
# the last client finishes.
LINGER_S="${C1M_LINGER_S:-0}"
# Client binary. The stock c100k_https_client links system OpenSSL, which
# ignores SSL_MODE_RELEASE_BUFFERS: every held connection keeps its two 16 KiB
# buffers, so an idle 125K-connection client still resident-uses ~4.2 GiB and
# the 8-client fleet needs ~34 GiB. A build against the repo's BoringSSL
# (RELEASE_BUFFERS is honored) idles at a fraction of that:
#   gcc -O2 -o c100k_bssl_client tests/c100k_https_client.c \
#       -Ilib/boringssl/include lib/boringssl/build/libssl.a \
#       lib/boringssl/build/libcrypto.a -lpthread -ldl -lstdc++
CLIENT_BIN="${C1M_CLIENT_BIN:-./c100k_https_client}"

ulimit -n 1050000 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
echo "ulimit -n = $(ulimit -n)"

# A fresh user+network namespace (unshare -Urn) starts from kernel defaults:
# ip_local_port_range is 32768-60999, which caps each client at
# HOSTS_PER_CLIENT x 28232 connections (3 hosts -> 84696, observed exactly),
# and somaxconn is small. On the host these writes need root and fail
# silently; inside the namespace we have it. Written via /proc/sys because
# sysctl(8) lives in /usr/sbin, which is not on a bare unshare PATH.
echo "1024 65535" > /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null
echo 1048576 > /proc/sys/net/core/somaxconn 2>/dev/null
echo 2097152 > /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null
echo "port_range=$(cat /proc/sys/net/ipv4/ip_local_port_range)"

cd "$(dirname "$0")/../.."

WORK="$(mktemp -d)"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
    -subj /CN=localhost -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1

CWIST_WORKERS="$WORKERS" CWIST_HTTPS_IDLE_TIMEOUT_MS=600000 \
    ./rss_gate_server "$PORT" "$WORK/cert.pem" "$WORK/key.pem" >c1m_server.log 2>&1 &
MASTER=$!
CLIENT_PIDS=""

cleanup() {
    for c in ${CLIENT_PIDS:-}; do kill "$c" 2>/dev/null; done
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
curl -sk -o /dev/null -m 2 "$URL" || { echo "c1m: server did not come up" >&2; cat c1m_server.log >&2; exit 2; }

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
echo "c1m: idle_rss=${idle_rss}KiB"

CLIENT_PIDS=""
i=0
while [ "$i" -lt "$CLIENTS" ]; do
    n=1; hosts=""
    while [ "$n" -le "$HOSTS_PER_CLIENT" ]; do
        hosts="${hosts:+$hosts,}127.0.$((i + 1)).$n"
        n=$((n + 1))
    done
    C100K_READY_FILE="$WORK/ready.$i" C100K_DONE_FILE="$WORK/done.$i" \
        C100K_LINGER_S="$LINGER_S" CLIENT_BIN="$CLIENT_BIN" \
        MALLOC_ARENA_MAX=4 MALLOC_MMAP_THRESHOLD_=16384 MALLOC_TRIM_THRESHOLD_=4194304 \
        sh -c '"$CLIENT_BIN" --hosts "$1" --port "$2" \
        --conns "$3" --threads "$4" --inflight "$5" \
        --hold "$6" --rounds "$7" --timeout "$8" \
        >"$9" 2>&1; echo "client_exit=$?" >>"$9"' \
        _ "$hosts" "$PORT" "$CONNS_PER_CLIENT" "$THREADS" "$INFLIGHT" \
        "$HOLD_S" "$ROUNDS" "$RAMP_MAX_S" "c1m_client.$i.log" &
    CLIENT_PIDS="$CLIENT_PIDS $!"
    i=$((i + 1))
    [ "$STAGGER_S" -gt 0 ] && [ "$i" -lt "$CLIENTS" ] && sleep "$STAGGER_S"
done

peak=0
while :; do
    alive=0
    for c in $CLIENT_PIDS; do kill -0 "$c" 2>/dev/null && alive=$((alive + 1)); done
    cur=$(total_rss)
    [ "$cur" -gt "$peak" ] && peak=$cur
    avail=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    echo "c1m: rss=${cur}KiB peak=${peak}KiB clients_alive=$alive mem_available=${avail}KiB"
    [ "$alive" -eq 0 ] && break
    sleep 2
done

fail=0
total_established=0
max_ramp=0
for i in $(seq 0 $((CLIENTS - 1))); do
    log="c1m_client.$i.log"
    val() { awk -F= -v k="$1" '$1==k{print $2}' "$log"; }
    est=$(val established); drop=$(val dropped_idle)
    ramp=$(val ramp_seconds)
    echo "c1m: client$i established=$est dropped_idle=$drop ramp=${ramp}s"
    total_established=$((total_established + ${est:-0}))
    awk -v r="${ramp:-0}" -v m="$max_ramp" 'BEGIN{exit !(r>m)}' && max_ramp=${ramp:-0}
    [ "${drop:-1}" != 0 ] && fail=1
    r=1
    while [ "$r" -le "$ROUNDS" ]; do
        echo "c1m: client$i round$r=$(val "round${r}_ok")ok/$(val "round${r}_fail")fail@$(val "round${r}_rps")rps"
        [ "$(val "round${r}_fail")" != 0 ] && fail=1
        r=$((r + 1))
    done
done

per_conn=$(( (peak - idle_rss) * 1024 / (CONNS_PER_CLIENT * CLIENTS) ))
echo "c1m: SUMMARY conns=$((CONNS_PER_CLIENT * CLIENTS)) established=$total_established max_ramp=${max_ramp}s peak_rss=${peak}KiB per_conn=${per_conn}B fail=$fail"
[ "$total_established" -eq $((CONNS_PER_CLIENT * CLIENTS)) ] || fail=1
kill -TERM "$MASTER" 2>/dev/null; wait "$MASTER" 2>/dev/null; MASTER=""
exit $fail
