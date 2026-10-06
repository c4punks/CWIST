#!/usr/bin/env bash
# Serve one of the language-binding hello examples (example/rust-hello,
# example/zig-hello) and check real HTTP responses from it (ROADMAP.md, v3.8
# Phase 2: "the Rust example serves requests"; issue #36).
#
# Usage: scripts/ci/hello_smoke.sh <hello binary> <expected X-Powered-By>
#
# Starts the server, waits until it answers, checks the routes and the Rust
# middleware header, then stops it with SIGTERM and requires a clean exit.
# The server is killed on every exit path, so a failure never leaves it
# running.
set -euo pipefail

bin=${1:?usage: hello_smoke.sh <hello binary> <expected X-Powered-By>}
powered_by=${2:?usage: hello_smoke.sh <hello binary> <expected X-Powered-By>}
port=8080 # fixed in the hello examples
base="http://127.0.0.1:$port"

work=$(mktemp -d)
log="$work/server.log"
hdr="$work/headers"
body="$work/body"

"$bin" >"$log" 2>&1 &
pid=$!

cleanup() {
    kill -KILL "$pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "hello smoke: $*" >&2
    echo "--- server output ---" >&2
    cat "$log" >&2 || true
    exit 1
}

# True while the server process exists and has not exited (an exited child
# that is not reaped yet is a zombie, which kill -0 still accepts).
running() {
    local state
    state=$(ps -o stat= -p "$pid" 2>/dev/null || true)
    case "$state" in
        "" | Z*) return 1 ;;
    esac
    return 0
}

# Ready once / answers at all; stop early if the process dies.
ready=0
for _ in $(seq 1 150); do
    running || fail "server exited before it was ready"
    if curl -s --max-time 2 -o /dev/null "$base/"; then
        ready=1
        break
    fi
    sleep 0.2
done
[ "$ready" = 1 ] || fail "server did not answer within 30 s"

# check <path> <status> <body>: an empty body skips the body comparison.
check() {
    local path=$1 want_status=$2 want_body=$3 status got
    status=$(curl -sS --max-time 5 -D "$hdr" -o "$body" -w '%{http_code}' "$base$path") ||
        fail "GET $path: request failed"
    [ "$status" = "$want_status" ] || fail "GET $path: status $status, want $want_status"
    if [ -n "$want_body" ]; then
        got=$(cat "$body")
        [ "$got" = "$want_body" ] || fail "GET $path: body '$got', want '$want_body'"
    fi
}

check / 200 "Hello, World!"
grep -qi "^X-Powered-By: $powered_by" "$hdr" ||
    fail "GET /: no X-Powered-By: $powered_by header"
check /users/42 200 "user 42"
check /no/such/route 404 ""

# Graceful shutdown: SIGTERM makes the app's listen return and main exit 0.
# A server that does not stop within 20 s is a failure, not a stuck job.
kill -TERM "$pid"
for _ in $(seq 1 200); do
    running || break
    sleep 0.1
done
running && fail "server still running 20 s after SIGTERM"
rc=0
wait "$pid" || rc=$?
[ "$rc" -eq 0 ] || fail "server exited with status $rc after SIGTERM"

echo "--- server output ---"
cat "$log"
echo "hello smoke: OK"
