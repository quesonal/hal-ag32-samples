# fcb_reload

Proves that the **fabric content follows FLASH, not the firmware**: the same
firmware binary is booted twice, with a different bitstream at
`0x800e7000` each time, and the SPI0 RDID the sample reports follows the
bitstream.

It is a probe for the **boot** path, not a reloader: for the runtime (no
reboot) version see `samples/fcb_hotswap` (§3.8.3), which does the same swap
with the CPU parked on HSI for the duration.

**Status: dev board passed 2026-09-14** — trace at the end of "Bench". Bitstream A
`spi_full_bitstream_97pad` gives `FCB boot: STAT=0x000f0002 (ACTIVE)` +
`RDID = 68 40 15 (W25Q16)`; bitstream B `spi_full_bitstream_without_flash`
gives the *same* firmware, the same ACTIVE state, and `RDID = 00 00 00` on
both controllers — the fabric follows FLASH, which is the whole point.

The dev board board's SWD link is still intermittent (`docs/FLASH.md#known-issues`);
`docs/FLASH-AND-CAPTURE.md` §10 and §10.1 cover that and the `oo -o` unlock,
and §3.8.1 explains why the 2026-09-14 "frozen board" reading was really a
15 MHz + v1-HID tooling problem.

```
fcb_reload: bitstream-from-FLASH probe (boot path)
fcb_reload: RST_CNTL=0x10000000
FCB boot: STAT=0x00000002 (ACTIVE)
fcb_reload [boot]: RDID = c8 40 16 (GD25Qxx)
```

## Why this sample reloads the boot way

On AgRV2K the CPU's own `sys_clk`/`bus_clk` are produced **inside the
fabric**: `example_board.bin` carries the PLL and the clock switch that drive
`rv32.sys_clk` (`gclksw_inst|gclk_switch__alta_gclksw__clkout -> rv32|sys_clk`
in the Quartus log). A `DEACTIVATE / AutoConfig / ACTIVATE` sequence therefore
tears down the clock the CPU is running on, and the core stops mid
instruction. Measured 2026-09-14:

* SWD still reads `DPIDR 0x2ba01477`, but every AP access reports
  `stalled AP operation` — openocd `init` fails and the board cannot be
  reflashed over SWD;
* the console is silent (its pins are routed through the fabric too);
* even the ROM bootloader stops answering (`agrv32flash` → "Failed to init
  device"), because it runs on the same frozen core.

An earlier revision of this sample did exactly that on every boot
(`agrv2k_fcb_reload()` + `sys_reboot()` in a loop) and left the board in that
state persistently — a power cycle does not help, because the firmware in
FLASH runs the same sequence again on the next boot.

So this sample's reload path is the boot path: put the image at `0x800e7000` and reboot.
`soc/agm/agrv2k/fcb.c::agrv2k_fcb_program()` streams it into the fabric while
the CPU still runs from HSI, and only afterwards does
`agrv2k_clk_switch_pll()` hand the clock tree to that fabric — the same order
the vendor SDK uses (`FCB_AutoConfigDma`, then `SYS_SwitchPLLClock`).

A genuine hot swap turned out to be exactly that recipe — switch to HSI, reload,
switch back — and it was implemented and measured on 2026-09-14:
`samples/fcb_hotswap` (+ `agrv2k_fcb_reload()`), with the observations in
`the development notes (not published here)` The same-class constraint is the
price: the two images must agree on SYSCLK, FLASH clock class and the console
pin route, and the console drops the few bytes that are in flight during the
~10 ms window.

## Bench

Both bitstreams are 100 MHz, so build with the 100 MHz overlay:

```sh
cd $HOME/zephyrproject
west build -d /tmp/b_fcb_reload -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/fcb_reload -- \
    -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
```

**Run 1 — the engine can reach the flash**

```sh
bash $HOME/zephyr-hal-ag32/tools/flash_logic.sh $HOME/spi_full_bitstream_97pad/example_board.bin
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -n -t 8 \
    /tmp/b_fcb_reload/zephyr/zephyr.bin
```

Measured 2026-09-14:

```
FCB boot: STAT=0x000f0002 (ACTIVE)
fcb_reload: spi@40012000 RDID = 68 40 15
fcb_reload [boot]: RDID = 68 40 15 (W25Q16)
```

(`spi_full_bitstream` — the non-97pad one — does **not** work as run 1: its
fabric patch never delivers MISO to the engine, so both controllers read
`00 00 00`. Use the 97pad or the plain `example_board.bin` bitstream.)

**Run 2 — routed nowhere, same firmware**

```sh
bash $HOME/zephyr-hal-ag32/tools/flash_logic.sh \
    $HOME/spi_full_bitstream_without_flash/example_board.bin
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -n -t 8   # no firmware arg
```

Measured 2026-09-14:

```
FCB boot: STAT=0x000f0002 (ACTIVE)
fcb_reload: spi@40012000 RDID = 00 00 00
fcb_reload: spi@40013000 RDID = 00 00 00
fcb_reload [boot]: RDID = 00 00 00 (nothing answering (MISO held low))
```

Pass = both boots report FCB ACTIVE and the RDID changes with the bitstream
while the firmware is byte-identical (compare the `built` timestamps: they must
be the same run).

The RDID probe accepts either part that has been seen on this dev board
(`C8 40 16` GD25Qxx, `68 40 15` W25Q16) and prints the part name, so a
different flash does not look like a failure; `FF FF FF` means nothing is
answering on SPI0, which is the `_without_flash` bitstream's expected result.

## Commands

| Key | Action |
| --- | --- |
| `s` | re-probe: FCB STAT + SPI0 RDID |
| `q` | halt here — SWD attach point |
| `h` | help |

## Recovery, if a dev board ever wedges the core again

SWD is useless in that state (the CPU has no clock). The way back is the ROM
bootloader, which does not need SWD. It requires the serial-download strap:
**BOOT0 high and BOOT1 low at power-up** (vendor 《硬件设计注意事项》§5/§7).
On the dev board BOOT1 is tied to GND and BOOT0 is a pull-down with a jumper
to 3.3V:

1. fit the **BOOT0** (PIN_94) jumper and power-cycle the board — the ROM
   bootloader takes over instead of the application, so nothing can freeze
   the core. The strap is latched at power-on *or* on a real nRESET: power
   cycle, or run `../../tools/probe_reset_target.py` and start the tool right
   after it (openocd's `reset run` is a soft reset and does **not** re-latch
   BOOT0 — measured);
2. reflash over UART: `agrv32flash -b 57600 -w <zephyr.bin> /dev/ttyACM0`
   (or `west flash --runner agrv32flash`);
3. remove the BOOT0 jumper and reset — the board boots the firmware you just
   wrote.

`docs/FLASH-AND-CAPTURE.md` §10 has the full symptom → cause → recovery table,
including why re-plugging USB on its own does not help. The rung itself was
exercised on the dev board on 2026-09-17 (two different firmwares written over
UART, read back byte-identical, bitstream region untouched).
