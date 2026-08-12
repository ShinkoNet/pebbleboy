#!/usr/bin/env python3
"""Pad a local ROM to 8 MiB and switch its test header to MBC5.

This is a storage/bank-path test fixture, not a general ROM-hacking tool. MBC5
has no RTC, so an expanded Pokemon Crystal image made here is only expected to
exercise booting from an 8 MiB cartridge. The original MBC3 image remains the
RTC compatibility test.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from make_sram_probe_rom import write_global_checksum, write_header_checksum


ROM_SIZE = 8 * 1024 * 1024


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    source = args.input.read_bytes()
    if len(source) < 0x150 or len(source) > ROM_SIZE or len(source) % 0x4000:
        raise SystemExit("input must be a bank-aligned Game Boy ROM up to 8 MiB")

    rom = bytearray(source)
    rom.extend(b"\xFF" * (ROM_SIZE - len(rom)))
    rom[0x0147] = 0x1B  # MBC5 + RAM + battery; intentionally no RTC.
    rom[0x0148] = 0x08  # 512 banks / 8 MiB.
    write_header_checksum(rom)
    write_global_checksum(rom)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(rom)
    print(f"wrote 8 MiB MBC5 test image: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
