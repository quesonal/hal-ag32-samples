# fcb_hotswap

Reload the FPGA fabric **at runtime, without rebooting**. `samples/fcb_reload`
proves the fabric content follows FLASH (bitstream swap + reboot); this sample
does the swap while the CPU keeps executing.

**Status: dev board passed 2026-09-14, and re-passed 2026-09-24** at the current
slot addresses (DT partitions, slot B = fabric update slot 2 `0x800cd000`) —
both directions plus the rejection path, transcripts at the end. Board state
after the run: slot A = 97pad bitstream, slot B = `_without_flash` bitstream,
fabric back on slot A.

## The sequence

The obstacle is that on AgRV2K the CPU's own `sys_clk` comes *out of the
fabric* (`gclksw_inst|gclk_switch__alta_gclksw__clkout -> rv32|sys_clk` in the
Quartus log), so a bare `DEACTIVATE / AutoConfig / ACTIVATE` stops the core
mid-instruction. The fix is the same trick the boot path uses — run from a
clock the fabric does not own for the duration:

| Step | Call | Why |
| --- | --- | --- |
| 1 | `slot_verify()` (src/slot.c) | CRC-32/BZIP2 + IDCODE over the whole image. The FCB only checks at ACTIVATE, i.e. after the old fabric is already gone; failing here costs nothing |
| 2 | `agrv2k_clk_switch_hsi()` | move the CPU onto the on-die RC oscillator, the only clock that survives the fabric going away |
| 3 | `agrv2k_fcb_reload()` (soc/agm/agrv2k/fcb.c) | `DEACTIVATE` → stream 24986 words → `ACTIVATE` → STAT readback + error clear |
| 4 | `agrv2k_clk_switch_pll()` + `pinctrl_apply_state()` | hand the clock tree and the pin routes (AFSEL/DIR are re-latched from the new bitstream) back to the new fabric |

### The bitstream checksum

A Supra bitstream is 24986 words (99944 bytes). The last word is
**CRC-32/BZIP2** (poly `0x04C11DB7`, init `0xFFFFFFFF`, no bit reflection,
final xor `0xFFFFFFFF`) over bytes 0..99939, stored **big-endian**: the
`example_board.bin` tail ends `… 0f 8f 9e ae 72 87`, and `0x9eae7287` is that
CRC. Measured against three images
(`<build_dir>/zephyr/board.bin`, `~/spi_full_bitstream_97pad/example_board.bin`,
`~/spi_full_bitstream_without_flash/example_board.bin`) — including two that
differ in 3137 words — so it is not a constant that happens to match one file.
`slot_verify()` implements it and is what the "rejected" row below is.

## Bench

Two **same-class** bitstreams are required: same SYSCLK (both 100 MHz here),
and the same console pin route — the UART pins they disagree about would take
the console with them. The two SPI bitstreams are exactly that: they differ in
whether the fabric routes SPI0 to the flash, which is also what makes the
swap visible.

```sh
HAL=$HOME/zephyr-hal-ag32
cd $HOME/zephyrproject

# 1. build (100 MHz profile is in the sample's own board overlay)
west build -d /tmp/b_hotswap -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/fcb_hotswap

# 2. slot A (the boot slot): 97pad bitstream — routes SPI0 to the flash
bash $HAL/tools/flash_logic.sh $HOME/spi_full_bitstream_97pad/example_board.bin

# 3. firmware. NOTE: this erases the whole firmware region, i.e. slot B too,
#    so it must come *before* slot B is written.
bash $HAL/tools/test_uart_capture.sh -t 30 /tmp/b_hotswap/zephyr/zephyr.bin

# 4. slot B (the spare): never booted, written without touching the boot
#    pointer (agm_oo.sh fw = oo -a <addr> -w <bin>). The address is the
#    sample's `hotswap_slot_b` partition (fabric update slot 2); the
#    transcripts below were taken at the old 0x800c0000 -- see the note there.
bash $HAL/tools/agm_oo.sh fw \
    $HOME/spi_full_bitstream_without_flash/example_board.bin 0x800cd000
```

