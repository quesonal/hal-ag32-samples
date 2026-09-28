# dual_ip

Two fabric IPs in one bitstream: the vendor's `custom_ip` **AHB RAM block** and
the **full-duplex SPI IP** that `samples/spi_quad_read` uses.

## Why a wrapper is needed

The AgRV2K fabric attaches exactly **one** IP macro to the MCU. `alta_rv32_top`
has one `mem_ahb_*` slave port, one `slave_ahb_*` master port, one `ext_dma_*`
and one `local_int`; and the SDK's `gen_vlog -m <ip>.v` is a single-valued
option (`etc/gen_vlog:1997`), so one run instantiates one macro
(`docs/CUSTOM-IP.md` 1.5 has the code-level walk-through, including the fact
that `ip_name` itself is parsed as a list but only `ips[0]` is instantiated).

So the two IPs go *inside* one wrapper — `ip/dual_ip.v`, the IP that this
sample declares — which is exactly the vendor's own shape for reuse
(`examples/custom_ip`: an IP that instantiates a sub-IP):

```
                    +--------------------------------------------+
   alta_rv32  ----> | dual_ip.v        (the one macro the fabric |
   mem_ahb_*  <---- |                   instantiates)             |
   slave_ahb_*      |   +----------------+   +------------------+ |
                    |   | full_duplex_spi|   | custom_ip (RAM)  | |
                    |   +----------------+   +------------------+ |
                    +--------------------------------------------+
```

Window split inside the wrapper (the MCU reaches both through
`0x60000000+`, the `cpld0` node):

| offsets | owner |
|---|---|
| `+0x000..0x7FF` (`haddr[15:11] == 0`) | `custom_ip` 2 KiB RAM |
| everything else | `full_duplex_spi`, i.e. the wiring it has as the only IP |

The unselected slave is handed an IDLE transfer instead of the real one, so it
cannot latch a write; only the selected slave drives `hreadyout`/`hrdata`.
`custom_ip`'s pin-driven master port is parked (`ip_pin_out_en/_data` tied low,
so its `ram_trigger` never rises) — this sample only needs the MCU side of the
RAM block.

### The M9K budget (why the RAM is 2 KiB, not the vendor's 4 KiB)

The user-logic LogicLock region (`core_logic`, 20x12 at X43_Y1) has room for
**4 M9K blocks**, and the vendor's own project enforces that
(`set_global_assignment -name MAX_RAM_BLOCKS_M4K 4` — the same line is in
`~/spi-logic/example_board.qsf`). Two IPs have to share it:

| block RAM | size | M9K |
|---|---|---|
| `full_duplex_spi` RX FIFO (`lpm_width 32`, `lpm_numwords 256`) | 8 Kbit | 1 |
| `custom_ip` RAM at the vendor's `RAM_SIZE = 4096` | 32 Kbit | 4 |
| **total, first attempt** | | **5 > 4** |

which is exactly what Quartus reported:

```
Error (170051): You have limited the RAM location(s) of type M9K to 4.
                However, the current design needs more than 4 to successfully fit
Error (171000): Can't fit design in device
```

`RAM_SIZE` is a parameter of the vendor module (`ADDR_BITS` follows it), so the
wrapper instantiates `custom_ip #(.RAM_SIZE(2048))` — 16 Kbit, 2 M9K, the
design sits at 3 of 4 and both IPs stay. The vendor's 4 KiB default does fit,
but only when it is the design's single IP.

## What the sample checks

In this order, each half announcing itself first:

1. **RAM block** (`src/ram_window.c`, `dual_ip_ram_test()`): write/read six
   words spread over the 4 KiB block through the `cpld0` window
   (`agm_cpld_read32/write32`).
2. **SPI** (`src/main.c`, copied from `samples/spi_quad_read` with only the
   `printk` prefix changed): RDID, `0x03`, `0x3B` (dual) and `0x6B` (quad)
   reads of the on-board flash, compared byte for byte. The point is that this
   still passes while a second IP sits in the same fabric.
3. **Decode** (`dual_ip_alias_test()`): write one block higher — into the SPI
   IP's side of the window — and check the RAM did not change. Without the
   window decode both IPs would answer every address; this is the access that
   proves the split, and the one most likely to stall on a bad bitstream, which
   is why it goes last.

WARNING: an access the fabric does not answer never completes (no `hreadyout`,
no fault), so a wrong bitstream hangs instead of printing a failure — the
banners say which half got that far.

Verdict lines: `dual_ip: SPI PASS / RAM PASS / decode PASS` and a final
`dual_ip: PASS (two IPs in one fabric: custom_ip RAM + full_duplex_spi)`.

## Build, synthesize, run

