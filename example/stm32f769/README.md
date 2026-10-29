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
configuration default, not a hardware limit. The adapter allocates one static
pool at listen time — `max_conns` slots of
`sizeof(conn) + rx_buffer_bytes + tx_buffer_bytes` — and never touches the
heap per connection or per response afterwards, so the memory cost is fixed
and known at build time. Responses larger than the TX buffer are streamed
through it in chunks, so a big page does not inflate the pool.

Measured on the board by holding N sockets open and requesting `/hello` on
all of them (50 ms between connects); `PCB` is `MEMP_NUM_TCP_PCB` in
`lwipopts.h`, always N + 8 for TIME_WAIT headroom. Buffer sizes are the new
`cfg.rx_buffer_bytes` / `cfg.tx_buffer_bytes` (both default 2048).

| max_conns / PCB | rx / tx bytes | result |
|---|---|---|
| 4 / 6 (stock)   | 2048 / 2048 | 4/4; a 5th connects but is not served |
| 32 / 40         | 2048 / 2048 | 32/32, clean even with tight bursts |
| 128 / 136       | 2048 / 2048 | 128/128 |
| 192 / 200       | 2048 / 2048 | 192/192 |
| 256 / 264       | 512 / 256   | 256/256 |
| **288 / 296**   | **512 / 256** | **288/288 — highest fully validated** |
| 320 / 328       | 512 / 256   | pool does not fit the heap; `cwist_stm32_app_init()` fails at boot |

Before the static pool (dynamic per-connection buffers), 256 connections
failed non-deterministically at ~196 as the heap ran out mid-run. With the
pool, capacity is decided once at boot: 288 simultaneous connections are
fully served, and 320 is a clean boot-time failure instead of resets under
load. Slot sizing on this firmware: `~128 B conn + rx + tx`, so 288 slots of
512+256 B cost ~258 KB out of the ~290 KB heap available after lwIP pools.

Two practical notes:

- **Bursts:** with 64 SYNs and GETs fired in one tight loop, some data frames
  never reach the driver and those connections idle out after 30 s. Pacing
  the connects (or retrying) serves all of them; raising
  `cfg.idle_timeout_ms` makes bursts more forgiving.
- **TIME_WAIT:** closed connections keep their PCB for about a minute, so
  back-to-back batches need the N + 8 headroom above or a short wait.

To raise the ceiling further, shrink `rx_buffer_bytes`/`tx_buffer_bytes`
(smaller slots = more slots) or free RAM. The 128 KB DTCM is unused, but
DMA-visible buffers must stay in SRAM1/2, and the TX buffers are handed to
the Ethernet DMA zero-copy, so they cannot move there.

## Memory budget

Roughly 93 KB flash, 139 KB RAM (including a 64 KB newlib heap and the lwIP
pools) out of 2 MB / 512 KB. `arm-none-eabi-size` prints exact numbers at the
end of `make`.
