#!/usr/bin/env bash
# TLS performance gates for CWIST 3.9 (issues #306 / #307).
#
# Measures five gate metrics against a bench server built from the current
# tree and prints them as a single JSON row (metrics + runner_hw) to
# https-gates-result.json in the current directory. Gate evaluation --
# absolute backstops plus same-runner-CPU relative checks -- lives in
# https_gates_eval.py; this script only produces the row.
#
# Gate inventory (measured baselines on a 12-core Ryzen 5600X, loopback,
# ECDSA P-256 self-signed cert generated below):
#   1. tls_rtt_p50_ms      sequential TLS client WITHOUT TCP_NODELAY,
#                          first-response RTT p50 over 200 fresh connections.
#                          Regression test for the #307 Nagle/delayed-ACK
#                          stall (43 ms before the fix, ~0.2 ms after).
#                          Backstop: p50 <= 5 ms.
#   2. https_churn_rps     ab -n 6000 -c 32, new connection per request.
#                          ~600/s before #307, ~1650/s after. Backstop >= 1200.
#   3. https_keepalive_rps wrk -t4 -c100 -d10s. ~167-190k/s. Backstop >= 130k.
#   4. https_big_Bps       wrk -t4 -c32 -d10s on a 1 MiB body. ~3.9-4.5GB/s.
#                          Backstop >= 3 GB/s.
#   5. http_* controls     same workloads plaintext; detect collateral damage
#                          to the plain path. The https/http keep-alive ratio
#                          (~0.65) is recorded as a signal, not gated.
set -eu

OUT=${1:-https-gates-result.json}

# --- cert: self-signed, generated at runtime (no example/ certs) -----------
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
  -keyout /tmp/https_gates_key.pem -out /tmp/https_gates_cert.pem \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null

# --- bench server -----------------------------------------------------------
cat << 'EOF' > /tmp/https_gates_server.c
#include <cwist/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char big_body[1024 * 1024];
static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"msg\":\"hello\"}");
}
static void big(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, big_body, sizeof(big_body));
}

int main(int argc, char **argv) {
    for (size_t i = 0; i < sizeof(big_body); i++) big_body[i] = (char)('a' + (i % 26));
    if (argc < 3) { fprintf(stderr, "usage: %s http|https port\n", argv[0]); return 1; }
    cwist_app *app = cwist_app_create();
    if (strcmp(argv[1], "https") == 0)
        cwist_app_use_https(app, "/tmp/https_gates_cert.pem", "/tmp/https_gates_key.pem");
    cwist_app_get(app, "/", hello);
    cwist_app_get(app, "/big", big);
    cwist_app_listen(app, atoi(argv[2]));
    cwist_app_destroy(app);
    return 0;
}
EOF

# --- gate 1 client: sequential TLS, NO TCP_NODELAY --------------------------
cat << 'EOF' > /tmp/https_gates_rtt.c
/* Sequential TLS first-response RTT probe. Deliberately does NOT set
 * TCP_NODELAY: this is the client shape that exposed the #307 quickack
 * stall (request held behind Nagle waiting for the server's delayed ACK
 * of the TLS Finished). Prints "p50_ms=<value>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <openssl/ssl.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s ip port iters\n", argv[0]); return 2; }
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    int iters = atoi(argv[3]);
    double *rtt = malloc(sizeof(double) * iters);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* self-signed cert */
    for (int i = 0; i < iters; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(atoi(argv[2]));
        inet_pton(AF_INET, argv[1], &sa.sin_addr);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) return 1;
        /* Intentionally no TCP_NODELAY here -- see file header. */
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) return 1;
        double t0 = now();
        SSL_write(ssl, req, (int)strlen(req));
        char buf[4096];
        if (SSL_read(ssl, buf, sizeof(buf)) <= 0) return 1;
        rtt[i] = now() - t0;
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
    }
    qsort(rtt, iters, sizeof(double), cmpd);
    printf("p50_ms=%.3f p90_ms=%.3f\n", rtt[iters / 2] * 1e3, rtt[(int)(iters * 0.9)] * 1e3);
    return 0;
}
EOF

# --- build ------------------------------------------------------------------
make -j"$(nproc)" libcwist.a > /tmp/https_gates_build.log 2>&1 || {
    tail -30 /tmp/https_gates_build.log; exit 1; }

CWIST_LIBS="libcwist.a lib/cnats/build/lib/libnats_static.a lib/libttak/lib/libttak.a \
  lib/cjson/libcjson.a lib/uriparser/build/liburiparser.a \
  lib/lsquic/build/src/liblsquic/liblsquic.a lib/boringssl/build/libssl.a \
  lib/boringssl/build/libcrypto.a -lcurl -lnghttp2 -lbrotlienc -lbrotlicommon \
  -lbrotlidec -pthread -ldl -lm -lstdc++ -lz -lzstd"

