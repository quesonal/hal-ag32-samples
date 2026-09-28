# spi_quad_read

Bring-up of the SPI NOR flash on `agrv2k_407` through the **generic** SPI
API with `SPI_LINES_DUAL` / `SPI_LINES_QUAD`. The driver's multiline
wrapper (`spi_agm_transceive_multiline()` in `drivers/spi/spi_agm.c`)
turns one `spi_transceive()` that asked for the multi-line mode into a
phase list -- one single-line TX phase for the command and address, one
single-line DUMMY phase (8 wait clocks) and one RX phase on the requested
line mode -- and runs it through the engine's phase extension
(`spi_agm_transceive_phases()`).

Four reads are exercised, all at the same address:

| shape | command | TX bytes | dummy | data bytes | line mode |
|------|---------|----------|-------|------------|-----------|
| RDID (baseline) | 0x9F | 1 | 0 | 3 | single |
| reference       | 0x03 | 4 | 0 | 16 | single |
| dual output     | 0x3B | 4 | 8 clocks | 16 | dual |
| quad output     | 0x6B | 4 | 8 clocks | 16 | quad |

The sample picks the address itself: it walks the page starts of the first
sector and uses the first 16-byte window that is not all `0xff` (the page
pattern `samples/spi_flash_rw` leaves at 0x000300 is found this way). A
fully erased sector is reported as `INCONCLUSIVE`, never as a pass -- an
all-blank "match" proves nothing (`the development notes (not published here)`).

## Requirements

* **`CONFIG_SPI_EXTENDED_MODES=y`** (see prj.conf). Without it
  `spi_operation_t` is `uint16_t`, the `SPI_LINES_*` bits (16..17) are
  truncated at compile time, and the request silently stays on the
  single-line path: the wrapper is never called, the call returns 0, and
  the RX buffer reads back as `0xff`/`00`. This is what made the
  multi-line path look "physically impossible" on 2026-09-14.
* `agm,spi-multiline = <2>` on the SPI node
  (boards/agrv2k_407.overlay), so `spi_agm_check_config()` accepts the
  multi-line request.
