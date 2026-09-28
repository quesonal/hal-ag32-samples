# crc_agm — CRC32/ISO-HDLC on the AgRV2K CRC unit

The AgRV2K has one "crypto-adjacent" block: the CRC calculation unit (reference
manual chapter 16). It is STM32-CRC compatible — `DR`/`IDR`/`CR`/`INIT`/`POL`,
programmable polynomial size (7/8/16/32) and seed, byte/half-word/word input
reversal, bit-level output reversal, 4 AHB clocks per 32-bit feed — and this
sample drives it through `drivers/crc/crc_agm.c`.

## What it is good for

The loader CRCs a fabric slot (`len/crc` from the boot record) and each app
image on the boot path. With Zephyr's software implementation that costs
**13.39 M cycles (~67 ms at 200 MHz) for a 99944-byte slot**; the hardware
does the same bytes in **0.70 M (~3.5 ms)**. No caller changes: enable the
`crc0` node *and* point `zephyr,crc` at it, and Zephyr's `CRC_HW_HANDLER`
replaces the weak software `crc32_ieee()` with the driver-backed one
(`subsys/crc/crc_hardware.c`).

## Running it

```sh
west build -b agrv2k_407 modules/hal_ag32/samples/crc_agm
west flash -d build                 # firmware + bitstream, as usual
bash tools/test_uart_capture.sh     # or any 115200 reader on /dev/ttyACM0
```

Expected output (repeats every 3 s):

```
crc_agm: crc@41002000 ready=1
crc_agm: check "123456789" -> 0xcbf43926 (expect 0xcbf43926) PASS
crc_agm: 99944 B -> 0x3c85bd8e in 701123 cycles (3505 us at 200000000 Hz)
crc_agm: PASS (hardware-speed feed: 701123 cycles <= 2000000)
crc_agm: ALL PASS
```

The verdict is a *dev board* check: `0xCBF43926` is the standard CRC-32/ISO-HDLC
check value, and the cycle ceiling (2 M) is there because a software CRC of
the same buffer takes ~13 M — so "ALL PASS" means the hardware path is really
in use, not just that the number is right.

## Things that had to be measured (all in the driver's comments)

1. **AHB domain, not APB**: clock gate bit 2 of `SYS.AHB_CLKENABLE`, reset
   release bit 2 of `SYS.AHB_RESET` (the vendor example calls
   `SYS_EnableAHBClock(AHB_MASK_CRC0)`). With the APB bit the registers read
   back `0xffffffff`/`0x0` and nothing computes.
2. **Configure, then write `CR |= RESET`**: writing `INIT` alone does not load
   the seed into `DR`.
3. **Keep the feed address in a register**: `sys_write8(buf[i], config->base +
   DR)` makes the compiler reload `config->base` — from XIP flash — on every
   byte (measured 66 cycles/byte vs 7 when hoisted).
4. **The device must be up before `soc.c`'s bring-up**: the fabric verifier
   CRCs a slot from inside it, hence `PRE_KERNEL_1` priority 0 for this driver
   and priority 1 for `soc.c`, plus the AHB gates opening before the FCB
   stream. See `the development notes (not published here)`.

## Boundaries

Only `CRC32_IEEE` (ISO-HDLC) is implemented and measured; CRC32-C, CRC-32/MPEG-2,
the 16/8/7-bit sizes and DMA-fed transfers return `-ENOTSUP`/are untested. A CRC
is not a cryptographic digest — this block buys speed, not security.