gcc -O2 -I./include -I./lib -I./lib/libttak/include -I./lib/cjson -I./lib/sqlite3 \
  -I./lib/uriparser/include -I./lib/cnats/src -I./lib/boringssl/include \
  -I./lib/lsquic/include -I./lib/multipart-parser-c \
  -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE \
  -o /tmp/https_gates_server /tmp/https_gates_server.c $CWIST_LIBS

gcc -O2 -o /tmp/https_gates_rtt /tmp/https_gates_rtt.c -lssl -lcrypto

# --- run servers ------------------------------------------------------------
/tmp/https_gates_server https 18443 > /tmp/https_gates_srv.log 2>&1 &
HTTPS_PID=$!
/tmp/https_gates_server http 18080 > /tmp/https_gates_srv_http.log 2>&1 &
HTTP_PID=$!
trap 'kill -TERM $HTTPS_PID $HTTP_PID 2>/dev/null || true' EXIT

for i in $(seq 1 30); do
  curl -sfk https://127.0.0.1:18443/ > /dev/null 2>&1 && curl -sf http://127.0.0.1:18080/ > /dev/null 2>&1 && break
  sleep 1
done

# --- measurements -----------------------------------------------------------
# Gate 1: TLS request-phase RTT, no-TCP_NODELAY sequential client.
RTT_OUT=$(/tmp/https_gates_rtt 127.0.0.1 18443 200)
RTT_P50=$(echo "$RTT_OUT" | sed -n 's/^p50_ms=\([0-9.]*\).*/\1/p')
RTT_P90=$(echo "$RTT_OUT" | sed -n 's/.*p90_ms=\([0-9.]*\)/\1/p')

# Gate 2: connection churn (ab, new connection per request).
HTTPS_CHURN=$(ab -n 6000 -c 32 https://127.0.0.1:18443/ 2>/dev/null | sed -n 's/^Requests per second:\s*\([0-9.]*\).*/\1/p')
HTTP_CHURN=$(ab -n 6000 -c 32 http://127.0.0.1:18080/ 2>/dev/null | sed -n 's/^Requests per second:\s*\([0-9.]*\).*/\1/p')

# Warmup for the keep-alive gates (results discarded).
wrk -t4 -c100 -d5s https://127.0.0.1:18443/ > /dev/null 2>&1 || true
wrk -t4 -c100 -d5s http://127.0.0.1:18080/ > /dev/null 2>&1 || true

# Gate 3: keep-alive throughput.
HTTPS_KEEP=$(wrk -t4 -c100 -d10s https://127.0.0.1:18443/ 2>/dev/null | sed -n 's/^Requests\/sec:\s*\([0-9.]*\).*/\1/p')
HTTP_KEEP=$(wrk -t4 -c100 -d10s http://127.0.0.1:18080/ 2>/dev/null | sed -n 's/^Requests\/sec:\s*\([0-9.]*\).*/\1/p')

# Gate 4: 1 MiB transfer throughput.
HTTPS_BIG=$(wrk -t4 -c32 -d10s https://127.0.0.1:18443/big 2>/dev/null | sed -n 's/^Transfer\/sec:\s*\([0-9.]*\)GB.*/\1/p')
[ -n "$HTTPS_BIG" ] || HTTPS_BIG=$(wrk -t4 -c32 -d10s https://127.0.0.1:18443/big 2>/dev/null | sed -n 's/^Transfer\/sec:\s*\([0-9.]*\)MB.*/0\1/p')
HTTP_BIG=$(wrk -t4 -c32 -d10s http://127.0.0.1:18080/big 2>/dev/null | sed -n 's/^Transfer\/sec:\s*\([0-9.]*\)GB.*/\1/p')

RUNNER_HW="$(nproc) vCPU | $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"

# --- row --------------------------------------------------------------------
python3 - "$OUT" "$RTT_P50" "$RTT_P90" "$HTTPS_CHURN" "$HTTP_CHURN" \
  "$HTTPS_KEEP" "$HTTP_KEEP" "$HTTPS_BIG" "$HTTP_BIG" "$RUNNER_HW" <<'EOF'
import json, sys, time

(out, rtt_p50, rtt_p90, https_churn, http_churn, https_keep, http_keep,
 https_big, http_big, runner_hw) = sys.argv[1:11]

row = {
    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "runner_hw": runner_hw,
    "tls_rtt_p50_ms": float(rtt_p50),
    "tls_rtt_p90_ms": float(rtt_p90),
    "https_churn_rps": float(https_churn),
    "http_churn_rps": float(http_churn),
    "https_keepalive_rps": float(https_keep),
    "http_keepalive_rps": float(http_keep),
    "https_keepalive_ratio": round(float(https_keep) / float(http_keep), 3),
    "https_big_gbps": float(https_big),
    "http_big_gbps": float(http_big),
}
with open(out, "w") as f:
    json.dump(row, f, indent=2)
    f.write("\n")
print(json.dumps(row, indent=2))
EOF
