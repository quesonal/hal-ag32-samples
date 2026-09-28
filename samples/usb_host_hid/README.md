# AgRV2K USB Host HID (interrupt IN)

USB0 host-mode sample for the `agrv2k_407` board that polls the interrupt IN
endpoint of a HID device — a mouse, a keyboard, or the wireless receiver a
mouse ships with — and prints every report it receives.

## What it does

The host stack offers every enumerated function to each registered class; this
sample's `probe` screens for a HID interface (`bInterfaceClass == 0x03`), takes
the interrupt IN endpoint out of the descriptors the stack already parsed
(`udev->ep_in[]`), and then keeps **exactly one transfer armed** on that
endpoint forever. Each completion prints the report as hex and re-arms.

An interrupt IN endpoint NAKs while its device has nothing to report — that is
the normal idle state, and the transfer simply stays on the wire until the
mouse moves or a key is pressed. So the sample prints a hint and waits; there
is nothing to poll from software.

When the device is unplugged the driver reports the transfer that was still
armed with `-ESHUTDOWN`. The completion callback and the class's `removed()`
callback run on different host-stack threads, so either can run first: the
sample therefore only marks the device gone in `removed()`, lets the
completion callback free the transfer, and stops re-arming. Plugging the
device back in re-probes and starts a fresh poll.

## Why it exists

`usb_host_enum` only exercises control transfers and `usb_host_msc` only bulk
ones. **This is the sample that covers the driver's periodic schedule**: the
frame list built at init, the 1/2/4/8 ms period heads, the queue head inserted
there according to the endpoint's `bInterval`, and `scan_periodic()`'s
completion walk in the ISR.

## Output

```
HID device: VID:PID 17ef:6215, interrupt IN 0x81 (mps 8, bInterval 1)
polling -- move the mouse or press a key to generate reports
report 1 (7 bytes):
02 00 bc ff fe 00 00                             |.......
...
report 4 (7 bytes):
02 00 ff ff ff 00 00                             |.......
PASS: 4 interrupt IN reports received (ep 0x81, mps 8, bInterval 1)
```

The report layout is whatever the device uses (this receiver sends 7 bytes:
a report id, buttons, relative X, relative Y, wheel — visible in the log as
`02 00 bc ff fe 00 00`, i.e. X −68, Y −2). The sample does not decode it on
purpose: it is a transport test, not a HID parser.

Reports are printed at `LOG_INF` and the log is synchronous, so the printed
rate is limited by the 115200 console, not by the bus. Raise the baud or drop
the per-report line if you need to measure the polling rate itself.

## Build, flash, test

```
source <your-venv>/bin/activate
cd <your-workspace>
west build -b agrv2k_407 -d /tmp/b_hid --pristine=auto \
    modules/hal_ag32/samples/usb_host_hid
west flash -d /tmp/b_hid            # add --skip-bitstream to keep the fabric
```

Plug the mouse/receiver into USB0 and **move the mouse** while capturing the
console (`tools/test_uart_capture.sh -t 30`). The bitstream must present USB0
in host mode (or OTG, which the driver overrides with `USBMODE.CM = host`).

## Measured

2026-09-26, `agrv2k_407`, canonical 200 MHz bitstream
(`example_board.bin`, md5 `6378549f3a8f82dd386353077f3d4a02`), a Lenovo
wireless mouse receiver (`17ef:6215`): enumerated, found interrupt IN `0x81`
(mps 8, `bInterval` 1), and received **461 reports in 24 s** with real
relative movement in them — no stalls, no transfer errors, no re-enumeration.

## Files

- `src/main.c` — class, endpoint discovery, one-transfer polling loop
- `boards/agrv2k_407.overlay` — disables `&usb0`, enables `&uhc0` as `zephyr_uhc0`
- `prj.conf` — host stack + logging
- `sample.yaml` — west metadata (`integration_platforms: agrv2k_407`)
