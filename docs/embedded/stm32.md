# Bare-metal STM32 with CWIST

CWIST supports a socket-free Cortex-M path for small firmware that drives
`cwist_app_dispatch_memory()` or the raw-lwIP adapter. It uses the same router,
middleware, handlers, JSON helpers, and HTTP/1.1 parser as hosted builds; it
does not port `cwist_app_listen()`, pthreads, epoll, or the C1M reactor.

## Supported targets and validation

- **Reference integration:** NUCLEO-H743ZI2 (STM32H743, Cortex-M7, 2 MB flash,
  1 MB SRAM, RMII Ethernet, TRNG). `example/stm32/` contains the application,
  HAL port hooks, lwIP options, CubeMX integration, and flashing instructions.
- **Validated board:** STM32F769I-DISCO (STM32F769NI, Cortex-M7, 2 MB flash,
  512 KB SRAM, LAN8742 RMII PHY). `example/stm32f769/` is a self-contained
  no-CubeMX build (upstream STM32CubeF7 HAL + CWIST bare-metal archive)
  serving an HTML wire inspector over real Ethernet, verified on hardware
  with ping, parallel/keep-alive `curl`, and browser POSTs. See its README
  for the rev-A RMII erratum workaround and the D-cache constraint.
- **CI target:** Cortex-M7 Thumb-2 firmware built with `arm-none-eabi-gcc` and
  run with `qemu-arm` semihosting. This validates compiled instructions and the
  HTTP/lwIP/TLS logic, but not a board's NVIC, PHY, clock tree, or RNG.
- **Sizing candidates:** STM32H7 first, then F767/F429-class parts with enough
  SRAM. Cortex-M4F and F429/F767 builds are not validated targets yet. Real
  hardware acceptance remains a manual step; do not infer it from QEMU success.

Low-end F0/F1/L0 parts and parts below roughly 128 KB SRAM are outside this
epic. Plain HTTP is the practical starting point for smaller parts; HTTPS is
intended for H7-class memory budgets.

## Build the firmware inputs

Install an ARM EABI GCC toolchain with newlib-nano, then run:

```sh
make CROSS_COMPILE=arm-none-eabi- CWIST_PLATFORM=baremetal CWIST_TINY=1 amalgamation fat-dist
make baremetal-dist-check
```

The first command produces both:

- `dist/amalgamation/cwist.c` and `dist/amalgamation/include/`: one C file and
  its headers for CubeIDE/CMake.
- `dist/cwist-fat/lib/libcwist_baremetal.a` and `dist/cwist-fat/include/`: the
  Cortex-M archive and headers.

`baremetal-dist-check` repeats the build in the bare-metal output directory,
cross-compiles the generated amalgamation, and lists the archive contents. Use
only one of the two library forms in a firmware image.

The lower-level targets remain available:

```sh
make baremetal             # libcwist_baremetal.a using BAREMETAL_CROSS
make baremetal-smoke       # four qemu-arm firmware runs
make baremetal-size-gate   # smoke plus size and allocation budgets
```

`BAREMETAL_CROSS` and `BAREMETAL_ARCH` can select another prefix or Cortex-M
FPU/CPU profile. Do not mix objects built for different ARM ABIs.

## Port hooks and runtime contract

Firmware provides these link-time symbols from `cwist/sys/port.h`:

```c
uint64_t cwist_port_monotonic_ns(void);
int cwist_port_entropy(void *buf, size_t len);
```

`cwist_port_entropy()` is **required**. It must read the MCU's RNG peripheral
and return nonzero on failure. Constant seeds and clock-plus-`rand()` fallbacks
are not accepted: entropy protects hash-flooding seeds, session IDs, and TLS.

The monotonic hook normally uses SysTick, DWT, or a timer. Logging is
synchronous through newlib's `_write`, which the example sends to USART3.
There is no background logger, scheduler, GC thread, or RTOS lock hook. All
lwIP adapter calls and callbacks must run in lwIP's core context.

A wall clock is optional. Supply newlib's `time()`/`_gettimeofday` when an RTC
is available; otherwise CWIST omits the HTTP `Date` header as RFC 9110 requires.
Compiled-in assets use the existing assets table rather than filesystem access.

## HTTP over raw lwIP

Compile `src/sys/port/lwip.c` with the firmware's own `lwipopts.h`, initialize
lwIP, and then create the server in its core context:

```c
cwist_lwip_config cfg;
cwist_lwip_config_init(&cfg);
cfg.max_conns = 4;
cfg.max_header_bytes = 4096;
cfg.max_body_bytes = 1024;
cwist_lwip_server *server = cwist_lwip_listen(app, &cfg);
```

The adapter supports Content-Length framing, keep-alive, ordered pipelining,
`Expect: 100-continue`, HEAD, idle timeouts, connection limits, and 400/413/431
responses. Request bodies using Transfer-Encoding receive 501. Each connection
has one exchange in flight; back-pressure uses `tcp_write`, `tcp_sent`, and
`tcp_poll`.

