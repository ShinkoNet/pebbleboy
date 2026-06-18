#!/usr/bin/env python3
"""Generate a tiny MBC1+RAM test ROM that writes a known byte to SRAM."""

from __future__ import annotations

import argparse
from pathlib import Path


NINTENDO_LOGO = bytes(
    [
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
        0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
        0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    ]
)


def write_header_checksum(rom: bytearray) -> None:
    value = 0
    for index in range(0x0134, 0x014D):
        value = (value - rom[index] - 1) & 0xFF
    rom[0x014D] = value


def write_global_checksum(rom: bytearray) -> None:
    total = 0
    for index, value in enumerate(rom):
        if index not in (0x014E, 0x014F):
            total = (total + value) & 0xFFFF
    rom[0x014E] = (total >> 8) & 0xFF
    rom[0x014F] = total & 0xFF


def build_rom(write_value: int) -> bytes:
    rom = bytearray([0x00] * 0x8000)

    rom[0x0100:0x0104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x0104:0x0134] = NINTENDO_LOGO
    title = b"SRAM PROBE"
    rom[0x0134:0x0134 + len(title)] = title
    rom[0x0143] = 0x00  # DMG only.
    rom[0x0147] = 0x03  # MBC1 + RAM + battery.
    rom[0x0148] = 0x00  # 32 KiB ROM.
    rom[0x0149] = 0x02  # 8 KiB RAM.

    program = bytes(
        [
            0x3E, 0x0A,              # ld a,$0a
            0xEA, 0x00, 0x00,        # ld ($0000),a ; enable cart RAM
            0xFA, 0x00, 0xA0,        # ld a,($a000) ; force SRAM load
            0x3E, write_value,       # ld a,write_value
            0xEA, 0x00, 0xA0,        # ld ($a000),a
            0x18, 0xFE,              # jr $
        ]
    )
    rom[0x0150:0x0150 + len(program)] = program

    write_header_checksum(rom)
    write_global_checksum(rom)
    return bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--value", type=lambda text: int(text, 0), default=0x42)
    args = parser.parse_args()

    if not 0 <= args.value <= 0xFF:
        raise SystemExit("--value must be a byte")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_rom(args.value))
    print(f"wrote SRAM probe ROM: {args.output} value=0x{args.value:02x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