Then drive it over the console (115200 8N1 on `/dev/ttyACM0`). Two ways:

* **Automated** — `tools/test_fcb_hotswap.sh` does steps 1–4 above plus the
  key sequence, and asserts the FCB reload rc=0 in both directions and that
  the SPI NOR RDID moved with the fabric. Default round trip; the board
  ends on slot A. This is the recipe used to confirm the 2026-09-14
  measurements (window 9–10 ms, RDID `68 40 15` ↔ `00 00 00`, FCB STAT
  `0x000f0002` both sides):

  ```sh
  HAL=$HOME/zephyr-hal-ag32
  bash $HAL/tools/test_fcb_hotswap.sh \
      --slot-a-bs $HOME/spi_full_bitstream_97pad/example_board.bin \
      --slot-b-bs $HOME/spi_full_bitstream_without_flash/example_board.bin
  ```

* **Manual** — for when you want to see the bytes go past on a terminal.
  `test_uart_capture.sh` cannot send keys, so open the port by hand:

  ```sh
  stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -crtscts raw -echo
  ( timeout 25 cat /dev/ttyACM0 > /tmp/hotswap.log ) &
  sleep 1.5; printf 'x' > /dev/ttyACM0   # swap A -> B
  sleep 6;   printf 'x' > /dev/ttyACM0   # swap back B -> A
  wait
  ```

| Key | Action |
| --- | --- |
| `x` | hot swap to the other slot (validated first) |
| `i` | HSI round trip only — no fabric change (isolates step 2) |
| `s` | probe: FCB STAT + SPI RDID + both slot verdicts |
| `q` | halt here — SWD attach point |
| `h` | help |

## Measured 2026-09-14

> **Re-run at the current address (2026-09-24)**: with the slots coming from
> the devicetree (`hotswap_slot_a` = `0x800e7000`, `hotswap_slot_b` =
> `0x800cd000`) the automated flow passes end-to-end:
> `tools/test_fcb_hotswap.sh --slot-a-bs <97pad> --slot-b-bs <without_flash>
> --expected-rdid "00 00 00"` →
> `RESULT: PASS`, boot RDID `68 40 15`, `slot B @0x800cd000 spare: ok`,
> both swaps `FCB reload completed (rc=0), window 9 ms`, RDID
> `68 40 15 → 00 00 00 → 68 40 15`, board left on slot A. The board was then
> restored to the canonical 200 MHz bitstream + `hello_world` and checked
> (banner 100 %, option bytes and the bind-salt sector unchanged, the fabric
> record sector still erased).
>
> The transcripts below are the original 2026-09-14 run, taken while slot B
> defaulted to `0x800c0000`. That address overlaps the fabric slots, the
> bind-salt sector and the bitstream A/B record, which is why the default
> moved; the numbers are kept as measured. See docs/FLASH-LAYOUT.md §8.

Boot, with both slots written (`i` and `x` were driven as above):

```
=== fcb_hotswap: runtime fabric reload (no reboot) ===
hotswap: sys pll=100000000 Hz flash_max=50000000 Hz rst=0x00000000 apb=0x04201f1d
hotswap: slot A @0x800e7000 boot slot  : ok (idcode=0x01002040)
hotswap: slot B @0x800c0000 spare      : ok (idcode=0x01002040)
hotswap: spi@40012000 RDID = 68 40 15
FCB boot: STAT=0x000f0002 (ACTIVE)
hotswap [boot]: uptime=71 ms rst=0x00000000 apb=0x04201f1d live=0x800e7000 rdid=68 40 15 (W25Q16)
```

Swap A → B (`x`):

