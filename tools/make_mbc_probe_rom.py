#!/usr/bin/env python3
"""Generate tiny MBC bank-switch ROMs that report success through SRAM."""

from __future__ import annotations

import argparse
from pathlib import Path

from make_sram_probe_rom import NINTENDO_LOGO, write_global_checksum, write_header_checksum


BANK_SIZE = 0x4000
START_ADDR = 0x0150
SUCCESS_VALUE = 0x42
FAIL_VALUE = 0xE0
BANK_MARKERS = {
    1: 0xA1,
    2: 0xB2,
    3: 0xC3,
}
MBC_CONFIG = {
    "mbc1": {
        "title": b"MBC1 PROBE",
        "cart_type": 0x03,  # MBC1 + RAM + battery.
        "mbc": 1,
    },
    "mbc3": {
        "title": b"MBC3 PROBE",
        "cart_type": 0x13,  # MBC3 + RAM + battery.
        "mbc": 3,
    },
    "mbc5": {
        "title": b"MBC5 PROBE",
        "cart_type": 0x1B,  # MBC5 + RAM + battery.
        "mbc": 5,
    },
}


def emit_ld_a_abs(program: bytearray, addr: int) -> None:
    program.extend([0xFA, addr & 0xFF, (addr >> 8) & 0xFF])


def emit_ld_abs_a(program: bytearray, addr: int) -> None:
    program.extend([0xEA, addr & 0xFF, (addr >> 8) & 0xFF])


def emit_write_a_to_addr(program: bytearray, value: int, addr: int) -> None:
    program.extend([0x3E, value])
    emit_ld_abs_a(program, addr)


def select_bank(program: bytearray, mbc: int, bank: int) -> None:
    if mbc == 5:
        emit_write_a_to_addr(program, bank & 0xFF, 0x2000)
        emit_write_a_to_addr(program, (bank >> 8) & 0x01, 0x3000)
    else:
        emit_write_a_to_addr(program, bank, 0x2000)


def build_program(mbc: int) -> bytes:
    program = bytearray()
    program.extend([0x31, 0xF0, 0xDF])  # ld sp,$dff0
    emit_write_a_to_addr(program, 0x0A, 0x0000)  # enable cart RAM

    jp_placeholders: list[int] = []
    for bank, marker in BANK_MARKERS.items():
        select_bank(program, mbc, bank)
        emit_ld_a_abs(program, 0x4000)
        program.extend([0xFE, marker])  # cp marker
        jp_placeholders.append(len(program) + 1)
        program.extend([0xC2, 0x00, 0x00])  # jp nz,fail

    emit_write_a_to_addr(program, SUCCESS_VALUE, 0xA000)
    program.extend([0x18, 0xFE])  # jr $

    fail_addr = START_ADDR + len(program)
    emit_write_a_to_addr(program, FAIL_VALUE, 0xA000)
    program.extend([0x18, 0xFE])  # jr $

    for placeholder in jp_placeholders:
        program[placeholder] = fail_addr & 0xFF
        program[placeholder + 1] = (fail_addr >> 8) & 0xFF

    return bytes(program)


def build_rom(kind: str) -> bytes:
    config = MBC_CONFIG[kind]
    rom = bytearray([0x00] * (4 * BANK_SIZE))

    rom[0x0100:0x0104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x0104:0x0134] = NINTENDO_LOGO
    rom[0x0134:0x0134 + len(config["title"])] = config["title"]
    rom[0x0143] = 0x00  # DMG only.
    rom[0x0147] = config["cart_type"]
    rom[0x0148] = 0x01  # 64 KiB ROM / 4 banks.
    rom[0x0149] = 0x02  # 8 KiB RAM.

    program = build_program(config["mbc"])
    rom[START_ADDR:START_ADDR + len(program)] = program
    for bank, marker in BANK_MARKERS.items():
        rom[bank * BANK_SIZE] = marker

    write_header_checksum(rom)
    write_global_checksum(rom)
    return bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--mbc", choices=tuple(MBC_CONFIG), required=True)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_rom(args.mbc))
    print(f"wrote {args.mbc.upper()} probe ROM: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
