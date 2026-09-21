#!/bin/sh
# Prove the host-KV round trip: POST persists the blob, a restarted instance
# serves the same rows (get -> put -> restart -> get).
#
#   WASMTIME=wasmtime ./smoke.sh
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
WASMTIME=${WASMTIME:-wasmtime}
PORT=${PORT:-18100}
LOG="$DIR/smoke.log"

[ -f "$DIR/app.wasm" ] || "$DIR/build.sh"
mkdir -p "$DIR/kv"
rm -f "$DIR/kv/cwist.db" # clean slate

WPID=
cleanup() { [ -n "$WPID" ] && kill -9 "$WPID" 2>/dev/null || true; }
trap cleanup EXIT

start() {
    # exec keeps $! pointed at wasmtime itself; the logfile redirect keeps the
    # guest from holding this script's stdout open after a kill.
    (cd "$DIR" && exec "$WASMTIME" run -S preview2=y -S tcp=y -S inherit-network=y \
        --dir . --env CWIST_C1M_MODE=0 app.wasm) >"$LOG" 2>&1 &
    WPID=$!
    i=0
    while [ $i -lt 30 ]; do
        sleep 0.3
        curl -s -m 2 "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && return 0
        i=$((i + 1))
    done
    echo "smoke: instance did not come up" >&2
    return 1
}

stop() { kill -9 "$WPID" 2>/dev/null || true; WPID=; sleep 0.5; }

start
curl -s -m 2 -X POST "http://127.0.0.1:$PORT/items" \
    -H 'Content-Type: application/json' -d '{"name":"edge","qty":7}' | grep -q '"ok":true'
[ -s "$DIR/kv/cwist.db" ] || { echo "smoke: blob not persisted" >&2; exit 1; }
stop

start # restart: the blob must come back through cwist_db_open_memory
body=$(curl -s -m 2 "http://127.0.0.1:$PORT/items")
stop

echo "$body" | grep -q '"name":"edge"' || { echo "smoke: row lost across restart: $body" >&2; exit 1; }
echo "wasip2-kv smoke: PASS (blob persisted and reloaded across restart)"
