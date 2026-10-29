# CWIST wire inspector on STM32F769I-DISCO

A no-RTOS firmware that serves a small HTML+CSS page on port 80 showing each
HTTP request and response as a hex+ASCII dump — open it in Firefox (or any
browser), hit F5, and watch the bytes your browser sends and the board sends
back. A POST form at the bottom of the page exercises the request-body path.
`GET /hello` and `GET /json` remain available for `curl` checks.

Unlike `example/stm32` (NUCLEO-H743ZI2, CubeMX-generated skeleton), this
example is fully self-contained: it builds against a sparse checkout of the
upstream STM32CubeF7 package and the CWIST bare-metal archive.

## 1. Fetch STM32CubeF7 (once)

```sh
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/STMicroelectronics/STM32CubeF7.git ~/STM32CubeF7
cd ~/STM32CubeF7
git sparse-checkout set --skip-checks \
    Drivers/CMSIS/Device/ST/STM32F7xx Drivers/CMSIS/Include Drivers/CMSIS/Core \
    Drivers/STM32F7xx_HAL_Driver \
    "Projects/STM32F769I-Discovery/Applications/LwIP/LwIP_HTTP_Server_Netconn_RTOS"
git submodule update --init --depth 1 \
    Drivers/STM32F7xx_HAL_Driver \
    Drivers/CMSIS/Device/ST/STM32F7xx \
    Drivers/BSP/Components/lan8742
```

Point `CUBE` at a different location with `make CUBE=/path/to/STM32CubeF7`.

## 2. Build and flash

From this directory, with `arm-none-eabi-gcc` (newlib-nano) and `openocd`
installed and the board on USB:

```sh
make          # also builds ../../libcwist_baremetal.a if missing
make flash    # openocd + on-board ST-LINK
```

The UART console (ST-LINK virtual COM port, e.g. `/dev/ttyACM0`, 115200 8N1)
prints boot progress, link state, and the DHCP-allocated address:

```
cwist-f769: boot
cwist-f769: listening on port 80 (DHCP)
cwist-f769: link UP (2)
cwist-f769: IP 192.168.0.222
```

Then browse to `http://<address>/`.

## Board-specific notes

- **Clocks:** HSE 25 MHz → PLL → 200 MHz SYSCLK (over-drive on), matching the
  CubeF7 LwIP project for this board.
- **UART:** USART1 TX=PA9 RX=PA10 (the ST-LINK VCP pins), 115200 baud.
- **Ethernet:** LAN8742 PHY over RMII, zero-copy RX/TX with hardware checksum
  offload. All DMA buffers sit in SRAM1/SRAM2 (0x20020000+); the linker script
  keeps them out of the DTCM, which the Ethernet DMA cannot reach.
- **D-cache is off** (I-cache on): this revision of the CubeF7 HAL ETH driver
  performs no cache maintenance, so DMA buffers would go stale with D-cache
  enabled. The CubeF7 RTOS example gets away with it by marking SRAM2
  non-cacheable through the MPU instead.
- **Rev-A RMII erratum (ES0290):** on F769 rev A (`DBGMCU_IDCODE` REV_ID
  `0x1000`) the RMII receiver can latch up after reset so every frame fails
  CRC. `ethernetif.c` runs ST's workaround — stop the MAC, toggle
  `SYSCFG_PMC_MII_RMII_SEL`, restart — from the poll loop until good unicast
  frames arrive.
- **GPIO speed:** the RMII pins use `GPIO_SPEED_FREQ_VERY_HIGH` with the I/O
  compensation cell enabled; `GPIO_SPEED_HIGH` is marginal at 50 MHz.

## Connection limits (measured on hardware)

Stock firmware: **4 simultaneous connections** (`cfg.max_conns = 4`). That is a
configuration default, not a hardware limit. The table below was measured on
the board by holding N sockets open and requesting `/hello` on all of them;
`max_conns` is `cfg.max_conns` in `app.c`, `PCB` is `MEMP_NUM_TCP_PCB` in
`lwipopts.h` (always set to N + 8 for TIME_WAIT headroom).

| max_conns / PCB | paced connects (50 ms apart) | tight 64-way SYN+GET burst |
|---|---|---|
| 4 / 6 (stock)   | 4/4; a 5th connects but is not served | — |
| 8 / 16          | 8/8    | 8/8 |
| 16 / 24         | 16/16  | 16/16 |
| 32 / 40         | 32/32  | 32/32 |
| 64 / 72         | 64/64  | ~50/64 (see below) |
| 96 / 104        | 96/96  | — |
| 128 / 136       | 128/128 | — |
| **192 / 200**   | **192/192** | — |
| 256 / 264       | ~196 served, then resets (RAM ceiling) | — |

**The highest fully validated configuration is 192 simultaneous connections**
(`max_conns = 192`, `MEMP_NUM_TCP_PCB = 200`). No leaked slots after overload:
every batch, once closed, is served again at the same rate.

Two distinct ceilings showed up:

- **Burst loss, not a connection cap.** With 64 SYNs and GETs fired in one
  tight loop, some data frames never reach the driver; those connections sit
  ESTABLISHED but silent and are reaped by the 30 s idle timeout. Pacing the
  connects ~50 ms apart (or simply retrying) makes the same configuration
  serve all 64. Raising `cfg.idle_timeout_ms` makes bursts more forgiving;
  enlarging the RX pool did not change the outcome.
- **RAM, around 196 connections with the stock per-connection buffers.** At
  256 the first ~196 requests are answered and later ones are reset: each
  connection owns a growable RX/TX buffer pair on the newlib heap, and the
  heap runs out. To go further, shrink the initial buffer sizes in
  `src/sys/port/lwip.c` or give the heap more of the 384 KB SRAM region (the
  128 KB DTCM is currently unused, but DMA-visible buffers must stay in
  SRAM1/2).

Note for repeat tests: closed connections sit in TIME_WAIT and keep their
PCB for about a minute. Back-to-back batches therefore need
`MEMP_NUM_TCP_PCB` headroom or a short wait between runs.

Each response is serialized into a per-connection buffer that is retained and
reused after the peer acknowledges it (Phase 2 deterministic-memory design),
so the steady-state per-request allocation count does not grow with the
connection count.

## Memory budget

Roughly 93 KB flash, 139 KB RAM (including a 64 KB newlib heap and the lwIP
pools) out of 2 MB / 512 KB. `arm-none-eabi-size` prints exact numbers at the
end of `make`.
