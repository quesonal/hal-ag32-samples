# cpld_reg — CPLD (FPGA fabric) AHB window

Demonstrates MCU-side access to the on-die FPGA fabric through the
`cpld0` devicetree node (`compatible = "agm,agrv2k-cpld"`) and the module's
`cpld_agm` driver (`include/zephyr/drivers/misc/cpld_agm.h`).

## What the hardware does

A load or store anywhere in `0x60000000-0x7FFFFFFF` is decoded by the SoC
and forwarded to the bitstream's `mem_ahb_*` slave port; the fabric
answers through the AHB `hreadyout` handshake. The register map *inside*
the window belongs to the user's Verilog (vendor examples:
`example_cpldAhbTxRxReg`, `example_adcAndSpi`, ...), not to the SoC, which
is why the devicetree describes only the window and the driver only offers
32-bit, bounds-checked access.

**An access the fabric does not answer never completes.** With the
shipped `example_board.bin` (no fabric slave at `0x60000000`) the first
read stalls the CPU with no fault and no console output. Flash a
bitstream with a slave before running this. The driver's bounds check
catches offsets outside `reg`; it cannot tell whether the fabric decodes
an offset inside the window.

## What the sample does

1. `agm_cpld_get_base()` / `agm_cpld_get_size()` — print the window the
   devicetree hands out.
2. Read-only dump of the first 8 words.
3. Negative case: `agm_cpld_read32(cpld, size, ...)` must return `-EINVAL`
   (the driver range-checks against `reg`), so a mis-sized window shows up
   as an error instead of a hang.
4. `CONFIG_APP_CPLD_ECHO_TEST=y` — three rounds of "write `+0x00`, read
   `+0x04`", the register pair implemented by the vendor
   `example_cpldAhbTxRxReg` bitstream (`logic/analog_ip.v`:
   `hrdata_reg <= hwdata_reg`). Off by default because on the ADC/SPI
   examples those offsets drive real hardware.

## Build and run

```sh
source <your-venv>/bin/activate
west build -b agrv2k_407 -d /tmp/b_cpld_reg --pristine=always \
    $HOME/zephyr-hal-ag32/samples/cpld_reg
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh \
    /tmp/b_cpld_reg/zephyr/zephyr.bin
```

The vendor echo bitstream runs at SYSCLK 100 MHz, so a 407 build needs a
temporary clock profile to match (otherwise the console baud rate is off
by the 200→100 ratio):

```sh
cat > /tmp/agm100.overlay <<'EOF'
&clk0 { clock-frequency = <100000000>; };
&cpu0 { clock-frequency = <100000000>; };
&sys  { flash-max-frequency = <50000000>; };
EOF
bash tools/flash_logic.sh \
    "$HOME/联合编程/example_cpldAhbTxRxReg/example_cpldTxRxReg/logic/example_board.bin"
west build -b agrv2k_407 -d /tmp/b_cpld_100 --pristine=always \
    samples/cpld_reg -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay \
    -DCONFIG_APP_CPLD_ECHO_TEST=y
bash tools/test_uart_capture.sh -t 10 /tmp/b_cpld_100/zephyr/zephyr.bin
# restore afterwards
bash tools/flash_logic.sh $HOME/example_board.bin
```

`boards/agrv2k_407.overlay` turns the node on for this board only; the
SoC-level node is `status = "disabled"`, like `can0`.

## Shell (CONFIG_CPLD_AGM_SHELL)

Build with `-DCONFIG_SHELL=y -DCONFIG_CPLD_AGM_SHELL=y` to poke the window
from the console — handy when the register map is only known from the
Verilog:

```
uart:~$ cpld info
cpld@60000000: base 0x60000000, size 0x8000
uart:~$ cpld dump 0 4
+0x0000: 0x00000000
+0x0004: 0x00000000
+0x0008: 0x00000000
+0x000c: 0x00000000
uart:~$ cpld write 0 0xdeadbeef
+0x0000 <- 0xdeadbeef
uart:~$ cpld read 4
+0x0004: 0xdeadbeef
uart:~$ cpld read 0x9000
read +0x9000 failed (-22)
```

## Expected output

```
cpld_reg: AgRV2K CPLD fabric window
cpld_reg: cpld@60000000: base 0x60000000, size 0x8000
cpld_reg: read-only dump (8 words, window-relative offsets)
cpld_reg:  +0x0000: 0x00000000
...
cpld_reg: bounds: read(+0x8000) -> -22 (PASS: rejected)
cpld_reg: echo self test skipped (CONFIG_APP_CPLD_ECHO_TEST=n)
cpld_reg: PASS
```

Zephyr names the device after the devicetree node (`cpld@60000000`). Use
`DT_NODELABEL(cpld0)` in code rather than assuming any instance numbering.

Measured on agrv2k_407 with the vendor echo bitstream (2026-09-13): the
eight dumped words read 0, `read(+0x8000)` returned -22, all three echo
rounds read back their own pattern (`cpld_reg: PASS`), and the shell
commands above behaved as shown. The full capture is in `the development notes (not published here)`.

See those notes for the fabric side, the bitstream prerequisites and what is
still missing (CPLD-to-GPIO
interrupts, DMA handshake, carrying user Verilog into the bitstream build).
