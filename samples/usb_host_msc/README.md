# AgRV2K USB Host Mass Storage (BOT/SCSI)

USB0 host-mode sample for the `agrv2k_407` board that talks to a USB card
reader or flash drive: it runs the Bulk-Only Transport state machine, issues
SCSI `INQUIRY`, `READ CAPACITY(10)` and `READ(10)` for LBA 0, and prints the
device's strings, the card's size and the first bytes of the boot sector.

## Why the class lives in the sample

This Zephyr revision ships **no host MSC class** — `subsys/usb/host/class/`
contains UVC only — so the sample implements the BOT/SCSI part itself. It uses
public APIs only:

* `uhc_xfer_alloc_with_buf()` / `uhc_ep_enqueue()` for the transfers (the
  completion callback drives the CBW → data → CSW state machine);
* the `usbh` class API (`init`/`probe`/`removed`) for the device lifecycle;
* `udev->ep_in[]`/`ep_out[]`, which the host stack filled in while parsing the
  configuration, to find the bulk endpoints.

`usb_host_enum` covers enumeration (control transfers) only; **this is the
sample that exercises the driver's bulk path**.

## Output

```
mass storage device: VID:PID 14cd:1212, bulk IN 0x81 (mps 64) OUT 0x02
  vendor      Mass
  product     Storage Device
  revision    1.00
  block size  512 bytes, last LBA 122138623
  capacity    62534975488 bytes (59638 MiB)
  sector 0
00 00 00 00 ...  (16 bytes)
PASS: read TF card through the USB card reader
```

The PASS verdict requires a CSW with status 0 plus a valid `0x55AA` boot-sector
signature in sector 0, so "card missing" or "read failed" cannot show up as a
blank-but-green run (a missing card makes `READ CAPACITY`/`READ(10)` fail, and
the sample prints `FAIL`).

## Build, flash, test

```
source <your-venv>/bin/activate
cd <your-workspace>
west build -b agrv2k_407 -d /tmp/b_msc --pristine=auto \
    modules/hal_ag32/samples/usb_host_msc
west flash -d /tmp/b_msc            # add --skip-bitstream to keep the fabric
```

Plug the reader into the USB0 connector before (or after) reset — the sample
keeps watching the port. The bitstream must present USB0 in host mode (or OTG,
which the driver overrides with `USBMODE.CM = host`).

## Measured

2026-09-26, `agrv2k_407`, canonical 200 MHz bitstream
(`example_board.bin`, md5 `6378549f3a8f82dd386353077f3d4a02`), a generic
`14cd:1212` USB 2.0 SD/MMC reader with a 64 GB TF card: `INQUIRY` →
`"Mass Storage Device" 1.00`, `READ CAPACITY(10)` → 512-byte blocks,
last LBA 122138623 (≈58.2 GiB), `READ(10)` LBA 0 → 512 bytes with a valid
`0x55AA` signature. Bulk IN runs at mps 64 (eight packets for one sector).

Read/write performance, 32 blocks (16 KiB) per SCSI command:

| Test | Result |
|---|---|
| Read 4 MiB (256 commands) | **1082–1089 KiB/s** (≈1.07 MB/s) |
| Write 256 KiB (16 commands, same region) | **992 KiB/s** (≈0.97 MB/s) |
| Read-back verify / restore | ✅ pattern matched; original content put back **and read back again** |

That is the full-speed bulk ceiling: 16 KiB takes ~14.8 ms, i.e. ~58 µs per
64-byte packet, or about 17 packets per millisecond. (Before the driver's ISR
acknowledge order was fixed the same test measured only 170 KiB/s — see the
driver's `HISTORY`.)

The sample prints `FAIL` and stops if a CSW reports an error, if the
read-back does not match, or if the transfer times out; the card's original
content is restored on the way out and then verified with one more read.

### The one region it rewrites

The write test touches **LBA 2048..2079** (16 KiB, starting 1 MiB into the
card) and nothing else. It saves that region to RAM first, writes the pattern,
reads it back, then writes the snapshot back and reads it back again — so a
clean run leaves the card unchanged and the `PASS` line is backed by a
comparison, not by the assumption that the restore write landed. The residual
exposure is a power cut between the write and the restore: treat LBA
2048..2079 on a card you use for the bench as scratch space.

If the device is unplugged while a transfer is in flight, the driver reports
it with `-ESHUTDOWN`; the completion callback frees its transfer and the state
machine stops (the class's `removed()` callback only marks the device gone —
the two callbacks run on different host-stack threads, in either order).

## Files

- `src/main.c` — BOT/SCSI class, state machine, reporting
- `boards/agrv2k_407.overlay` — disables `&usb0`, enables `&uhc0` as `zephyr_uhc0`
- `prj.conf` — host stack, logging, UHC buffer pool sized for a 512 B sector
- `sample.yaml` — west metadata (`integration_platforms: agrv2k_407`)
