# spi_flash_id — AgRV2K SPI + SPI NOR bring-up

Read-only exercise of the [spi_agm](../../drivers/spi/spi_agm.c) driver
against whatever SPI NOR the board's bitstream routes to SPI0 or SPI1.

## What the hardware allows

The AgRV2K SPI controller is a *phase engine*, and the vendor document
"AG32 下 SPI 的扩展使用" (page 1) states the timing rules a device must
meet to work with it:

1. a transfer starts with TX (never RX);
2. TX and RX never overlap (TX first, then RX);
3. RX is the last segment.

The direct path moves at most 4 bytes per direction — the phase data
register is 32 bits. Longer transfers go through the DMA port
(`PHASE_DATA`): RX of any length drains the last phase (verified: the
flash's 8-byte unique ID, and 260-byte page reads for `jedec,spi-nor`),
and TX beyond 32 bytes hands phase1 to the DMAC (verified byte-exact with
`samples/spi_loopback`). A long TX cannot be combined with RX, because the
DMA applies to the last phase only — the driver returns `-ENOTSUP` there.

Commands with RX-first, CPOL/CPHA ≠ 0 or overlapping TX/RX are still
rejected with `-ENOTSUP` rather than silently mis-clocked; those cases need
the vendor's `full_duplex_spi.v` fabric patch, see
`the development notes (not published here)`/.

RX is counted in **slots**, like the SPI API does: byte i of the frame
carries `tx[i]` on MOSI and lands in `rx[i]`. This engine only samples MISO
once its TX phases are done, so the slots that hold the command come back as
`0xFF` and the answer follows at its natural offset — the sample passes its
RX buffer list that way (`echo` + data), which is also what upstream
`jedec,spi-nor` does.

## Which controller, which pins

That is a **bitstream** property, not a driver one. The sample probes every
enabled `agm,agrv2k-spi` node and prints what each one sees, so a board
that routes SPI1 (rather than SPI0) needs no change here.

Measured on the dev board board (2026-09-13): its SPI NOR sits on
PIN_92/93/95/96/97/98 while the vendor stock bitstream routes SPI0 to
PIN_2/3/4/5/97/98 — mismatched, and even the vendor's own SPI firmware read
all zeros there. With a bitstream rebuilt for the board (100 MHz, SPI1),
this sample reports:

```
spi_flash_id: spi@40013000
spi_flash_id: RDID  = 68 40 15
spi_flash_id: RDSR  = 0x00
spi_flash_id: MFID  = 68 14 (repeated: 68 14)
spi_flash_id: EID   = 14 (expect the 0x90 device ID 14)
spi_flash_id: READ  = ffffffff ffffffff ffffffff (addr 0x00/0x04/0x08, one 0x03 command each)
spi_flash_id: PASS
```

Three independent commands (0x9F / 0x90 / 0xAB) agree, which is what makes
the RX path believable.

## Long transfers (`CONFIG_APP_SPI_LONG_XFER`, default y)

The long-xfer part reads the 8-byte unique ID twice through RX DMA and then
probes the transmit side. Measured on the same bitstream (2026-09-13, SPI1):

```
spi_flash_id: RX-DMA #1 = 45 53 53 95 5b 88 a6 fa
spi_flash_id: RX-DMA #2 = 45 53 53 95 5b 88 a6 fa
spi_flash_id: WREN (1 byte, no DMA) -> RDSR 0x02 (WEL=1)
spi_flash_id: RDID via 8-byte TX (2 phases): ret=0 RDID = 40 15 68
spi_flash_id: 32-byte WREN (register phases) -> RDSR 0x00 (WEL=0) [informational]
spi_flash_id: 36-byte WREN (engine TX DMA) -> RDSR 0x00 (WEL=0) [informational]
```

The last two lines are **not** a TX verdict. This part leaves WEL clear when
`0x06` is followed by filler bytes inside the same CS window, while the
one-byte WREN sets it — so WEL only tells us the flash saw a 1-byte command.
The engine itself transmits correctly:
[`samples/spi_loopback`](../spi_loopback/README.md) captures MOSI through the
fabric and shows 4-byte, 32-byte and 36-byte (engine TX DMA) transfers
matching byte-for-byte, and reading RDID with a padded two-phase TX still
answers `40 15 68` (same ID, one dummy byte shifted in). The *write* path is
covered by [`samples/spi_flash_rw`](../spi_flash_rw/README.md) — a `0x02` +
address + data frame that erases, programs and reads back a 4 KiB sector (and
destroys it, so keep that sample away from this read-only one's flash).

## Build and run

```sh
source <your-venv>/bin/activate
# bitstream first (the board's own bitstream: SPI + its SYSCLK)
bash tools/flash_logic.sh $HOME/example_board.bin
# firmware must match that SYSCLK; a 100 MHz bitstream needs the overlay
cat > /tmp/agm100.overlay <<'EOF'
&clk0 { clock-frequency = <100000000>; };
&cpu0 { clock-frequency = <100000000>; };
&sys  { flash-max-frequency = <50000000>; };
EOF
west build -b agrv2k_407 -d /tmp/b_spi --pristine=always \
    $HOME/zephyr-hal-ag32/samples/spi_flash_id \
    -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash tools/test_uart_capture.sh -t 10 /tmp/b_spi/zephyr/zephyr.bin
```

Nothing here writes to the flash: RDID/RDSR/0x90/0xAB/0x03 are read-only
commands. That matters on a board whose bitstream image lives in the same
flash.
