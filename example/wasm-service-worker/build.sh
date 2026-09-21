#!/bin/sh
# Build the CWIST WASM service-worker demo (app.js + app.wasm).
#
# Prereq: the CWIST WASM archive, built at the repo root with `make wasm`
# (this script runs that for you if the archive is missing).
#
# Usage (from example/wasm-service-worker/):
#   ../../libcwist_wasm.a already built:   ./build.sh
#   from scratch:                          make -C ../.. wasm && ./build.sh
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
EMCC=${EMCC:-emcc}

if [ ! -f "$ROOT/libcwist_wasm.a" ]; then
    echo "libcwist_wasm.a not found - building it (make wasm)"
    make -C "$ROOT" wasm
fi

# MODULARIZE makes app.js export a factory (createCwistAppModule) instead of
# populating a bare global Module, so sw.js can inject the session secret
# before any dispatch; sw.js loads the glue with importScripts().
#
# Exports (see wasm/npm/README.md): the standard CWIST WASM entry points plus
# malloc/free and the HEAP views the dispatch glue reads. _main must be
# exported or the linker dead-code-eliminates main() and the app - routes,
# migrations - is never created.
# Same minimal include set as the Makefile's WASM_INCLUDE_PATHS: host
# pkg-config -I paths must not leak into the emscripten sysroot.
"$EMCC" -std=c17 -O2 \
    -I"$ROOT/include" -I"$ROOT/lib" -I"$ROOT/lib/cjson" -I"$ROOT/lib/boringssl/include" \
    -I"$ROOT/lib/libttak/include" -I"$ROOT/lib/sqlite3" \
    -o "$DIR/app.js" "$DIR/app.c" "$ROOT/libcwist_wasm.a" \
    -sEXPORTED_FUNCTIONS=_main,_cwist_wasm_dispatch,_cwist_wasm_dispose,_cwist_wasm_use_session,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
    -sMODULARIZE -sEXPORT_NAME=createCwistAppModule

echo "built $DIR/app.js + $DIR/app.wasm"
