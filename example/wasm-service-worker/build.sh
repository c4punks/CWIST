#!/bin/sh
# Build the CWIST WASM service-worker demo: the app as a cwist-guest
# component, its node and browser transpiles, and the servable site in
# build/site/ (index.html, the bundled sw.js and the core wasm modules).
#
# Prereqs: wasi-sdk (WASI_SDK, default ~/toolchains/wasi-sdk-25.0-x86_64-linux),
# wasm-tools, wit-bindgen, node/npm. See docs/api/wasm-component.md.
#
# Usage: ./build.sh            (same as make -C ../.. wasm-sw-example)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec make -C "$ROOT" wasm-sw-example "$@"
