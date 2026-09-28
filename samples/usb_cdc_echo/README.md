# AgRV2K USB CDC-ACM Echo

USB0 CDC-ACM echo sample for the `agrv2k_407` board (`the development notes (not published here)`, the `udc_agm` device-controller driver).

## What it does

When USB0 is plugged into a host, the board enumerates as a USB CDC-ACM
device (one Communications interface + one Data interface, full-speed).
The host sees a new `/dev/ttyACMx` node. Any bytes the host writes to
that node are echoed back to the host through the same node — pure
software loopback inside the device, no physical UART loopback required.

## Why a separate sample (not the upstream `cdc_acm`)

- We needed a place that lives in `zephyr-hal-ag32/samples/` so changes
  don't pollute upstream Zephyr.
- The driver side (`drivers/usb/udc/udc_agm.c`) and the bitstream-side
  (CMSIS-DAP UART bridge on UART0) are stable enough to test from a
  hal-owned sample.

## Build

```
source <your-venv>/bin/activate
cd ~/zephyrproject/zephyr
west build -b agrv2k_407 -p auto \
    -d ~/zephyr-hal-ag32/samples/usb_cdc_echo/build \
    ~/zephyr-hal-ag32/samples/usb_cdc_echo/
```

## Flash

```
source <your-venv>/bin/activate
~/zephyr-hal-ag32/tools/flash_fw.sh \
    ~/zephyr-hal-ag32/samples/usb_cdc_echo/build/zephyr/zephyr.bin
```

The flash script does **not** erase the FPGA bitstream region
(`flash mass_erase 0` wipes the bitstream; the script targets only the
firmware slot, so the bitstream survives across firmware-only rebuilds).

## Test

1. Connect USB0 (the agrv2k_407 board has a dedicated USB0 connector;
   the CMSIS-DAP probe and the bitstream console share **UART0**
   hardware, so the host's `/dev/ttyACM0` from the AgRV probe is the
   *debug* bridge, **not** this CDC-ACM device) to the host.
2. `lsusb` should now show a new "AgRV2K USB CDC-ACM" device and a
   new `/dev/ttyACMx` node appears.
3. `minicom -D /dev/ttyACMx -b 115200` (or any terminal emulator).
4. Type — every character should echo back.

## Files

- `src/main.c`            fork of upstream `cdc_acm` sample
- `boards/agrv2k_407.overlay`  enables the `cdc_acm_uart0` DLM on
                               `&zephyr_udc0`
- `prj.conf`              CDC-ACM stack + logging at ERR level
- `Kconfig`               pulls in upstream `SAMPLE_USBD_*` options
- `CMakeLists.txt`        includes upstream `samples/subsys/usb/common/`
                           shared `sample_usbd_init.c` (no fork)
- `sample.yaml`           west metadata (`integration_platforms:
                           agrv2k_407`)
