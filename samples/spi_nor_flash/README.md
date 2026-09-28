# spi_nor_flash — Zephyr SPI NOR on top of the AgRV2K SPI driver

The pay-off of the SPI port: the upstream `jedec,spi-nor` flash driver
running over `spi_agm`, on a bitstream that routes the controller straight
to the flash pins (the vendor stock bitstream routes SPI1 there; with the
CPLD full-duplex patch MISO never returns to the controller, so spi-nor
cannot work on such a bitstream).

## How the pieces line up

| Piece | Detail |
|---|---|
| DT | `flash@0` on the SPI controller: `jedec-id = [68 40 15]`, `size = <0x1000000>` — **`size` is in bits** (16 Mbit = 2 MiB; the RDID capacity code 0x15 is 2^21 bytes) |
| Config | `CONFIG_SPI_NOR_SFDP_MINIMAL` — the part implements no SFDP, so the driver synthesizes page/erase geometry from `jedec-id` + `size` and **verifies the ID at bring-up** |
| Transfer | spi-nor reads a page as TX `{0x03 + 3 address bytes}` then RX `{1 dummy byte, 256 data bytes}` — `tx_len=4, rx_len=260, rx count=2` |
| Driver | `spi_agm` programs the TX phase, then one RX phase of 260 bytes and streams it out of the phase data register with **DMA** into its bounce buffer, copying the bytes into the caller's two RX buffers |

The bounce buffer is what makes this work at all: the engine moves 32-bit
words, while callers like spi-nor hand over a non-contiguous RX list whose
total length (1 + data) is not a multiple of four. `SPI_AGM_RX_BOUNCE_BYTES`
(512 by default) must cover the largest single read.

Two devicetree traps found the hard way:

* **`size` is in bits, not bytes** (`jedec,jesd216.yaml`: "flash capacity in
  bits"). Writing the byte count makes the driver think the part is 8x
  smaller and `flash_read()` returns `-EINVAL` past that (wrong) end -- with
  no other symptom.
* The bring-up ID check means a wrong `jedec-id` or a broken RX path fails
  the first `flash_read()`, so the sample's result covers both.

## Measured (2026-09-13, non-fabric SPI1 bitstream, 100 MHz clock overlay)

```
spi_nor_flash: flash@0 before first use: not ready (PM resume pending)
spi_nor_flash: write-block-size=1, erase value=0xff
spi_nor_flash: DT size = 0x200000 bytes
spi_nor_flash: first page: start 0x00000000, size 65536
spi_nor_flash: after the first read the device is still not ready
spi_nor_flash: page@0x0000 = ff ff ff ff ff ff ff ff ... (256 bytes)
spi_nor_flash: 0 of 256 bytes differ from the erase value (blank region, as expected)
spi_nor_flash: two 256-byte page reads agree -> PASS
spi_nor_flash: scanned 8192 of 8192 pages (2097152 bytes) at 25000000 Hz
spi_nor_flash: 0 bytes differ from 0xff; checksum 0x00000000
spi_nor_flash: the whole part is blank
```

The blank content is expected: the board always boots from the chip's
**internal 1 MB flash** (that is what the whole port targets), and this
on-board SPI flash has only ever been read here -- nothing in this repository
programs it. The full scan is 8192 page reads through the RX DMA path, so it
exercises the whole 2 MiB address range, not just the first page.

> `BOOT0`/PIN_94 is **not** "the pin that switches to this SPI flash": the
> vendor's hardware notes (《硬件设计注意事项》§5) make `BOOT0` high + `BOOT1`
> low at power-up the **ROM serial-download** strap. Which combination (if
> any) boots from the on-board SPI flash is not documented by the vendor;
> see `docs/AG32-PINOUT.md` §12.

Two quirks worth knowing:

* **`device_is_ready()` can read false right after boot** (and stays false
  while the driver's runtime-PM state is "suspended"). The bring-up,
  including the JEDEC ID check, happens in the PM resume action, which the
  first API call triggers — so gate on the *call's* return code, not on
  readiness.
* The driver reports `write-block-size=1`, i.e. byte writes are allowed by
  the *driver*; the flash's page size (256 B) still applies.

## Build and run

```sh
source <your-venv>/bin/activate
bash tools/flash_logic.sh $HOME/example_board.bin      # non-fabric bitstream
cat > /tmp/agm100.overlay <<'EOF'
&clk0 { clock-frequency = <100000000>; };
&cpu0 { clock-frequency = <100000000>; };
&sys  { flash-max-frequency = <50000000>; };
EOF
west build -b agrv2k_407 -d /tmp/b_spinor --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_nor_flash \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash tools/test_uart_capture.sh -t 10 /tmp/b_spinor/zephyr/zephyr.bin
```

## Write test

`CONFIG_APP_SPI_NOR_WRITE_TEST=y` adds the write half of the API:
`flash_erase()` on one 4 KiB sector, `flash_write()` of 4096 bytes, and a
byte-by-byte read-back. That is the path whose page program hands the SPI
driver a **two-buffer** TX list (command+address, then 256 bytes of data),
so it needs the engine's TX DMA and the driver's TX bounce buffer:

```
spi_nor_flash: erase -> 0 of 4096 bytes differ from 0xff -> PASS
spi_nor_flash: write 4096 bytes, 0 bytes read back wrong -> PASS
```

It is **destructive** and off by default; `CONFIG_APP_SPI_NOR_KEEP_PROGRAMMED`
leaves the data in place instead of erasing it again, so another tool can
read back exactly what this path wrote. The read-back is chunked to 256
bytes because a single 4 KiB read would have to fit the driver's RX bounce
buffer.

## Not covered

* A single `flash_read()` larger than `CONFIG_SPI_AGM_RX_BOUNCE_BYTES`
  (512 by default): the engine streams RX through a bounce buffer, so this
  sample reads page-sized chunks instead. Raise that option for big reads.
* Protection bits (`WPSEL`/block protect) and data retention -- the parts
  used here come up unprotected, and the sample only tests the same boot.