See `example/stm32/README.md` for CubeMX settings and the main-loop integration.

## Memory sizing

The numbers below are for the complete **test firmware**, including the loopback
client and test buffers. They are sizing ceilings, not the cost of CWIST alone.
Exact current budgets and measurements are recorded in
`scripts/ci/baremetal-budget.txt`.

| Firmware | Flash | Static RAM | Heap high-water | Stack | Allocations/request |
|---|---:|---:|---:|---:|---:|
| hello | 45,156 B | 1,840 B | 23,308 B | 8,352 B | 1 |
| hello-rtc | 45,132 B | 1,852 B | 23,312 B | 8,800 B | 1 |
| hello-lwip | 75,328 B | 62,432 B | 71,212 B | 8,316 B | 0 |
| hello-tls | 138,308 B | 75,608 B | 127,072 B | 8,340 B | 0 |

The one in-memory allocation is the response buffer returned to the caller by
`cwist_app_dispatch_memory()`. The lwIP and TLS measurements are taken after
warm-up on an established connection; handlers that allocate, such as a cJSON
handler, add their own allocations.

### Request arenas

Request and response objects use two arenas. Non-hosted builds recycle arena
structs and default-sized buffers after first use, so steady-state requests do
not repeatedly fragment the heap. The default reserves two 8 KB generations;
four buffers and four structs are cached at most.

For smaller parts, override the common-case capacity at build time:

```sh
make baremetal BAREMETAL_EXTRA_CFLAGS=-DCWIST_ARENA_DEFAULT_GENERATION_BYTES=4096
```

Allocations beyond a generation return `NULL` from the arena and use the
caller's heap fallback. Reducing the arena does not change request limits and
does not eliminate handler or returned-response allocations.

### Connections and responses

On 32-bit ARM, the adapter control block is about 60 bytes per plaintext
connection, plus:

- receive storage, grown up to `max_header_bytes + max_body_bytes + 1`;
- response storage, grown to the largest serialized response and retained for
  later keep-alive responses;
- lwIP's own PCB, pbuf, segment, and timer pools.

Budget response storage for the largest response, not the average one. Limit
`max_conns`, body sizes, and handler output together. Reserve at least 16 KB of
main stack for the current serializer and parser, and validate that margin on
the final application.

The TLS adapter additionally holds an mbedTLS session, ciphertext input that
can grow to `MBEDTLS_SSL_IN_CONTENT_LEN + 2048`, and the same HTTP buffers. The
test profile uses about 25 KB for one server-side TLS session before adapter
buffers. `MBEDTLS_SSL_IN_CONTENT_LEN` remains 16 KB because arbitrary clients
can send full-size records.

### lwIP pools

The example starts with these values; tune them with linker maps and runtime
pool statistics enabled in a debug build:

| Option | Starting value | Effect |
|---|---:|---|
| `MEM_SIZE` | 16 KB | lwIP's internal heap, separate from CWIST/newlib heap |
| `MEMP_NUM_TCP_PCB` | 6 | listener plus up to four connections and margin |
| `MEMP_NUM_TCP_SEG` | 16 | queued transmit segments |
| `PBUF_POOL_SIZE` | 12 | packet buffers for RX/TX and the network driver |
| `TCP_WND` | `4 * TCP_MSS` | receive window and in-flight input bound |
| `TCP_SND_BUF` | `2 * TCP_MSS` | transmit back-pressure; exercises streaming correctly |

`NO_SYS=1`, `LWIP_NETCONN=0`, and `LWIP_SOCKET=0` are required for the supplied
raw-only example. `MEMP_NUM_TCP_PCB` must cover the listener and every accepted
connection. Increase pools when enabling DHCP, DNS, or extra netifs.

## HTTPS

Build the adapter with `CWIST_LWIP_TLS`, link mbedTLS 3.6, and set `cfg.tls` to
a context returned by `cwist_lwip_tls_create()`. The current profile is a TLS
1.2 server using ECDHE-ECDSA P-256 and AES-128-GCM-SHA256. Certificate/key
parsing and DRBG seeding fail closed; a TLS configuration never falls back to
plaintext.

MCU CRYP/HASH/PKA acceleration is a firmware-side `MBEDTLS_*_ALT` choice, not a
CWIST hook. TLS 1.3, session tickets, and resumption are not included. Review
record-buffer sizes before moving from the test profile to a product.

## Limitations

- No `cwist_app_listen()`, sockets, worker pool, RTOS/netconn variant, or W5500
  adapter in this epic.
- No HTTP/2, HTTP/3, WebSocket upgrades, deferred `cwist_async` responses, or
  filesystem streaming through the bare-metal adapter.
- No request-chunk reassembly, DB/SQLite, NATS/Redis, or OTel runtime in the
  bare-metal archive.
- No `qemu-system-arm` board model or physical-board pass is claimed. Follow
  `example/stm32/README.md` for the required hardware smoke and network test.
