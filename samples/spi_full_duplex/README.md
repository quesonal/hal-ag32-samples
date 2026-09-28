# spi_full_duplex — CPLD-assisted full-duplex SPI

Exercises the vendor fabric patch `full_duplex_spi.v`: the on-die SPI phase
engine supplies SCK/CSN/MOSI, the fabric drives the pins and captures MISO on
every clock edge — so RX and TX happen on the same edges, which the
controller alone cannot do ("AG32 下 SPI 的扩展使用": TX first, no overlap,
RX last).

## Wiring and register model

| Piece | Register | Role |
|---|---|---|
| Fabric (CPLD window) | `+0x00` (configurable) | `DMA_EN` = 1<<8, `ENDIAN` = 1<<10, `CPOL` = 1<<24, `CPHA` = 1<<25 |
| Fabric | `+0x04` (configurable) | `DMA_EN ? FIFO head : last 4 captured bytes` |
| On-die controller | SPI0 / SPI1 phase engine | one TX phase of ≤ 4 bytes: it clocks the transfer |

The VE snippet connects the pins to the fabric and does **not** hand MISO
back to the controller, so `spi_transceive()` reads nothing on such a
bitstream — the sample prints that half-duplex probe for contrast and takes
its answer from the window.

## What the test does

Because the capture register is 32 bits and this sample does no fabric-side
DMA, a transfer captures at most 4 bytes — the device's answer has to be
clocked *inside* those bytes. That is why:

* `0x9F` (RDID) works: the answer follows the command byte immediately, so
  `9F FF FF FF` captures `00 68 40 15`;
* `0x90` does not: its answer starts after the 4th byte, so nothing is
  captured.

The pass criterion is therefore self-calibrating: **two 0x9F captures with
different dummy bytes must yield the same 3-byte ID** (neither all-00 nor
all-FF). `APP_SPI_FULL_EXPECT_ID` can pin the ID for regression on a known
board.

`CONFIG_APP_SPI_FULL_LOOPBACK=y` instead expects a jumper between the MOSI
and MISO pins and checks the transmitted pattern comes back (a one-byte
shift or a reversed byte order is accepted and printed).

## Knobs (all bitstream properties)

| Kconfig | Default | Meaning |
|---|---|---|
| `APP_SPI_FULL_CONTROLLER_SPI0` / `_SPI1` | SPI0 | which controller the fabric taps. The fabric has **no capture-clear**, so testing a controller it does not tap just replays the previous capture — hence an explicit choice instead of probing every node |
| `APP_SPI_FULL_CTRL_OFFSET` | 0 | where the fabric control register sits in the window (vendor `ADDR_OFFSET_SPI0`) |
| `APP_SPI_FULL_DATA_OFFSET` | 4 | where the captured bytes sit |
| `APP_SPI_FULL_CPOL` / `_CPHA` | 0 / 0 | written into the fabric control register; the on-die engine stays in its default timing |
| `APP_SPI_FULL_EXPECT_ID` | 0 | optional expected JEDEC ID (e.g. `0x684015`) |
| `APP_SPI_FULL_LOOPBACK` | n | jumper test instead of the ID test |
| `APP_SPI_FULL_DMA` / `_DMA_CHANNEL` / `_DMA_REQUEST` | y / 5 / 1 | drain the fabric RX FIFO with DMA (`EXT_DMA0_REQ` = 1), DMA_EN set in the fabric CTRL |

## Long transfers (fabric FIFO + DMA)

Long TX needs no DMA: the engine takes up to eight TX phases of four bytes
each (that is how the SDK sends a long command too), so the sample clocks a
16-byte 0x4B command in four phases. Long RX comes from the fabric: with
`DMA_EN` set it pushes a 32-bit word per four captured bytes into its FIFO
and raises `ext_dma_DMACBREQ[0]`, which a `PERIPHERAL_TO_MEMORY` channel
(fixed source = the window data register) drains into the caller's buffer.

## Measured (2026-09-13, 100 MHz full-duplex bitstream, SPI0 tapped)

```
spi_full_duplex: window cpld@60000000, CTRL +0, DATA +0x4, mode 00
spi_full_duplex: controller spi@40012000 (fabric taps SPI0)
spi_full_duplex: half-duplex RDID = 00 00 00 (expected empty on a full-duplex bitstream)
spi_full_duplex: 0x9F capture (FF dummy) = 00 68 40 15
spi_full_duplex: 0x9F capture (00 dummy) = 00 68 40 15
spi_full_duplex: ID = 68 40 15 (at offset 1 and 1)
spi_full_duplex: both captures agree -> PASS

spi_full_duplex: 0x4B capture #1 = 00 00 00 00 00 45 53 53 95 5b 88 a6 fa 45 53 53
spi_full_duplex: 0x4B capture #2 = ff ff ff ff ff 45 53 53 95 5b 88 a6 fa 45 53 53
spi_full_duplex: unique ID = 45 53 53 95 5b 88 a6 fa; 11 non-FF bytes in the answer, both captures agree -> PASS
```

Same flash, same ID as the half-duplex path in `samples/spi_flash_id` — two
different data paths, one answer.

The 64-bit unique ID read (0x4B: command + 4 dummy + 8 ID bytes, 16 bytes
clocked) is also the "pure RX" shape: the engine only sends dummy data while
the fabric captures the device's answer.

Mode 3 (`-DCONFIG_APP_SPI_FULL_CPOL=y -DCONFIG_APP_SPI_FULL_CPHA=y`) returns
the same IDs, with the idle level in the capture flipping to 0xFF — the
polarity bit doing its job.

**The first five bytes of a long capture are not comparable**: the device
does not drive MISO during the command and the dummy bytes, so the captured
idle level differs between runs (observed `00 …` vs `ff …`). Only the answer
region is deterministic, which is what the check compares.

## Build and run

```sh
source <your-venv>/bin/activate
bash tools/flash_logic.sh $HOME/spi_full_bitstream/example_board.bin
# firmware must match the bitstream's SYSCLK (100 MHz here -> overlay)
west build -b agrv2k_407 -d /tmp/b_spifull --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_full_duplex \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash tools/test_uart_capture.sh -t 10 /tmp/b_spifull/zephyr/zephyr.bin

# jumper variant (MOSI <-> MISO), same bitstream:
west build -b agrv2k_407 -d /tmp/b_spifull_lb --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_full_duplex \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay -DCONFIG_APP_SPI_FULL_LOOPBACK=y
```

## If it fails

1. **Capture all 00/FF** — the patch is not in the bitstream, the window
   offsets differ, or the fabric taps the other controller (try
   `APP_SPI_FULL_CONTROLLER_SPI1`).
2. **Answer missing although the device works** — the reply starts after the
   clocked bytes (0x90-style); only in-line answers fit in the 4-byte window
   without DMA.
3. **Loopback mismatch only** — check `APP_SPI_FULL_CPOL/CPHA` and that the
   engine really reaches the fabric (VE lines `SPI0_SCK sck` …).
4. **Long capture hangs or is short** — DMA channel/request
   (`APP_SPI_FULL_DMA_CHANNEL` / `_REQUEST`), that `CONFIG_DMA=y`, and that
   the requested length matches what the engine clocks (the fabric pushes
   one word per four bytes).

See `the development notes (not published here)` for the register-level analysis
and the remaining work (a long TX combined with a long RX, and a real
erase/program on a non-fabric bitstream; the engine's own TX/RX DMA is
verified in §3.27.8/§3.27.10).