```
hotswap: swap #1: 0x800e7000 -> 0x800c0000 (slot B)
hotswap: slot B verified (idcode=0x01002040, crc ok)
hotswap: sys_clk -> HSI (CPU off th<0x80>          <- the console loses the bytes in flight
hotswap: FCB reload completed (rc=0), window 10 ms
hotswap: spi@40012000 RDID = 00 00 00
hotswap: spi@40013000 RDID = 00 00 00
FCB post: STAT=0x000f0002 (ACTIVE)
hotswap [post]: uptime=40723 ms rst=0x00000000 apb=0x04201f1d live=0x800c0000 rdid=00 00 00 (nothing answering (MISO held low))
hotswap: slot B is live; uptime kept counting and no boot banner appeared, so the core was never reset — that is the hot swap
```

Swap B → A (`x` again): `window 9 ms`, `spi@40012000 RDID = 68 40 15`,
`live=0x800e7000`, `uptime=70429 ms`. So the fabric follows the slot in both
directions, the FCB comes back `ACTIVE` each time, and the ~30 s between the
two swaps is the same uptime that started at boot — no reset, no banner.

Rejection path — slot B overwritten with a 4 KB slice of the *other* bitstream
(`dd` + `agm_oo.sh fw … 0x800c0000`):

```
hotswap: slot B @0x800c0000 spare      : checksum mismatch
hotswap: swap #1: 0x800e7000 -> 0x800c0000 (slot B)
hotswap: slot B rejected (checksum mismatch) — fabric untouched, still on 0x800e7000
```

HSI round trip (`i`), which is what makes the swap possible at all: the core
keeps executing on HSI (the console is garbage for the duration because the
UART's clock divides from SYSCLK, and it comes back clean as soon as the PLL
is re-selected), and the kernel's uptime accounting is distorted inside that
window — measured, not explained. Keep the window printk-free.

## Constraints and recovery

* **Same clock class** — SYSCLK, FLASH clock class and console pin route. A
  mismatch does not brick the board, but the console comes back as garbage
  (`docs/FLASH-AND-CAPTURE.md` §4).
* **Both slots are devicetree partitions now** (`boards/agrv2k_407.overlay`:
  `hotswap_slot_a` = the factory region `0x800e7000`, `hotswap_slot_b` = the
  fabric update slot 2 `0x800cd000`), and `src/main.c` reads them with
  `DT_REG_ADDR()` plus a `BUILD_ASSERT` against `AGM_BITSTREAM_FACTORY_ADDR` /
  `AGM_BITSTREAM_SLOT2_ADDR`. The old Kconfig hex knobs are gone (2026-09-24).
  Slot A must stay `0x800e7000` — that is what the FLASH option byte points at;
  slot B is a fabric slot, so a `flash_fw.sh` / `test_uart_capture.sh <fw>` run
  (or a loader bitstream update) still changes what is in it: re-write slot B
  before a run.
* **Deferred: checksum-mismatch policy** — today a CRC-32/BZIP2 mismatch on
  the candidate image is logged once and the swap is refused. The failure
  path is silent (no retry, no fallback to slot A, no diagnostic beyond the
  log line). Decide whether that should fall back to the last-known-good
  slot, queue the candidate for an interactive CLI, or stay silent. Not done
  — the immediate need was to prove the swap path is safe; the policy is a
  product question, not a driver question.
* **The failure mode is the expensive one**: if the fabric never comes back,
  the console, the SWD AP and the ROM bootloader all stop answering (they run
  on the same core). Recovery is the ROM bootloader over UART with the
  serial-download strap -- BOOT0 high **and** BOOT1 low **at power-up**
  (dev board: jumper BOOT0/PIN_94 to 3.3V, BOOT1 is already at GND;
  power-cycle, or `tools/probe_reset_target.py` for a real nRESET pulse
  followed immediately by the write; `docs/FLASH-AND-CAPTURE.md` §10):
  `agrv32flash -b 57600 -w <zephyr.bin> /dev/ttyACM0`. That is why the image
  is validated *before* `agrv2k_fcb_reload()` and why nothing in the module
  calls it by itself.
* Peripherals you keep using across the swap have to be same-class too; the
  sample re-asserts only the pin routes (`pinctrl_apply_state`), because the
  clock tree and register contents of the hard-macro peripherals survive.
