#!/usr/bin/env python3
"""Check blank enrollment regions in an offline 1 MiB T096 flash dump."""
import argparse
from pathlib import Path

FLASH_BYTES = 0x100000
REGIONS = (("legacy PKI history", 0xa6000, 0xd0000), ("settings NVS", 0xd0000, 0xd4000))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="raw flash readback starting at chip address zero")
    parser.add_argument("--device-reset", action="store_true",
                        help="also verify all 128 KiB of the original LittleFS range for the reset-capable first enrollment")
    parser.add_argument("--full-companion", action="store_true",
                        help="verify former PKI, settings, LittleFS and reset page before installing either extended first-install image")
    args = parser.parse_args()
    if args.dump.stat().st_size != FLASH_BYTES:
        parser.exit(2, "An exact 1 MiB raw flash readback is required.\n")
    flash = args.dump.read_bytes()
    if len(flash) != FLASH_BYTES:
        parser.exit(2, "The readback changed while it was being read.\n")
    if all(b == 0xff for b in flash[0xf4000:]):
        parser.exit(2, "UF2 boot reservation is blank. Verify the readback and board.\n")
    regions = REGIONS + ((("original LittleFS volume", 0xd4000, 0xf4000),) if args.device_reset or args.full_companion else ())
    occupied = [name for name, start, end in regions if flash[start:end].count(0xff) != end-start]
    if occupied:
        parser.exit(1, "First enrollment refused. Occupied regions: " + ", ".join(occupied) + "\n")
    print("First-enrollment regions are erased in this readback." if args.device_reset or args.full_companion
          else "Former PKI history and settings NVS are erased in this readback.")


if __name__ == "__main__":
    main()
