# hal_ag32 samples

58 samples covering the AgRV2K SoC, the `agrv2k_407` board wiring and the
CMSIS-DAP probe flows. Every sample is `platform_allow: agrv2k_407` and
`build_only: true` in twister: they need the board and, unless noted, the
canonical 200 MHz bitstream (SYSCLK 200 / BUSCLK 100 / HSECLK 8, md5
`6378549f3a8f82dd386353077f3d4a02`) so that the pins each sample uses are
routed. That image is **not redistributed here** — keep a copy on your machine
and point `AGM_BITSTREAM_BIN` at it, as described in
[`bitstreams/README.md`](bitstreams/README.md). The doc index is
[../docs/README.md](../docs/README.md).

Each sample is a normal west-module app: `src/main.c`, `prj.conf`,
`sample.yaml`, plus `boards/<board>.overlay` when it needs board-level
configuration.

## What is in here

| Area | Samples |
|---|---|
| Platform / boot | `hello_world` (banner + LED heartbeat), `board_info` (prints the devicetree it was built against), `flash_internal` (on-die flash self-test), `fcb_reload` (which bitstream is live after a FLASH-side reload), `fcb_hotswap` (hot fabric reload experiment), `pm_gate_audit` (APB clock gates at boot / idle / runtime-PM) |
| Bootloader / DFU | `spi_boot_loader` (stage-1 loader: record + A/B + upload paths + bitstream slots), `spi_boot_app` (the payload it boots; the signed layout is a second scenario), `verify_flow` (the application half of the end-to-end verification flow: `../tools/verify_flow.py` drives the whole signed-boot chain and asserts every step) |
| SPI + NOR | `spi_flash_id`, `spi_flash_rw` (erase + 4/64/256-byte page program + read-back), `spi_nor_flash` (upstream `jedec,spi-nor` driver), `spi_quad_read` (dual/quad lines), `spi_full_duplex` (CPLD-assisted full duplex), `spi_loopback` (what the engine actually transmits), `slave_spi` (fabric-side slave) |
| CPLD / fabric | `cpld_reg` (CPLD AHB window through `cpld_agm`) |
| Display | `lcd_40pin` (HX8369A 480x800 on a bit-banged 16-bit 8080 bus), `elink` / `elink_monitor` (Pervasive E2271CS091 e-paper) |
| CAN | `can_loopback`, `can_master`, `can_listen`, `can_looptx`, `can_looptx_ack`, `can_rxmon`, `can_txedge`, `can_pin_drive` (driver bring-up plus the two-node dev board setups) |
| Timers / counters | `timer_alarm`, `gptimer_alarm`, `gptimer_pwm`, `gptimer_pwm_blinky`, `gptimer_pwm_pinout`, `rtc_alarm`, `tick_busy` (tick keeps running while a thread never sleeps) |
| Power / watchdog | `pm_sleep` (suspend-to-idle), `pm_rtc_wake` (stop + RTC alarm wake), `wdt_feed`, `iwdg_basic` |
| I2C / DMA | `i2c_scan` (finds the BH1750 at 0x23 on the eval board), `dma_memcpy` (memory-to-memory) |
| GPIO / interrupts | `gpio_irq` (GPIO6 edge → PLIC → MEIP), `plic_uart_irq` (UART RX → PLIC → ISR) |
| Networking | `lan8720_link` (MAC0 + RMII PHY, link state, ICMP), `lan8720_iperf` (TCP throughput against a host `iperf -s`), `lan8720_udp_direct` (hand-built Ethernet/IP/UDP datagrams) |
| USB | `usb_cdc_echo`, `usb_hid_mouse` (UDC device controller; the board enumerates as CDC-ACM / HID) |

## Build, flash, capture

From a west workspace (`<ws>` = the directory that contains `zephyr/` and
`modules/hal_ag32/`):

```bash
source <your-venv>/bin/activate
cd <ws>
west build -d /tmp/b_hello -b agrv2k_407 --pristine=auto modules/hal_ag32/samples/hello_world
west flash -d /tmp/b_hello            # firmware + bitstream (the default)
```

`west flash` writes both images by default; `--skip-bitstream` writes firmware
only (sector erase keeps the fabric), `--bitstream-only` touches the bitstream
area alone. To build a sample against another bitstream without reconfiguring,
pass it as an environment variable:

```bash
AGM_BITSTREAM_BIN=<your canonical bitstream> west flash -d /tmp/b_hello
```

Capture the boot output (the reader must be open *before* the reset):

```bash
bash tools/test_uart_capture.sh -n -t 6        # -n: keep the firmware area erased-state, write only
bash tools/test_uart_capture.sh -t 6           # full erase + write + capture
```

Regression build over the whole directory:

```bash
west twister -T modules/hal_ag32/samples -p agrv2k_407 --build-only -O /tmp/tw_samples
```

Samples that need wiring or a non-default bitstream say so in their own
`sample.yaml` / `README.rst`; `../tools/check_pin_routing.sh` checks the pin map
against the generated pinctrl fragments.

## Reading the output

Samples print their verdicts rather than only raw values (e.g. `PASS` / `FAIL`
lines, register read-backs compared against expectations). A sample that is
silent is usually a clock/bitstream mismatch or a reset that did not happen —
see [../docs/FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §0 for the
symptom table and the BOOT0 recovery path.
