# AgRV2K USB HID Mouse Demo

USB0 HID mouse demo for the `agrv2k_407` board (`the development notes (not published here)`,
the `udc_agm` device-controller driver + the device-next `usbd_hid` class).

## What it does

When USB0 is plugged into a host, the board enumerates as a full-speed USB
HID mouse (`2fe3:0008`, "AgRV2K USB HID Mouse"). No physical mouse or GPIO
keys are needed: a background loop submits relative-motion reports that
trace a circle (64 steps × 10 ms, radius ~10 px), so the host cursor moves
by itself. LED0 toggles roughly every 16 reports (~160 ms) as a visual
heartbeat.

## Why a separate sample (not the upstream `hid-mouse`)

Upstream `samples/subsys/usb/hid-mouse` drives reports from the `input`
subsystem (`gpio-keys` + `INPUT_KEY_*` events); the agrv2k_407 board has no
such keys wired in Zephyr, so it builds but does nothing. This sample
replaces the input path with a built-in demo pattern, and carries the
hal_ag32 USB fixes from `usb_cdc_echo` (`CONFIG_HWINFO=n`, bus-powered 50 mA,
etc. — see `the development notes (not published here)`).

## Build

```
source <your-venv>/bin/activate
cd ~/zephyrproject/zephyr
west build -b agrv2k_407 -p auto \
    -d ~/zephyr-hal-ag32/samples/usb_hid_mouse/build \
    ~/zephyr-hal-ag32/samples/usb_hid_mouse/
```

## Flash

```
source <your-venv>/bin/activate
~/zephyr-hal-ag32/tools/flash_fw.sh \
    ~/zephyr-hal-ag32/samples/usb_hid_mouse/build/zephyr/zephyr.bin
```

Erase the firmware slot before writing (`flash erase_address 0x80000000
0xe6000` via openocd) or verify reports a checksum mismatch. Never erase
past `0x800E7000` — that is the FPGA bitstream/FCB area.

## Test on the host

1. Plug USB0 into the host.
2. `dmesg | tail` should show a new input device:

   ```
   input: AgRV2K USB HID Mouse as /devices/.../input/inputN
   hid-generic ...: input: USB HID v1.10 Mouse [AgRV2K USB HID Mouse]
   ```

3. Watch the cursor: it traces a circle on its own.
4. (Optional) `sudo evtest /dev/input/eventN` shows `REL_X` / `REL_Y`
   events with no key presses.

The demo sends movement only (buttons byte stays 0). To exercise a button,
set bit 0 of `report[0]` (left) or bit 1 (right) for one report.

## Files

- `src/main.c`                 report-descriptor registration + circle loop
- `boards/agrv2k_407.overlay`  instantiates the `zephyr,hid-device` class
- `prj.conf`                   USB device-next stack + sample descriptors
- `Kconfig` / `CMakeLists.txt` sample options + upstream usbd helper
- `sample.yaml`                west/twister metadata
