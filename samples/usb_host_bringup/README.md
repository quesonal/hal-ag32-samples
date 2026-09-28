# AgRV2K USB Host Bring-up Self-check

The one USB host sample that **needs no device** on the connector. It drives
the UHC API directly (no host stack, no class, no `usbh` threads) and checks
that the driver really put the controller into host mode, then stops it again.

It is the on-board (HIL) counterpart of the `native_sim` suite in
[`tests/drivers/usb/uhc/uhc_agm`](../../tests/drivers/usb/uhc/uhc_agm): same
kind of register assertions, but against the real controller, which is the only
way to check the parts a RAM window cannot model.

## What it checks

| Phase | Checks |
|---|---|
| `init` | `uhc_init()` rejects a missing callback; `USBMODE.CM` = host; `ASYNCLISTADDR` programmed and 32-byte aligned; `PERIODICLISTBASE` programmed and 4 KiB aligned; interrupts still masked; not running yet |
| `enable` | run bit + periodic + async schedule enables; 8-entry frame list (ChipIdea encoding); `USBINTR` = the host interrupt set; `USBSTS.HCH` clear |
| port | `PORTSC.CCS` agrees with the connect event that was (or was not) delivered |
| `disable` | run bit cleared, interrupts masked, `USBSTS.HCH` set |

The port check is deliberately *consistency*, not "no device": with nothing
plugged nothing must arrive, with a device plugged something must. That makes
the scenario safe to schedule on a bench where somebody may have left a mouse
in the connector.

## Output

```
usb_host_bringup: AgRV2K USB0 host bring-up self-check
  ok   the UHC device came up
  ok   USBMODE.CM = host
  ...
  port PORTSC 0xe0001801: device attached, 1 event(s)
  ok   port state agrees with the connect event
  ...
usb_host_bringup: 22 checks - PASS
```

Any failed check prints `FAIL <what>` and the last line becomes `N of M checks
- FAIL`, which is what the twister `harness: console` regex is *not* looking
for.

## Build and run

```
source <your-venv>/bin/activate
cd <your-workspace>
west build -b agrv2k_407 -d /tmp/b_bringup --pristine=auto \
    modules/hal_ag32/samples/usb_host_bringup
west flash -d /tmp/b_bringup
bash tools/test_uart_capture.sh -t 8 /tmp/b_bringup/zephyr/zephyr.bin
```

Or as part of the device-testing gate:

```
tools/test_hil.sh sample.usb_host_bringup.hil.agm_agrv2k_407
```

## Measured

2026-09-26, `agrv2k_407`, canonical 200 MHz bitstream
(`example_board.bin`, md5 `6378549f3a8f82dd386353077f3d4a02`): **22 checks -
PASS**, both standalone and through `tools/test_hil.sh` (twister
`--device-testing`), and both port states were exercised:

* with nothing in USB0 — `port PORTSC 0xec001000: device absent, 0 event(s)`
* with a wireless mouse receiver plugged in — `port PORTSC 0xe0001801: device
  attached, 1 event(s)`

Together with `sample.usb_host_enum.hil.agm_agrv2k_407` the device-testing run
was 2/2 PASS on an empty port.

## Files

- `src/main.c` — the checks
- `boards/agrv2k_407.overlay` — disables `&usb0`, enables `&uhc0` as `zephyr_uhc0`
- `prj.conf` — `UHC_DRIVER` only (no host stack)
- `sample.yaml` — build-only + `hil` scenarios
