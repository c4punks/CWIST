# CWIST on NUCLEO-H743ZI2

This directory contains the application and port files for a CubeMX-generated
NUCLEO-H743ZI2 project. It serves `GET /hello` and `GET /json` over lwIP's raw
TCP API without an RTOS, BSD sockets, or `cwist_app_listen()`.

The CI smoke tests run Cortex-M7 code under `qemu-arm`; they do not emulate the
board's Ethernet, NVIC, or RNG. The final curl check below is a manual hardware
check, not a claimed CI result.

## 1. Generate the CWIST sources

From the repository root, with `arm-none-eabi-gcc` and newlib-nano installed:

```sh
make CROSS_COMPILE=arm-none-eabi- CWIST_PLATFORM=baremetal CWIST_TINY=1 amalgamation fat-dist
make baremetal-dist-check
```

Use either `dist/amalgamation/cwist.c` or
`dist/cwist-fat/lib/libcwist_baremetal.a`, not both. The first command must
produce the bare-metal variants; `make baremetal-dist-check` cross-compiles the
amalgamation and validates the archive.

## 2. Create the CubeMX project

Create a no-RTOS project for the NUCLEO-H743ZI2 and retain the generated HAL,
Ethernet PHY, and lwIP initialization code.

- **SYS:** Serial Wire debug. Enable newlib-nano and a linker heap of at least
  64 KB for plaintext HTTP; use 160 KB when adding the TLS test profile.
  Reserve at least 16 KB for the main stack.
- **Clocks:** use the board's ST-LINK MCO/HSE input and a valid H743 clock tree
  with a 50 MHz RMII reference clock.
- **ETH:** RMII with the board PHY, matching the NUCLEO-H743ZI2 pinout.
- **RNG:** enabled. CubeMX declares `hrng`; `port.c` uses it for every entropy
  request and returns an error if the peripheral fails.
- **USART3:** asynchronous, 115200 baud, on the ST-LINK virtual COM port pins.
  CubeMX declares `huart3`; `port.c` routes newlib logs there. Remove any other
  `_write` implementation from the generated project.
- **LWIP:** IPv4, raw API, `NO_SYS=1`, DHCP or a static address on your test
  network. Apply `lwipopts.h` from this directory, then regenerate only the
  files that do not own that configuration.

Copy `app.c`, `app.h`, and `port.c` into the application. Add
`src/sys/port/lwip.c` from CWIST to the target sources. For the amalgamation,
add `dist/amalgamation/cwist.c` and `dist/amalgamation/include`; for the fat
archive, add the archive and `dist/cwist-fat/include` instead.

## 3. Start CWIST in `main.c`

Initialize the HAL, clocks, RNG, USART3, Ethernet, and lwIP in CubeMX's usual
order. After `MX_LWIP_Init()`, add:

```c
#include "app.h"

if (cwist_stm32_app_init() != 0) {
    Error_Handler();
}
```

Keep `MX_LWIP_Process()` in the infinite loop. CWIST handlers run in lwIP's
core context, so they must not block waiting for another network event.

```c
while (1) {
    MX_LWIP_Process();
}
```

No RTC is required. Without a working wall clock CWIST omits the HTTP `Date`
header. Do not substitute a constant entropy function: the RNG hook is
mandatory for hash seeds, session secrets, and TLS.

## 4. Flash and check

For a binary produced by CubeIDE:

```sh
st-flash --reset write Debug/cwist-stm32.bin 0x08000000
```

Or with STM32CubeProgrammer:

```sh
STM32_Programmer_CLI -c port=SWD -d Debug/cwist-stm32.elf -rst
```

Once DHCP has assigned an address, or after setting a static one, run:

```sh
curl --fail http://BOARD_IP/hello
curl --fail http://BOARD_IP/json
```

Expected bodies are `hello from stm32h743` and
`{"board":"nucleo-h743zi2","rtos":false}`.

See `docs/embedded/stm32.md` for memory sizing, TLS, supported targets, and the
adapter's limitations.
