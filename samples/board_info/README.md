# board_info

Prints what the running firmware was **built against** — the devicetree,
not the board you think you flashed:

```
board_info: board      = agrv2k_407/agrv2k
board_info: built      = Sep 14 2026 06:12:03
board_info: marker     = base
board_info: SYSCLK     = 100000000 Hz (clk0, from DT)
board_info: cpu0       = 100000000 Hz (from DT)
board_info: flash ceil = 50000000 Hz (sys, from DT)
board_info: enabled AGM nodes: uart 1, spi 2, i2c 0, can 1
```

Why it exists: the most expensive failure mode in this port has been "the
devicetree and the bitstream disagree" — a 100 MHz bitstream with a board
default of 200 MHz produces a garbled console (see
`../docs/FLASH-AND-CAPTURE.md` §4), and a peripheral can be compiled in while
nothing is routed to it. `SYSCLK` here is read from `clk0` in the
devicetree, so it answers "which DT am I running" without an openocd
session.

The `marker` line is meant to be edited by hand (`BOARD_INFO_MARKER` in
`src/main.c`): bump it, rebuild, and if the console still shows the old
value, the image on the board is not the one you just built. Combined with
`built`, that is the cheapest "did my new binary actually reach the board"
check there is.

Read-only: no flash access, no bus traffic, no pin changes.

## Build and run

```sh
cd $HOME/zephyrproject
west build -d /tmp/b_board_info -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/board_info -- \
    -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -t 8 \
    /tmp/b_board_info/zephyr/zephyr.bin
```
