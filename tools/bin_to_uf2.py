#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# bin_to_uf2.py -- wrap a firmware binary into the UF2 block stream the
# UF2-style MSC DFU sample (samples/usb_msc_dfu_boot) accepts.
#
# The host copies the resulting file onto the drive the board presents; the
# board parses each 512-byte block as it is written and streams the payload
# into the loader's A/B store. The last block ends the update, at which point
# the board closes the MSC interface and reboots.
#
#   tools/bin_to_uf2.py build/zephyr/zephyr.bin -o FW.UF2
#
# For the AGM family, targetAddr is the offset *within the store* (the board
# picks the store the boot record does not point at), not an absolute flash
# address -- there is no fixed absolute address until that choice is made on
# the device.

import argparse
import struct
import sys

BLOCK = 512
PAYLOAD = 256          # RP2040's uf2conv uses 256; keeps targetAddr aligned
DATA_OFF = 32
MAGIC0 = 0x0A324655
MAGIC1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLAG_FAMILY_ID = 0x00002000
AGM_FAMILY = 0x41474D55        # "AGMU" -- our own, not an allocated family


def block(target, payload, number, total, family):
    b = bytearray(BLOCK)
    struct.pack_into("<IIIIIIII", b, 0, MAGIC0, MAGIC1, FLAG_FAMILY_ID,
                     target, len(payload), number, total, family)
    b[DATA_OFF:DATA_OFF + len(payload)] = payload
    struct.pack_into("<I", b, BLOCK - 4, MAGIC_END)
    return bytes(b)


def main():
    p = argparse.ArgumentParser(
        description="Wrap a firmware binary into the UF2 block stream the "
                    "UF2-style MSC DFU sample accepts.")
    p.add_argument("image", help="raw firmware binary (zephyr.bin)")
    p.add_argument("-o", "--out", required=True, help=".uf2 to write")
    p.add_argument("--family", default=f"0x{AGM_FAMILY:08x}",
                   help="UF2 family id (default: the AGM one)")
    args = p.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()
    if not image:
        p.error(f"{args.image}: empty")

    chunks = [image[i:i + PAYLOAD] for i in range(0, len(image), PAYLOAD)]
    total = len(chunks)
    family = int(args.family, 0)

    with open(args.out, "wb") as f:
        for i, chunk in enumerate(chunks):
            f.write(block(i * PAYLOAD, chunk, i, total, family))

    print(f"{args.out}: {len(image)} B -> {total} UF2 blocks "
          f"({total * BLOCK} B), family 0x{family:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
