# AgRV2K USB Host Enumeration

USB0 host-mode sample for the `agrv2k_407` board: the board acts as a USB
host and enumerates whatever full-speed device is plugged into the USB0
connector. It is the host-side counterpart of `usb_cdc_echo` / `usb_hid_mouse`
and exercises `drivers/usb/uhc/uhc_agm.c` plus the Zephyr USB host stack
(`subsys/usb/host`).

## What it does

`usbh_init()` + `usbh_enable()` bring the controller up in host mode. On
connect the host stack runs the normal enumeration sequence (bus reset,
`GET_DESCRIPTOR(8)`, `SET_ADDRESS`, `GET_DESCRIPTOR(18)`,
`SET_CONFIGURATION`) through the driver's EP0 queue head. The sample registers
one host class with an *empty* filter table, so it is offered every device
that finishes enumeration -- no class driver required -- and prints the
device descriptor:

```
Device connected, full speed          <- uhc_agm
PASS: enumerated a USB device         <- this sample
  address   1
  speed     2
  VID:PID   xxxx:yyyy
  ...
device removed
```

## Why it needs a new driver

USB0 previously had only the device-mode `udc_agm` driver (binding
`agm,agrv2k-usb0`, `usb-ep.yaml`). Host mode needs a host controller driver;
`drivers/usb/uhc/uhc_agm.c` is that driver, ported from the vendor's own
host stack (TinyUSB's ChipIdea/EHCI host port) to the Zephyr UHC API.

USB0 is one piece of silicon with one role at a time, so the devicetree has
two mutually exclusive nodes: `&usb0` (device) and `&uhc0` (host). This
sample's overlay disables `&usb0`, enables `&uhc0` and labels it
`zephyr_uhc0`. Enabling both nodes in one build is a bug (two drivers, one
interrupt line).

## Build

```
source <your-venv>/bin/activate
cd <your-workspace>
west build -b agrv2k_407 -d /tmp/b_usbhost --pristine=auto \
    modules/hal_ag32/samples/usb_host_enum
```

## Flash

`west flash` writes firmware + bitstream by default. **The bitstream must
present USB0 host mode** (or OTG, which the driver overrides with
`USBMODE.CM = host`); the canonical `example_board.bin` is the one the other
USB samples use.

```
west flash -d /tmp/b_usbhost
```

## Test

1. Build and flash.
2. Open the console (`tools/test_uart_capture.sh`) -- the banner ends with
   `USB0 host is up`.
3. Plug a full-speed USB device (a USB stick or keyboard is fine; no class
   driver is needed at this stage) into the USB0 connector.
4. Expect the connect line, then `PASS: enumerated a USB device` and the
   descriptor fields. `lsusb` on the debug host is unrelated -- this is the
   AgRV board enumerating the device, not the other way round.
5. Unplug: `device removed`.

If nothing appears, check that the bitstream is in host mode and that the
device is full speed (the block has no high-speed PHY).

## Files

- `src/main.c`          host class (empty filter) + `usbh_init/enable`
- `boards/agrv2k_407.overlay`  disables `&usb0`, enables `&uhc0` as `zephyr_uhc0`
- `prj.conf`            host stack + logging
- `sample.yaml`         west metadata (`integration_platforms: agrv2k_407`)

## Status

**Measured 2026-09-26 on `agrv2k_407`** (canonical 200 MHz bitstream, md5
`6378549f3a8f82dd386353077f3d4a02`) with a Lenovo wireless mouse receiver
(VID:PID `17ef:6215`) plugged into USB0: full enumeration (8/18/9/34-byte
descriptor reads, SET_ADDRESS, SET_CONFIGURATION), `PASS`, one HID interface
found. Three consecutive flash-and-capture runs were clean; see the `HISTORY`
block in `drivers/usb/uhc/uhc_agm.c` for the register read-backs, the three
bugs the bench exposed, and what is still uncovered (bulk/interrupt traffic,
hubs, isochronous).
