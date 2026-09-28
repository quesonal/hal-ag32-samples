# spi_flash_rw — erase and program an SPI NOR through spi_agm

> **This sample destroys data.** It erases the 4 KiB sector at
> `CONFIG_APP_FLASH_RW_TEST_OFFSET` (default `0x0`, the on-board SPI flash's
> own boot-vector area), then reprograms three of its pages. Only run it on a
> flash you are willing to lose.

The write half of the SPI bring-up. `samples/spi_flash_id` proves the read
path; this one exercises every transmit shape the
[spi_agm](../../drivers/spi/spi_agm.c) driver supports, and checks each one
by reading the result back:

| # | What | Frame | Path |
|---|---|---|---|
| 1 | `0x20` sector erase | 4 B | one register-fed TX phase |
| 2 | `0x02` program 4 B | 4 + 4 = 8 B | register-fed phases |
| 3 | `0x02` program 64 B | 4 + 64 = 68 B | engine TX DMA (flow 5) |
| 4 | `0x02` program 256 B | 4 + 256 = 260 B | engine TX DMA — the shape a page program needs |
| 5 | count of programmed bytes | 16 x 256 B reads | RX DMA + bounce buffer |
| 6 | `0x20` erase again | 4 B | register-fed, leaves the sector blank |

Every `0x06` Write-Enable is checked through `RDSR` before the command that
needs it is issued, and each erase/program is followed by a `WIP` poll, so a
frame that silently does not reach the part fails the test instead of
looking like a slow one.

The reads go through the driver in the SPI API's **slot** shape: the frame's
first bytes carry the command, so they appear in the RX buffer list too and
come back as `0xFF` (this engine samples MISO only after its TX phases).
That is the same shape upstream `jedec,spi-nor` hands over — see
`the development notes (not published here)`.

## Which controller?

That is a bitstream property, so the sample probes every enabled
`agm,agrv2k-spi` node and takes the first one whose RDID is neither all-zero
nor all-ones.

## Measured (2026-09-13, SPI1 on the flash bitstream, 25 MHz)

```
spi_flash_rw: spi@40012000 RDID = 00 00 00 (ret=0)
spi_flash_rw: spi@40013000 RDID = 68 40 15 (ret=0)
spi_flash_rw: status before = 0x00
spi_flash_rw: sector 0x000000 currently holds 0 bytes that differ from 0xff (blank)
spi_flash_rw: [1] erase 0x000000..0x000fff: 0 of 4096 bytes differ from 0xff -> PASS
spi_flash_rw: [2] program 4 B at 0x000000 -> MATCH
spi_flash_rw: [3] program 64 B at 0x000100 -> MATCH
spi_flash_rw: [4] program 256 B at 0x000200 -> MATCH
spi_flash_rw: [5] 323 of 4096 bytes in the sector are now programmed (expected 323)
spi_flash_rw: [6] erase again: 0 of 4096 bytes differ from 0xff -> PASS
spi_flash_rw: PASS
```

`323` instead of `4 + 64 + 256 = 324` is the pattern, not the flash: the
data is `0xA0 + i`, so exactly one byte of the 256-byte page lands on `0xFF`
and is indistinguishable from erased. The sample computes the expectation
from the pattern instead of hard-coding it.

The patterns are `0xA0 + i`, so a shift, a byte reversal or a byte that
stayed erased is visible in the mismatch dump.

## Does the data survive a reboot?

A read-back in the same boot cannot tell a programmed page from a page that
was never erased. `CONFIG_APP_FLASH_RW_KEEP_PROGRAMMED=y` skips step 6, and
the sample prints what the sector holds *before* it erases anything. Run it
once with that option, then run any build again (reflashing the MCU does not
touch the external flash):

```
spi_flash_rw: sector 0x000000 currently holds 323 bytes that differ from 0xff (previous run's data?)
```

## Build and run

```sh
source <your-venv>/bin/activate
# the bitstream must route a controller straight to the flash pins
bash tools/flash_logic.sh $HOME/spi_full_mac_bitstream_200mhz/example_board.bin
# firmware must match that bitstream's SYSCLK; 100 MHz needs the overlay
west build -b agrv2k_407 -d /tmp/b_frw --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_flash_rw \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash tools/test_uart_capture.sh -t 30 /tmp/b_frw/zephyr/zephyr.bin
```

## Not covered

* The upstream `jedec,spi-nor` **write** path: its page program hands the
  SPI driver a two-buffer TX set (command+address, then data), which
  `spi_agm` only accepts inside the 32-byte register-fed window today — a
  page-sized write from the flash API needs the TX side to gain the bounce
  buffer the RX side already has. `samples/spi_nor_flash` stays read-only
  for that reason.
* Data retention beyond a warm reboot, and the flash's own protection bits
  (`WPSEL`/block protect) — the parts used here come up unprotected.
