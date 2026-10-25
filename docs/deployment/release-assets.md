# Prebuilt release assets

Every published GitHub release carries, next to the source tarball
`cwist-<version>.tar.gz`, the prebuilt targets that CI builds and verifies.
`.github/workflows/release-assets.yml` builds them from the release tag when
the release is published, smoke-tests each one, and attaches them together
with a checksum file. `<version>` is the tag without the leading `v` (`4`
for `v4`, `4.1` for `v4.1`).

| Asset | Contents | Built on |
| --- | --- | --- |
| `cwist-<version>-fat-linux-x86_64.tar.gz` | `make fat-dist` bundle, BoringSSL | ubuntu-22.04, gcc |
| `cwist-<version>-fat-linux-x86_64-mbedtls.tar.gz` | `make CWIST_TLS=mbedtls CWIST_HTTP3=0 fat-dist` bundle | ubuntu-22.04, gcc |
| `cwist-<version>-fat-linux-aarch64.tar.gz` | `make CROSS_COMPILE=aarch64-linux-gnu- fat-dist` bundle | ubuntu-22.04, cross gcc |
| `cwist-<version>-fat-macos-arm64.tar.gz` | `make fat-dist` bundle (libtool merge) | macos-15, Apple clang |
| `cwist-<version>-amalgamation-linux.tar.gz` | `make amalgamation`: `cwist.c` plus `include/` | ubuntu-22.04 |
| `cwist-<version>-amalgamation-linux-tiny.tar.gz` | `make CWIST_TINY=1 amalgamation` | ubuntu-22.04 |
| `cwist-wasm-<version>.tgz` | `make wasm-dist`: the cwist-wasm npm package | ubuntu-latest, Emscripten 5.0.0 |
| `cwist-<version>-assets-SHA256SUMS` | sha256 of every asset above | |

The source tarball is not built by this workflow; the maintainer uploads it
and the Homebrew formula pins its checksum.

## Using a fat bundle

A bundle unpacks to `cwist-<version>-fat-<target>/` with `lib/libcwist.a`
(CWIST plus every vendored archive) and `include/`. Only system libraries stay
external:

```sh
cc -O2 -std=gnu17 -Icwist-4-fat-linux-x86_64/include -o app main.c \
  cwist-4-fat-linux-x86_64/lib/libcwist.a \
  $(pkg-config --libs libcurl libnghttp2 libbrotlienc libbrotlicommon libbrotlidec libzstd) \
  -pthread -ldl -lm -lstdc++ -lz
```

On macOS replace `-ldl -lstdc++` with `-lc++`. This is the link line
`scripts/ci/bundle_smoke.sh` runs against every bundle before it is attached.

- **CPU baseline.** Bundles are built with `CWIST_PORTABLE=1`: no
  `-march=native`, so they run on any CPU of the target architecture. Build
  from source for host-tuned code.
- **glibc.** The Linux bundles are built on Ubuntu 22.04 (glibc 2.35) and do
  not reference newer glibc symbols, so they link on that release and later.
- **No armhf bundle.** libttak does not build for 32-bit ARM yet
  (`src/accel/accel_cpu.c` requires `__int128`).

## Using the amalgamation

The amalgamation is the CWIST code only; the vendored libraries still link
from a fat bundle of the same configuration (the default amalgamation with
`fat-linux-x86_64`, for example). The compile line is one `-I` plus the file:

```sh
gcc -O2 -std=gnu17 -Ibundle/include -pthread -c bundle/cwist.c -o cwist.o
```

It carries the Linux platform defines of the build, hence the `linux` name.

## Re-running

The workflow can be started by hand (Actions, "Release assets", Run workflow)
with an existing tag, for example to attach assets to a release published
before the workflow existed. Assets are uploaded only after every build and
smoke test has passed, and a re-run replaces the assets this workflow owns.