* A bitstream that routes SPI0 to the flash *and* IO2/IO3 to its WP#/HOLD#
  pins. **This sample's own overlay now provides exactly that**: it restates
  the pins node's `mcu-functions`/`mcu-pins` with the six SPI0 pins on the
  flash wiring, so a `west build -t bitstream` here renders
  `<build>/logic/board.ve` with

      SPI0_HOLDN_IO3 PIN_95   SPI0_WPN_IO2 PIN_98
      SPI0_SCK       PIN_93   SPI0_SI_IO0  PIN_92
      SPI0_SO_IO1    PIN_97   SPI0_CSN     PIN_96

  plus the IP's own rows on the same pins (case C: `SPI0_CSN csn` / `SPI0_SCK
  sck` / `SPI0_SI_IO0 si_io0`; case B: `csn`/`sck`/`si_io0`/`so_io1` → the
  same four pins) and the two read-back rows SPI0 needs to see the flash
  again (`SPI0_SO_IO1 PIN_97`, `SPI0_SI_IO0 PIN_92:INPUT`). That is the row
  set the vendor's 200 MHz reference states
  (`~/spi_full_mac_bitstream_200mhz/example_board.ve`). For a quick run one can
  also just flash one of the two bitstreams already known to work: that vendor
  reference (200 MHz -- **measured 2026-09-25: `0x03`/`0x3B`/`0x6B` agree byte
  for byte**) or `~/spi_full_bitstream_97pad/example_board.bin` (100 MHz, the
  same pins, measured 2026-09-14). The repository's *default* pin map (and
  therefore `<build>/zephyr/board.bin` built without this overlay) targets the
  header pins (`PIN_2/3/4/5/97/98`) instead: the on-board flash is not on
  SPI0's path there, and the multi-line calls return `-ENOTSUP`.

  **The C+B rows alone are not enough** (measured 2026-09-25 on
  `agrv2k_407`): the generated wrapper then ties SPI0's read path to zero
  (`gpio0_io_in = {..., 1'b0, 1'b0}`), and the sample reads
  `RDID = 00 00 00`. The two Case A rows above are what puts `PIN_92`/`PIN_97`
  back on SPI0's RX path.

  When you synthesize this sample's `logic/` on a Quartus workstation, take
  the directory **as generated**: `board.v` and `board.vex` are two halves of
  one `gen_vlog` run, and re-running an IP-less prepare step over the
  directory renames the pin constraints back to the MCU function names
  (`SPI0_SI_IO0 PIN_92`), which no port in `board.v` answers to -- Quartus
  does not fail on that, and Supra only warns before putting the four IOs on
  unrelated pins:

      Warn: Unrecognized pin SPI0_CSN or location PIN_96.
      Warn: IO csn is not assigned, placed at pin PIN_4.

  Measured 2026-09-25 (same `.vo`, only the constraint names changed):
  mismatched names -> `RDID = a7 a7 a7`, `spi_quad_read: FAIL`; matching names
  -> `RDID = 68 40 15`, `PASS`. `../../tools/build_bitstream.sh` now refuses to ship
  a mismatched pair (Step 1c).

  **Measured 2026-09-25 (this overlay's own bitstream, `board.bin` md5
  `ba9cc97a…`, path `~/agm-logic/board_spi_ip_20260925.bin`): `PASS (0x03, 0x3B
  and 0x6B agree byte for byte)`** -- so building the fabric here is no longer
  a "borrow someone else's bitstream" exercise. The 323-byte page pattern came
  from `samples/spi_flash_rw -DCONFIG_APP_FLASH_RW_KEEP_PROGRAMMED=y`.

Build it yourself (C+B pin map + the SPI IP): the sample ships the IP in
`ip/full_duplex_spi.v`, and its CMakeLists declares it the way the vendor's
project does -- by name (`set(AGM_LOGIC_IP "full_duplex_spi")`, sources taken
from `ip/<name>.v`) -- so the command needs no extra flag. `-DAGM_USER_RTL=<path>`
overrides it with a file from elsewhere:

```sh
west build -d /tmp/b_quad -b agrv2k_407 <this sample>
# -> <build>/logic/{board.ve,board.qsf} with SPI0 routed through the IP
#    (case A + case C + case B, the same rows as the vendor reference) and
#    the IP's Verilog registered; take logic/ to Quartus as-is, then
#    `west build -t bitstream`
```

Or flash one of the two bitstreams already known to work:

```sh
bash tools/flash_logic.sh ~/spi_full_mac_bitstream_200mhz/example_board.bin
# bash tools/flash_logic.sh ~/spi_full_bitstream_97pad/example_board.bin  # 100 MHz
```

## Build and run

```sh
cd $HOME/zephyrproject
west build -d /tmp/b_spi_quad_read -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/spi_quad_read -- \
    -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -t 25 \
    /tmp/b_spi_quad_read/zephyr/zephyr.bin
```

If the verdict is `INCONCLUSIVE`, the sector is blank: run
`samples/spi_flash_rw` once to program its page pattern, then run this
sample again.

## Expected output (measured 2026-09-14, 97pad 13:41)

```
spi_quad_read: spi@40012000
spi_quad_read: RDID   = 68 40 15
spi_quad_read: pattern at 0x0300
spi_quad_read: 0x03   = a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
spi_quad_read: DUAL   = a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
spi_quad_read: QUAD   = a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
spi_quad_read: spi@40012000 -> PASS (0x03, 0x3B and 0x6B agree byte for byte)
spi_quad_read: PASS
```

Anything else means:

* `INCONCLUSIVE` -- the first 4 KiB are blank; program a pattern first.
* `failed (-95)` (= `-ENOTSUP`) -- the bitstream lacks the quad routing,
  or the node has no `agm,spi-multiline`.
* `FAIL (multiline data differs from 0x03)` -- the request did reach the
  engine but the line/pin setup is wrong. The known-good reference for
  the same bitstream is `samples/spi_flash_rw` case [12], which drives
  the identical read shapes through the phase-list extension.

## Why the buffer shape matters

The wrapper takes **exactly one TX buffer** (the command+address frame)
and **exactly one RX buffer** (the data phase), and answers `-ENOTSUP` for
anything else. The generic SPI convention of mirroring `tx_bufs` in
`rx_bufs` with a `NULL` slot for the command phase is implemented only by
the single-line path (`spi_agm_collect()` reports those slots as `0xff`),
so the sample passes a single RX buffer for the multi-line calls and the
hole-style buffer set for the single-line reference.