```sh
cd $HOME/zephyrproject
west build -d /tmp/b_dual -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/dual_ip
# -> /tmp/b_dual/logic/{board.v,board.qsf,dual_ip.v,full_duplex_spi.v,
#                       custom_ip.v,ram2ahb.v,ahb2ram.v}
#    Take that directory to a Quartus workstation *as generated*:
quartus_sh -t af_quartus.tcl
#    then, back here (needs simulation/modelsim/board.vo from Quartus):
cp <board.vo> /tmp/b_dual/logic/simulation/modelsim/board.vo
west build -d /tmp/b_dual -t bitstream
AGM_BITSTREAM_BIN=/tmp/b_dual/zephyr/board.bin west flash -d /tmp/b_dual
bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -d /dev/ttyACM0 -n -t 25
```

Program the flash pattern first if the sample reports `INCONCLUSIVE`:
`samples/spi_flash_rw -DCONFIG_APP_FLASH_RW_KEEP_PROGRAMMED=y`.

## What the build already proves (no Quartus needed)

Measured 2026-09-25, `west build` of this sample on the clean tree:

* `board.proj` / `board.qsf` list **one** `macro_inst`'s worth of sources —
  `board.v` plus the wrapper and all four IP files:
  `verilogFiles=board.v, dual_ip.v, full_duplex_spi.v, custom_ip.v, ram2ahb.v,
  ahb2ram.v`;
* `board.v` contains exactly **one** instantiation, `dual_ip macro_inst(` — the
  wrapper, not two macros (the fabric has one slot);
* the port list gen_vlog writes into `dual_ip_tmpl.v` (from the pin map) is
  **identical** (42 names) to the wrapper's own port list, and each sub-IP
  instantiation connects exactly the ports those modules declare
  (42/42 and 41/41, no missing, no extra);
* `tools/build_bitstream.sh` Step 1c (pin constraints must name ports
  `board.v` has) passes, and `west twister -T samples -p agrv2k_407
  --build-only` selects and builds `sample.dual_ip.agrv2k_407` (62
  configurations, 0 failed).

The board result (RAM window + SPI reads) is **not measured yet** — it needs
the Quartus step above.

## Measured on the board (2026-09-25)

Bitstream: Quartus + Supra from the directory above,
`~/agm-logic/board_dual_ip_20260925.bin`, md5
`8b959a9a02bd22cb1fac378820b06241` (99944 B). Quartus fit: **M9K 3** of the
region's budget, 24,576 memory bits (2 KiB RAM + 256x32 FIFO); Supra: 0 errors
/ 0 warnings / 128 infos (the single-IP build has 123).

Capture (`tools/test_uart_capture.sh -d /dev/ttyACM0 -n -t 25`, board
`agrv2k_407`, external NOR holding `spi_flash_rw`'s 323-byte pattern):

```
dual_ip: RAM block (custom_ip, 0x60000000+0x000..0xFFF)
dual_ip: RAM window 0x60000000+0x7ff.. (custom_ip, ip/dual_ip.v)
dual_ip: RAM: 6 word(s) read back -> PASS
dual_ip: SPI IP (full_duplex_spi) four flash reads
dual_ip: spi@40012000
dual_ip: RDID   = 68 40 15
dual_ip: pattern at 000000
dual_ip: 0x03   = a0 a1 a2 a3 ff ff ff ff ff ff ff ff ff ff ff ff
dual_ip: DUAL   = a0 a1 a2 a3 ff ff ff ff ff ff ff ff ff ff ff ff
dual_ip: QUAD   = a0 a1 a2 a3 ff ff ff ff ff ff ff ff ff ff ff ff
dual_ip: spi@40012000 -> PASS (0x03, 0x3B and 0x6B agree byte for byte)
dual_ip: RAM: +0x800 does not alias into the block -> PASS
dual_ip: SPI PASS / RAM PASS / decode PASS
dual_ip: PASS (two IPs in one fabric: custom_ip RAM + full_duplex_spi)
```

So: the SPI IP behaves byte for byte as it does alone, the second IP is
reachable, and the window decode is real (`+0x800` did not land in the RAM).

## Provenance of the RTL

| file | from | md5 |
|---|---|---|
| `ip/dual_ip.v` | this repo (Apache-2.0) | `03538a3a…` |
| `ip/full_duplex_spi.v` | vendor SPI example (`examples/spi/full_duplex_spi`), same file as `samples/spi_quad_read/ip/` | `9eccf7021cc66d5a7099054a03531849` |
| `ip/custom_ip.v` | vendor `examples/custom_ip/ip/logic/` | `e3d9921b813f59af60ae77545619d7ef` |
| `ip/ram2ahb.v` | vendor `examples/custom_ip/ip/logic/` | `afc174fbd1a45bf2bbca6f5786698cfc` |
| `ip/ahb2ram.v` | vendor `examples/custom_ip/ip/logic/` | `667767706a36a6e35247a39f0b0ca3d2` |

The vendor files are unmodified copies (only the file mode differs), so the
board result stays comparable with the vendor example's own test.
