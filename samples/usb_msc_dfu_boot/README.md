# AgRV2K UF2-style USB MSC DFU (bootloader upload channel)

The "copy a file onto a USB drive" upload path for the
[`spi_boot_loader`](../spi_boot_loader/README.rst) DFU, built the way a UF2
bootloader is (RP2040 BOOTSEL, Adafruit nRF52): the board presents a drive,
the host copies a `.uf2` onto it, and the board closes the drive and flashes
the image. It sits next to the loader's console / AN3155 / mcumgr paths (see
[`docs/BOOT-DFU-STATUS.md`](../../docs/BOOT-DFU-STATUS.md) §0.1, the update
section).

This is **not** the Zephyr USB DFU class. The sibling sample
[`usb_msc_dfu`](../usb_msc_dfu/README.md) is the USB-class gadget (driven by
`dfu-util`); this one is the USB **Mass Storage** transport for the loader's
own A/B DFU, driven by a file copy.

## How it works

It is a **payload**: the loader installs it into the on-die application slot
and runs it, like [`spi_boot_app`](../spi_boot_app/README.rst). While it runs:

| Step | Host | Board |
|---|---|---|
| 1 | mounts a 64 MiB FAT16 drive (`AGM-DFU`) | publishes a *fake* FAT volume |
| 2 | copies `FW.UF2` onto the drive | parses each 512 B write as a UF2 block |
| 3 | — | streams each block's payload straight into the store the record does **not** point at |
| 4 | the drive disappears | the last block arrives → closes the MSC interface |
| 5 | — | publishes (`agm_boot_upload_finish`) and reboots; the loader boots the new image |

### The volume is fake, and that is the point

A UF2 bootloader never stores the file. The drive exists only so the host has
something to mount and write to; the writes *are* the transfer. That is why
the volume can claim 64 MiB on a part with 128 KiB of SRAM: nothing backs it.
Concretely, this sample registers its own disk
(`disk_access_register()` + `struct disk_operations`) and:

- returns the generated FAT16 boot sector / first FAT sector
  ([`src/uf2_volume.h`](src/uf2_volume.h), from
  [`../../tools/gen_uf2_volume.py`](../../tools/gen_uf2_volume.py)) for those two
  sectors, and zeroes for the rest of an empty FAT16 volume;
- keeps whatever the host writes to the metadata region (its directory entry
  and FAT chain) in a small RAM overlay so the volume stays coherent if the
  host reads it back;
- treats any write carrying a UF2 block as image data and streams its payload
  into the loader's store with `agm_boot_upload_write()`.

So the image is never held in SRAM as a whole (only the 512 B block being
parsed), there is no file system on the device, and there is no flash staging
area: it goes straight into the target store. The UF2 block's `blockNo` /
`numBlocks` give the ordering and the end-of-image signal, which is what makes
"write finished → close the drive → update" deterministic.

`targetAddr` is a **store offset**, not an absolute flash address, because the
store (A or B) is chosen on the device from the boot record.

## Host side

Convert a built image and copy it:

```
tools/bin_to_uf2.py build/zephyr/zephyr.bin -o FW.UF2
# then, with the board's drive mounted (e.g. E:\ on Windows, /media/x/AGM-DFU):
cp FW.UF2 <mounted drive>/
```

On Windows the drive shows up in Explorer as `AGM-DFU` with ~64 MB free; drag
the `.uf2` onto it. The drive disappears when the copy finishes (that is the
board closing the MSC interface) and the board reboots into the new image.
If the drive does not disappear when the copy dialog closes, the host is still
holding the last block in its cache: eject / "safely remove" the drive to
flush it.

## Build, flash, test

The payload needs the loader to install it, so the bench order is: build and
flash the loader, build this image, install it through any of the loader's
existing paths, boot it, then drive the file copy.

