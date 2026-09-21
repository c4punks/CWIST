#!/bin/sh
# Build the wasip2 edge-KV demo (app.wasm).
#
# Prereq: the wasip2 archive, built at the repo root with
# `make libcwist_wasip2.a` (this script runs that for you if missing).
#
#   WASI_SDK=/path/to/wasi-sdk ./build.sh
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
WASI_SDK=${WASI_SDK:-$HOME/toolchains/wasi-sdk-25.0-x86_64-linux}

if [ ! -f "$ROOT/libcwist_wasip2.a" ]; then
    echo "libcwist_wasip2.a not found - building it (make libcwist_wasip2.a)"
    make -C "$ROOT" libcwist_wasip2.a
fi

# Same flags/include set as the Makefile's wasip2 section.
# The socket request path needs more stack than wasm-ld's 64KB default —
# see WASIP2_STACK_BYTES in the root Makefile.
"$WASI_SDK/bin/clang" --target=wasm32-wasip2 -std=c17 -O2 -Wall -fvisibility=hidden \
    -D_WASI_EMULATED_GETPID \
    -I"$ROOT/include" -I"$ROOT/lib" -I"$ROOT/lib/cjson" -I"$ROOT/lib/boringssl/include" \
    -I"$ROOT/lib/libttak/include" -I"$ROOT/lib/sqlite3" \
    -o "$DIR/app.wasm" "$DIR/app.c" "$ROOT/libcwist_wasip2.a" \
    -lwasi-emulated-pthread -lwasi-emulated-getpid \
    -Wl,--gc-sections -Wl,--allow-undefined -Wl,-z,stack-size="${WASIP2_STACK_BYTES:-1048576}"

echo "built $DIR/app.wasm"
