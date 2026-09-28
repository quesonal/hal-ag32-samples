# hal_ag32_samples tools

Sample-specific host scripts. The module-wide tools (sign_image,
agm_upload, check_pin_routing, build_bitstream, flash_*) live in the
companion `hal_ag32` repo.

| Tool | Needs |
| --- | --- |
| `bin_to_uf2.py` | any python3 (stdlib only) — no venv, no SDK, no board |
| `gen_uf2_volume.py` | any python3 (stdlib only) — no venv, no SDK, no board |

## bin_to_uf2.py

Wraps a raw `zephyr.bin` into the UF2 block stream the
`samples/usb_msc_dfu_boot` sample accepts (512 B blocks, 256 B payloads).
For the sample's own family id `0x41474d55`, `targetAddr` is the offset
*within* the loader's store, not an absolute flash address — the store
is chosen on the device.

```sh
west build -b agrv2k_407 modules/hal_ag32_samples/samples/usb_msc_dfu_boot
tools/bin_to_uf2.py build/zephyr/zephyr.bin -o FW.UF2
```

## gen_uf2_volume.py

Generates the *fake* FAT16 volume header (`samples/usb_msc_dfu_boot/src/uf2_volume.h`)
that the same sample publishes: a 64 MiB claim backed by nothing but a
boot sector and the first FAT sector, because a UF2 bootloader never
stores the file the host writes. Pure stdlib python3; regenerate the
checked-in header when the geometry changes.

```sh
modules/hal_ag32_samples/tools/gen_uf2_volume.py \
    --out modules/hal_ag32_samples/samples/usb_msc_dfu_boot/src/uf2_volume.h
```

Both tools exit non-zero on any malformed input; messages go to stderr
and the final summary to stdout.
