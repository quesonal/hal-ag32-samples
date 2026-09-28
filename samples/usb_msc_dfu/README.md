# AgRV2K USB Composite MSC + DFU

USB0 device-mode sample for the `agrv2k_407` board that presents **one
full-speed USB device carrying two functions at once**:

* an **MSC** (Mass Storage, Bulk-Only Transport) interface, so the host sees a
  removable raw block device;
* the **DFU** class (runtime interface at first, DFU mode after a detach), so
  `dfu-util` can upload or download the image.

Both functions serve **the same in-RAM disk** (`zephyr,ram-disk`, 128 × 512 B =
64 KiB): the legal unit the host mounts over MSC and the DFU image
`ramdisk0` are the same 128 sectors. Write a sector over one path and it
reads back over the other.

## What it demonstrates

* Two class instances on one configuration (MSC needs two bulk endpoints, DFU
  needs only EP0) — the first multi-class composite on this port.
* The runtime-interface → DFU-mode transition (`usbd_shutdown()` then a fresh
  context) on the AgRV2K controller, the same flow the upstream
  `samples/subsys/usb/dfu` uses.
* One backing store exported over two independent USB classes.

Only the build gate has been run so far (see *Not done / caveats*); everything
below that names a host tool is the bench procedure, not a measured result.

## Host-side view (expected)

In runtime mode:

```
$ lsusb
... ID 2fe3:0007 ... AgRV2K USB MSC+DFU

$ lsblk              # a new 64 KiB, read-only-looking raw disk appears, e.g.
NAME   MAJ:MIN RM  SIZE RO TYPE MOUNTPOINT
sdb      8:16   1   64K  0 disk

$ sudo dd if=/dev/sdb bs=512 count=1 2>/dev/null | hexdump -C | head
00000000  00 00 00 00 ...                          # whatever is in the RAM disk
```

The disk is raw (no file system), which is what a bootloader's "force-flash"
volume looks like; the point is the block device, not a mountable FAT.

To update the image over DFU:

```
$ dfu-util --detach                       # enters DFU mode
$ dfu-util -a 0 -D firmware.bin           # download into the "RAM" disk
$ dfu-util -a 0 -U readback.bin           # (reset, detach again) upload it back
```

After `dfu-util --detach` the device re-enumerates with only the DFU-mode
interface; the MSC disk is gone until the board is reset.

## Console verdict

The console stays on **UART0** (the CMSIS-DAP bridge, `ttyACM0` on the host),
so the samples' PASS/FAIL lines are readable independently of the USB0 data
path. On a clean DFU download the firmware prints:

```
<inf> main: USBD message: DFU detach request
<inf> main: Detach: switching to DFU mode
<inf> main: DFU mode: `dfu-util -a 0 -D <image>` to download, -U to upload
<inf> main: USBD message: DFU download completed
<inf> main: PASS: DFU download completed, 4096 bytes in disk "RAM"
```

A download that manifests with zero bytes prints `FAIL: DFU download
completed with 0 bytes`, so a "no data actually moved" run cannot look green.

## Build, flash, test

```
source <your-venv>/bin/activate
cd ~/zephyrproject/zephyr
west build -b agrv2k_407 -p auto \
    -d ~/zephyr-hal-ag32/samples/usb_msc_dfu/build \
    ~/zephyr-hal-ag32/samples/usb_msc_dfu/
west flash -d ~/zephyr-hal-ag32/samples/usb_msc_dfu/build   # or --skip-bitstream
```

Plug a USB host into the board's USB0 connector, then follow **Host-side
view** above. The UART0 console is the debug bridge the probe already
provides.

### Bench checklist

1. Runtime mode: `lsusb` shows `2fe3:0007 AgRV2K USB MSC+DFU`; a new raw block
   device appears and `dd` can read sector 0.
2. `dfu-util --detach` → console prints the detach sequence and the device
   re-enumerates in DFU mode.
3. `dfu-util -a 0 -D <image>` → console prints
   `PASS: DFU download completed, <N> bytes in disk "RAM"`.
4. Reset, re-enter DFU mode, `dfu-util -a 0 -U <out>` → `<out>` matches what
   was downloaded (same bytes, same length).

## Files

* `src/main.c` — MSC LUN, DFU image backend over the ramdisk, runtime →
  DFU-mode switch, console PASS/FAIL
* `boards/agrv2k_407.overlay` — the shared `ramdisk0` (`zephyr,ram-disk`)
* `prj.conf` — device-next stack, MSC + DFU classes, disk access, logging
* `Kconfig` — pulls in the upstream `SAMPLE_USBD_*` options
* `CMakeLists.txt` — includes the upstream USB sample common code (no fork)
* `sample.yaml` — west metadata (`integration_platforms: agrv2k_407`)

## Not done / caveats

* Only the build gate (`west twister -T samples -p agrv2k_407 --build-only`)
  has been run for this sample; the on-board run above is the bench procedure,
  not a measured result.
* Only `agrv2k_407` is wired up; the 103/303/test boards have their own USB0
  bitstream/fabric story.
* The disk is raw SRAM; a power cycle wipes it.
