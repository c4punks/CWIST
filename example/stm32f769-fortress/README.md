# CWIST Connection Fortress on STM32F769I-DISCO

A no-RTOS firmware that turns the board's 800x480 LCD into a live connection-pool monitor. Each of the 288 cells represents one real CWIST lwIP pool slot and changes color as the slot receives, sends, or streams a response. Firefox shows the same state through a local HTML dashboard.

The example also serves a configurable 64 B, 16 KiB, 256 KiB, or 1 MiB response. Small responses use direct body references to avoid delayed-ACK stalls; large responses use bounded 192-byte staging chunks. The 1 MiB response body occupies external SDRAM, not the MCU heap.

## What is on the board

- 288 fixed connection slots with 512-byte RX and 192-byte TX buffers
- LCD slot grid, accept/reject counters, request count, DHCP address, and payload size
- `GET /`: self-contained HTML+CSS+JavaScript dashboard
- `GET /api/stats`: pool counters and all 288 slot states as JSON
- `POST /api/config`: `payload=64`, `16384`, `262144`, or `1048576`
- `GET /stream`: the selected response, streamed without per-chunk allocation
- Snapshot arrays in DTCM, LCD framebuffer at `0xC0000000`, response payload at `0xC0200000`

## 1. Fetch STM32CubeF7

Follow `example/stm32f769/README.md` for the initial sparse checkout, then add the display inputs:

```sh
cd ~/STM32CubeF7
git sparse-checkout add \
    Drivers/BSP/STM32F769I-Discovery \
    Drivers/BSP/Components/Common \
    Drivers/BSP/Components/otm8009a \
    Drivers/BSP/Components/nt35510 \
    Drivers/BSP/Components/adv7533 \
    Utilities/Fonts
git submodule update --init --depth 1 \
    Drivers/BSP/Components/Common \
    Drivers/BSP/Components/otm8009a \
    Drivers/BSP/Components/nt35510 \
    Drivers/BSP/Components/adv7533
```

## 2. Build and flash

```sh
cd example/stm32f769-fortress
make -j4
make flash
```

The UART console uses the ST-LINK virtual COM port, 115200 8N1. It prints the DHCP address and the LCD displays the same URL:

```text
cwist-f769-fortress: boot
cwist-f769-fortress: 288 slots, listening on port 80
cwist-f769-fortress: IP 192.168.0.222
```

Open `http://<address>/` in Firefox. Select a payload size and open `/stream` to see streaming slots on both displays.

## 3. Run a real connection test

The host script uses only the Python standard library. It opens and holds concurrent TCP connections, sends one request on each, verifies status and Content-Length, and reports every failure:

```sh
python3 load_test.py 192.168.0.222 -n 32
python3 load_test.py 192.168.0.222 -n 128
python3 load_test.py 192.168.0.222 -n 288
```

Use a larger ramp if the network drops tight connection bursts:

```sh
python3 load_test.py 192.168.0.222 -n 288 --pace 0.10 --hold 5
```

To exercise the 289th-connection rejection path:

```sh
python3 load_test.py 192.168.0.222 -n 289
```

To stream the selected body on every connection instead of requesting the JSON snapshot:

```sh
python3 load_test.py 192.168.0.222 -n 32 --path /stream
```

The firmware is configured for 288 slots; use the script result, not the configuration value, as the hardware acceptance measurement. Wait about one minute between full-capacity batches or leave PCB headroom for TIME_WAIT sockets.

## Measured on hardware

Validated on an STM32F769I-DISCO over real Ethernet on 2026-11-02:

- A single keep-alive connection served 872.81 requests/second at 1.54 ms average latency, up from 22.40 requests/second at 44.03 ms.
- 32/32 and 128/128 concurrent `/api/stats` exchanges succeeded.
- 288/288 concurrent exchanges succeeded; connection 289 was reset by the configured capacity gate.
- 32/32 concurrent 1 MiB streams returned the complete 32 MiB aggregate payload in 27.61 seconds.
- The HTML dashboard, JSON snapshot, payload selection, and exact 1 MiB payload contents were verified separately.

## Memory and timing notes

The LCD framebuffer and payload use the board's external SDRAM. Snapshot arrays use DTCM because Ethernet DMA never accesses them. Ethernet buffers, lwIP pools, and the CWIST connection pool remain in SRAM1/SRAM2. The Makefile builds the bare-metal archive with 4 KiB arena generations to leave room for the 288-slot pool and concurrent response objects.

The main loop refreshes the LCD every 200 ms and redraws only slots whose state changed. Network polling continues in the same no-RTOS lwIP core context. The browser polls `/api/stats` every 500 ms; WebSocket support is not required.

The RMII, clock, UART, and D-cache constraints are identical to `example/stm32f769`. In particular, D-cache remains disabled because the reused CubeF7 Ethernet HAL does not perform the required cache maintenance.
