#!/usr/bin/env bash
# STM32 Phase 0 spike (issue #447): build the socket-free CWIST core for
# Cortex-M7 with arm-none-eabi-gcc, link a hello firmware that is driven only
# through cwist_app_dispatch_memory(), report its size, and run it under
# qemu-arm (user mode, -cpu cortex-m7) with semihosting output.
#
# Needs: arm-none-eabi-gcc (newlib-nano), lib/cjson, lib/libttak and
# lib/mbedtls checked out (headers only for mbedtls), optionally qemu-arm.
# usage: tools/stm32-spike/run.sh [iterations]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/tools/stm32-spike
OUT=${OUT:-$ROOT/.stm32-spike}
ITERS=${1:-3}
CC=arm-none-eabi-gcc
ARCH=(-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
# -D__wasi__ selects the existing "no fork/signals/rlimits/threads" guards;
# with no <netdb.h> in the shim, wasi.h also defines CWIST_WASI_NO_SOCKETS.
CFLAGS=(-std=c17 -Os "${ARCH[@]}" --specs=nano.specs -ffunction-sections -fdata-sections
        -fstack-usage -D_GNU_SOURCE -D_XOPEN_SOURCE=700
        -I"$ROOT/include" -I"$ROOT/lib" -I"$ROOT/lib/cjson"
        -I"$ROOT/lib/libttak/include" -I"$ROOT/src/security/tls/shim"
        -I"$ROOT/lib/mbedtls/include" -I"$ROOT/lib/sqlite3"
        -I"$HERE/shim" # after libttak: ttak/compat/pthread.h include_next-s into it
        -include "$HERE/shim/spike_prelude.h" -D__wasi__ -DCWIST_TLS_MBEDTLS)
# The WASIP2 source set minus sqlite/db (CWIST_DB=0), the WASI compat layer
# and the test client.
SRCS=(
src/core/sstring/sstring.c src/core/seq/seq.c src/core/seq/seq_auth.c
src/sys/err/error.c src/net/http/http.c src/net/http/async_tls.c src/net/http/mux.c
src/net/http/query.c src/net/http/cookie.c src/net/http/session.c src/sys/app/app.c
src/sys/app/middleware.c src/sys/app/config.c src/sys/app/logger.c src/sys/app/log_async.c
src/sys/app/shutdown.c src/sys/app/big_dumb_reply.c src/core/siphash/siphash.c
src/core/utils/json_builder.c src/core/utils/json_heal.c src/core/utils/zod.c
src/core/template/template.c src/core/html/builder.c src/core/html/css_composer.c
src/core/html/component.c src/net/http/html_response.c src/sys/app/assets.c
src/core/validation/bind.c src/core/mem/alloc.c src/core/mem/arena.c lib/cjson/cJSON.c
src/sys/metrics/metrics.c src/net/http/writer_fast.c lib/libttak/src/net/lattice.c
lib/libttak/src/shared/shared.c lib/libttak/src/timing/deadline.c src/core/log/log.c
lib/libttak/src/net/mols_control.c src/net/http/async.c src/core/mem/gc.c
lib/libttak/src/mem/epoch.c src/sys/sys_info.c lib/libttak/src/mem/mem.c
lib/libttak/src/mem/fastpath.c lib/libttak/src/mem/owner.c lib/libttak/src/mem/abstract.c
)

rm -rf "$OUT"; mkdir -p "$OUT/obj" "$OUT/log"
fail=0
for s in "${SRCS[@]}"; do
  o="$OUT/obj/${s//\//_}"; o="${o%.c}.o"
  if ! (cd "$ROOT" && $CC "${CFLAGS[@]}" -c "$s" -o "$o") >"$OUT/log/${s//\//_}.log" 2>&1; then
    echo "FAIL $s (see $OUT/log/${s//\//_}.log)"; fail=$((fail+1))
  fi
done
echo "core: $(( ${#SRCS[@]} - fail ))/${#SRCS[@]} translation units compiled"
[ $fail -eq 0 ] || exit 1
arm-none-eabi-ar rcs "$OUT/libcwist_cm7.a" "$OUT"/obj/*.o

# qemu-arm (user mode) loads .data at its VMA and owns the FPU/SP setup.
$CC "${CFLAGS[@]}" -DSPIKE_ITERS="$ITERS" -c "$HERE/fw/hello.c" -o "$OUT/hello.o" || exit 1
$CC -std=c17 -Os "${ARCH[@]}" -ffunction-sections -fdata-sections -fstack-usage \
    -Wno-builtin-declaration-mismatch -DSPIKE_QEMU_USER -c "$HERE/fw/port.c" -o "$OUT/port.o" || exit 1
$CC "${ARCH[@]}" --specs=nano.specs --specs=nosys.specs -nostartfiles -T "$HERE/fw/stm32h743.ld" \
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map="$OUT/hello.map" -o "$OUT/hello.elf" \
    "$OUT/hello.o" "$OUT/port.o" "$OUT/libcwist_cm7.a" -lm 2>"$OUT/link.log" || { cat "$OUT/link.log"; exit 1; }
arm-none-eabi-size -A "$OUT/hello.elf" | grep -vE "debug|comment|ARM.attr|^$"

if command -v qemu-arm >/dev/null; then
  timeout 120 qemu-arm -cpu cortex-m7 "$OUT/hello.elf" | grep -vE "^Host|^$"
  exit "${PIPESTATUS[0]}"
fi
echo "qemu-arm not found: built only"
