# spi_loopback — what does the SPI engine actually transmit?

Wire the fabric patch's MISO input to its own MOSI output — a jumper between
the two pins, or `SPI1_SI_IO0 → so_io1` inside the bitstream — and the bytes
the engine clocks out come back through the patch's capture register. The
fabric becomes the witness for the transmit path, which is otherwise
invisible without a logic analyser.

All the transfers are clocked by the on-die engine and captured by the
fabric (`full_duplex_spi.v`):

| # | TX | Path | What it tells us |
|---|---|---|---|
| 0 | 4 B | direct loopback, no fabric (`spi_transceive` TX + RX) | is the jumper between the pins the engine uses? A direct loopback cannot show the TX bytes: the engine samples MISO only after its TX phases, so what comes back is the line *after* the pattern — measured `00 00 00 00` for every pattern tried (`00…`, `ff…`, `5a a5 00 ff`), i.e. the idle level rather than the data. The RX list here covers the whole frame: the four slots that carry the command come back as `0xFF` because the engine cannot sample them (see `the development notes (not published here)`) |
| 1 | 4 B | `spi_write()` — one register-fed phase | does the plain TX path put the bytes on the wire? |
| 2 | 32 B | `spi_write()` — eight register-fed phases, captured through the fabric FIFO | the driver's register-fed maximum |
| 3 | 36 B | `spi_write()` — engine TX DMA (phase0 4 B register + DMA-fed phase1, flow 5) | the SDK's `SPI_Send_Long`/`SPI_FLASH_WritePage` shape |
| 4 | 36 B | the same transfer with the engine and the DMAC programmed by hand (`APP_LOOP_DIRECT_TX_DMA`, off by default) | characterisation probe: the driver's bytes must match the hand-built ones |
| 5 | 256 B (up to 4096) | **long TX and long RX at once**: `spi_write()` (engine TX DMA) while a second DMAC drains the fabric's capture FIFO (`APP_LOOP_LONG_BYTES`) | the shape a full-duplex transfer needs — the driver's own RX cannot follow a DMA-fed TX, the fabric's can |

The patterns are `0xA0 + i`, so a shift, a byte reversal or an idle MOSI are
all readable in the printed `TX`/`RX` lines.

## Wiring

* The bitstream must carry the full-duplex patch **and** route one
  controller's SCK/CSN/MOSI into it — the dev board bitstream does this for
  SPI0; a bitstream that also maps SPI1 uses `APP_LOOP_CONTROLLER_SPI1`.
* The loop: MOSI → MISO. Either a physical jumper or, if the bitstream maps
  both, a fabric connection.
* Do **not** run this on a bitstream without the fabric patch: reading the
  CPLD window at `0x60000000` on a bitstream with no slave there stalls the
  CPU (no `hreadyout`).

## Knobs

| Kconfig | Default | Meaning |
|---|---|---|
| `APP_LOOP_CONTROLLER_SPI0` / `_SPI1` | SPI0 | which engine clocks the transfer |
| `APP_LOOP_CTRL_OFFSET` / `_DATA_OFFSET` | 0 / 4 | fabric register offsets in the window |
| `APP_LOOP_CPOL` / `_CPHA` | 0 / 0 | written into the fabric control register |
| `APP_LOOP_FIFO_DMA_CHANNEL` / `_REQUEST` | 5 / 1 | drain of the fabric RX FIFO (`EXT_DMA0_REQ`) |
| `APP_LOOP_TX_DMA_CHANNEL` / `_REQUEST` | 6 / 6 | only used by the hand-programmed probe, test 4 (SPI0 TX request = 6, SPI1 = 8) |
| `APP_LOOP_EXPERIMENTAL_TX_DMA` | y | run test 3 (the driver's >32 B TX-DMA path) |
| `APP_LOOP_DIRECT_TX_DMA` | n | also run test 4 (the hand-programmed probe) |
| `APP_LOOP_LONG_BYTES` | 256 | length of test 5 (long TX + long RX); 0 disables it, 4096 is the practical ceiling |
| `APP_LOOP_CONTINUOUS` | y | after the tests, retransmit 4 bytes in a loop so a meter sees the pins move; turn it off for a quiet board |

## Build and run

```sh
source <your-venv>/bin/activate
bash tools/flash_logic.sh <bitstream with the patch and the loop>
west build -b agrv2k_407 -d /tmp/b_loop --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_loopback \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash tools/test_uart_capture.sh -t 12 /tmp/b_loop/zephyr/zephyr.bin
```

## Expected outcomes

Measured on the dev board (2026-09-13, `spi_full_bitstream_without_flash`, MOSI
PIN_79 wired to MISO PIN_80):

```
spi_loopback: fabric CTRL readback = 0x00000400 (patch answers)
spi_loopback: [1] TX   4 B via driver: ret=0
spi_loopback: [1] RX = a0 a1 a2 a3 -> MATCH
spi_loopback: [2] TX  32 B via driver: ret=0
spi_loopback: [2] RX = a0 a1 ... bd be bf -> MATCH
spi_loopback: [3] TX  36 B via driver: ret=0
spi_loopback: [3] RX = a0 a1 ... c1 c2 c3 -> MATCH
spi_loopback: [5] long TX + long RX: 4096 B each -> captured 4096 bytes -> MATCH
```

All the shapes transmit byte-exactly (4 B register, 32 B eight phases, 36 B
engine TX DMA), so `spi_agm` enables the TX DMA path
for TX-only transfers longer than 32 bytes. See
`the development notes (not published here)`.

Test 5 is the "long TX and long RX at the same time" case: 256, 1024 and
4096 bytes all came back byte-for-byte, with the engine's TX DMA and the
fabric's FIFO drain running concurrently. The patch's FIFO is 256 words
(1024 bytes), so 4096 proves the drain keeps up with the producer rather
than relying on the FIFO as a whole-transfer buffer -- see
`the development notes (not published here)`.

* If the capture stays `00`/`ff` while the engine reports done, the patch is
  not in the bitstream, the offsets are wrong, or the loop is missing.