```
source <your-venv>/bin/activate
cd <your-workspace>

# 1. the loader (skip if it is already the one on the board)
west build -b agrv2k_407 --pristine=auto -d /tmp/b_loader \
    modules/hal_ag32/samples/spi_boot_loader
west flash -d /tmp/b_loader --skip-bitstream

# 2. this payload
west build -b agrv2k_407 --pristine=auto -d /tmp/b_uf2 \
    modules/hal_ag32/samples/usb_msc_dfu_boot

# 3. install it through the loader (console path shown; the signed flow uses
#    tools/sign_image.py + agm_upload.py, the locked flow tools/smp_cli.py)
tools/agm_upload.py <port> a /tmp/b_uf2/zephyr/zephyr.bin

# 4. convert whatever image you want to install and copy it onto the drive
tools/bin_to_uf2.py /tmp/b_some_app/zephyr/zephyr.bin -o /tmp/FW.UF2
cp /tmp/FW.UF2 "<mounted drive>/"
```

Console on UART0 (the CMSIS-DAP bridge):

```
 usb_msc_dfu_boot: UF2-style MSC DFU
 volume  : 64 MiB FAT16 (fake, 131072 sectors)
 protocol: copy a .uf2 onto the drive
 ...
drive is up -- copy a .uf2 onto it
receiving UF2: 296 blocks into store B
UF2 complete, closing the MSC interface
PASS: 75608 B in store B, rebooting
```

A UF2 with the wrong family id, a malformed block, an out-of-order block, or an
image larger than the store prints `FAIL: …` and fails the host's write, so a
bad file cannot look like a green run.

## Regenerating the volume metadata

`src/uf2_volume.h` is generated and checked in (like the generated pinctrl
dtsi):

```
tools/gen_uf2_volume.py --out samples/usb_msc_dfu_boot/src/uf2_volume.h
```

## Limits and caveats

* The image is capped by the **store** (`app-size`, 200 KiB by default), not
  by the volume: the volume claims 64 MiB, so an oversized file is accepted by
  the host and then refused at the first block (`FAIL: UF2 holds up to …`).
* The write pattern assumed is "one file, written from the data area in
  order". Metadata writes are buffered so the FAT stays coherent, and data
  writes with no UF2 magic (another file the host left behind) are ignored.
* The family id `0x41474d55` is this sample's own, not an allocated UF2 family.
* Only `agrv2k_407` / the default on-die layout.

## Windows needs an upstream MSC fix

On **Windows** the enumeration stops right after `READ FORMAT CAPACITIES`: the
device answers it (12-byte capacity list, 131072 blocks × 512 B) but the CSW
never arrives, so USBSTOR gives up and no drive appears. Linux is unaffected.

The cause is in Zephyr, not in this sample. `usbd_msc.c:msc_handle_bulk_in()`
stalls the bulk-IN pipe whenever the host asked for more bytes than the command
returned (`Hi > Di`, BOT outside-the-box case 5):

| Host | `READ FORMAT CAPACITIES` asks | command returns | residue | result |
|---|---|---|---|---|
| Linux `sd` | 12 B | 12 B | 0 | no stall → drive works |
| Windows USBSTOR | **252 B** | **12 B** | 240 | **stall** → no CSW → enumeration stops |

The 12-byte response is fixed by the SCSI layer, so nothing in this sample can
avoid it; the host simply asks for different lengths. The fix belongs in
Zephyr and is one hunk: drop that `msc_stall_bulk_in_ep()` call and let the
short packet plus the CSW (which carries the residue) end the data phase. That
is what the upstream patch should do; this module deliberately does not fork
the upstream class. Measured 2026-09-27 on `agrv2k_407` (canonical factory
bitstream): with that one call removed, Windows enumerated the 64 MiB
`AGM-DFU` volume, `spi_boot_app.uf2` copied onto it published into store B
(22604 B, crc `0x331d4ce3`), the drive disappeared, the board rebooted into
`spi_boot_app` and it confirmed itself.
