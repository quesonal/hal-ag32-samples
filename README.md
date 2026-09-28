# hal_ag32 — AgRV2K samples

This repository is the **samples-only** companion of
[`hal_ag32`](https://github.com/quesonal/hal-ag32), the AGM AgRV2K Zephyr
module. It carries the 58 sample applications; the module repo carries the
drivers, the SoC glue, the boards, the devicetree and the user-facing
docs.

## How this repo lands in a workspace

`hal_ag32` (the module) pulls this repo as a west sub-module. After
`west init -m hal_ag32`, west places the samples at:

```
<ws>/modules/hal_ag32_samples/samples/
```

so the canonical build command is:

```sh
west build -b agrv2k_407 modules/hal_ag32_samples/samples/hello_world
```

If you use this repo directly (a `west init -m hal_ag32_samples` style
workspace) you also need `hal_ag32` itself on the manifest path; see
[`west.yml`](west.yml).

## What is in here

| | |
|---|---|
| **Index** | [`samples/README.md`](samples/README.md) — every sample, what it proves, what hardware it needs |
| **Loader** | [`samples/spi_boot_loader/`](samples/spi_boot_loader/README.rst) — the on-flash stage-2 bootloader (driver is in `hal_ag32`) |
| **Bitstream targets** | [`samples/dual_ip/`](samples/dual_ip/), [`samples/slave_spi/`](samples/slave_spi/), [`samples/user_ip/`](samples/user_ip/) — examples of how the fabric user logic hooks into the AHB window |
| **USB class gadgets** | [`samples/usb_cdc_echo/`](samples/usb_cdc_echo/), [`samples/usb_hid_mouse/`](samples/usb_hid_mouse/), [`samples/usb_msc_dfu/`](samples/usb_msc_dfu/) and the loader's UF2-channel variant [`samples/usb_msc_dfu_boot/`](samples/usb_msc_dfu_boot/) |
| **Peripherals** | everything else under `samples/` — UART/PLIC/GPIO/DMA/I2C/SPI/TIMER/GPTIMER/WDOG/RTC/CAN/EMAC/LCD/cpld_reg/fcb_reload/pm_rtc_wake/etc. |
| **Sample-specific tools** | [`tools/bin_to_uf2.py`](tools/bin_to_uf2.py) and [`tools/gen_uf2_volume.py`](tools/gen_uf2_volume.py) — wrap a `.bin` into the UF2 block stream and generate the fake FAT16 volume header the `usb_msc_dfu_boot` sample publishes. Both are pure stdlib python3 (no venv, no SDK, no board) |

## Quick start

```sh
# In a workspace built with `west init -m hal_ag32` (the module's manifest
# pulls this repo as a sub-module at modules/hal_ag32_samples):

west build -b agrv2k_407 modules/hal_ag32_samples/samples/hello_world
west flash                                # firmware + bitstream (needs the on-board CMSIS-DAP probe)
modules/hal_ag32/tools/test_uart_capture.sh  # open the console first, then reset -> banner
```

`west flash` writes firmware *and* bitstream by default —
`--skip-bitstream` and `--bitstream-only` split the two;
`AGRV_ADAPTER` picks the debug probe (default `cmsis-dap`, the on-board
one).

The six hard rules (commands that must not be used, what each one does to
the bitstream) and the symptom → cause table for flashing and capturing
live in
[`hal_ag32/docs/FLASH-AND-CAPTURE.md`](https://github.com/quesonal/hal-ag32/blob/main/docs/FLASH-AND-CAPTURE.md).
Read it **first** if automating.

## Common mistakes

* **Console is garbled but the board is alive** — the bitstream's clock
  and the firmware's clock disagree. The canonical image is 200 MHz and
  matches the board default (no overlay); a 100 MHz image needs
  `-- -DEXTRA_DTC_OVERLAY_FILE=<100mhz overlay>`, otherwise UART0 runs at
  the wrong divisor (115200 requested → 57600 actual).
* **`west flash` writes firmware *and* bitstream** by default. Use
  `--skip-bitstream` or `--bitstream-only` to split the two. A
  firmware-only flash keeps the fabric as it was.
* **The boot banner only appears at reset** — open the console *before*
  resetting; `minicom`, `cat /dev/ttyACM0` or a late attach show only the
  periodic output.
* **`west twister` is the regression net**, not `west build`. twister
  additionally enables `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` and
  `--edtlib-Werror`, so a green build can still fail CI-shaped checks.
* **Do not touch `/dev/ttyACM1`** — on the eval board that is the on-board
  ESP32-C3, not the AgRV2K console (`/dev/ttyACM0`).
* **Flashing the wrong clock domain?** `west flash` writes firmware and
  the bitstream at the fabric address; if the board looks dead afterwards,
  see the linked `FLASH-AND-CAPTURE.md` §10 (recovery via the ROM
  bootloader).

## Bitstream

The samples are written against one 200 MHz reference image; it is *not*
redistributed in this repo — `samples/bitstreams/README.md` points the
reader at their own copy (`.gitignore` keeps `example_board.bin` /
`.ve` out of the tree). Pass it via `AGM_BITSTREAM_BIN`; the build
picks it up automatically when it lives at `<build>/zephyr/board.bin`.

## License

Apache-2.0 — see [LICENSE](LICENSE). [NOTICE](NOTICE) lists the vendor
SDK components this module builds against.
