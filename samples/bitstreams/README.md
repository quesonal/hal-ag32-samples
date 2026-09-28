# Canonical bitstream for the samples

The samples are written against one 200 MHz bitstream, but it is **not
redistributed with this repository** (it is generated from the vendor's
reference design and is `.gitignore`d out of the tree here). Keep a copy
on your machine and point the build at it:

```bash
cp $HOME/spi_full_mac_bitstream_200mhz/example_board.bin /tmp/b_hello/zephyr/board.bin
west flash -d /tmp/b_hello
```

Firmware-only iterations (sector erase keeps the fabric):
`west flash -d /tmp/b_hello --skip-bitstream`.

## When it is not the right bitstream

* Samples that need different pin routing carry their own overlay and say
  so in their `sample.yaml` / `README.rst` (the LAN8720, e-paper and
  40-pin LCD samples are the ones to look at first).
* A rebuilt bitstream from `west build -t logic` lands in the build tree
  as `zephyr/board.bin` and is picked up automatically.
* Changing the bitstream's clock changes the UART divisor: a 100 MHz image
  needs matching board clock values, otherwise the console is garbage.
  The clock table and the overlay to use are in
  [`hal_ag32/docs/FLASH-AND-CAPTURE.md`](https://github.com/quesonal/hal-ag32/blob/main/docs/FLASH-AND-CAPTURE.md) §0.3.
