#!/usr/bin/env bash
# Link example/micro/01-hello against a `make fat-dist` bundle the way an
# embedded user does (bundle + app source + system libraries only), run it and
# check that it answers.  Used by .github/workflows/release-assets.yml on every
# bundle before it is attached to a release.
#
# Usage: scripts/ci/bundle_smoke.sh <bundle-dir>
#
# Environment:
#   CC        compiler for the app (default: cc)
#   RUN       prefix for running the binary, e.g. "qemu-aarch64 -L /usr/aarch64-linux-gnu"
#   PORT      port example/micro/01-hello listens on (default: 8080)
#   APP_OBJS  objects linked ahead of the archive, e.g. the amalgamated cwist.o
#
# pkg-config is consulted for the system libraries, so a cross run sets
# PKG_CONFIG_LIBDIR to the target's .pc directory.
set -euo pipefail

bundle=$(cd "$1" && pwd)
repo=$(cd "$(dirname "$0")/../.." && pwd)
cc=${CC:-cc}
port=${PORT:-8080}

case "$(uname -s)" in
    Darwin) cxx_lib=-lc++ ;;
    *) cxx_lib="-lstdc++ -ldl" ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$repo/example/micro/01-hello/main.c" "$work/"
cd "$work"

# shellcheck disable=SC2046,SC2086
"$cc" -O2 -std=gnu17 -I"$bundle/include" -o hello main.c ${APP_OBJS:-} "$bundle/lib/libcwist.a" \
    $(pkg-config --libs libcurl libnghttp2 libbrotlienc libbrotlicommon libbrotlidec libzstd) \
    -pthread -lm $cxx_lib -lz

# shellcheck disable=SC2086
${RUN:-} ./hello &
pid=$!
out=
for _ in $(seq 1 30); do
    out=$(curl -s -m 2 "http://127.0.0.1:${port}/") && [ -n "$out" ] && break
    sleep 1
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

if [ "$out" != "Hello, CWIST!" ]; then
    echo "bundle_smoke: hello did not answer (got '$out')" >&2
    exit 1
fi
echo "bundle_smoke: $bundle OK"
