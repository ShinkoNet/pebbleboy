#!/usr/bin/env python3
"""Generate tiny MBC ROM/RAM bank-switch probes that report through SRAM."""

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
ROM_SIZE_CODES = {
    2: 0x00,
    4: 0x01,
    8: 0x02,
    16: 0x03,
    32: 0x04,
    64: 0x05,
    128: 0x06,
    256: 0x07,
    512: 0x08,
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


def emit_ld_a_imm(program: bytearray, value: int) -> None:
    program.extend([0x3E, value & 0xFF])


def emit_ld_a_abs(program: bytearray, addr: int) -> None:
    program.extend([0xFA, addr & 0xFF, (addr >> 8) & 0xFF])


def emit_ld_abs_a(program: bytearray, addr: int) -> None:
    program.extend([0xEA, addr & 0xFF, (addr >> 8) & 0xFF])


def emit_write_a_to_addr(program: bytearray, value: int, addr: int) -> None:
    emit_ld_a_imm(program, value)
    emit_ld_abs_a(program, addr)


def select_bank(program: bytearray, mbc: int, bank: int) -> None:
    if mbc == 5:
        emit_write_a_to_addr(program, bank & 0xFF, 0x2000)
        emit_write_a_to_addr(program, (bank >> 8) & 0x01, 0x3000)
    else:
        emit_write_a_to_addr(program, bank, 0x2000)


def emit_select_ram_bank(program: bytearray, mbc: int, bank: int) -> None:
    if mbc == 1:
        emit_write_a_to_addr(program, 0x01, 0x6000)
    emit_write_a_to_addr(program, bank, 0x4000)


def emit_compare_a_to_value(program: bytearray, value: int, jp_placeholders: list[int]) -> None:
    program.extend([0xFE, value])  # cp value
    jp_placeholders.append(len(program) + 1)
    program.extend([0xC2, 0x00, 0x00])  # jp nz,fail


def bank_marker(bank: int) -> int:
    return BANK_MARKERS.get(bank, (bank * 73 + 0x5A) & 0xFF)


def build_rom_bank_program(mbc: int, bank_count: int) -> bytes:
    program = bytearray()
    program.extend([0x31, 0xF0, 0xDF])  # ld sp,$dff0
    emit_write_a_to_addr(program, 0x0A, 0x0000)  # enable cart RAM

    jp_placeholders: list[int] = []
    for bank in range(1, bank_count):
        select_bank(program, mbc, bank)
        emit_ld_a_abs(program, 0x4000)
        emit_compare_a_to_value(program, bank_marker(bank), jp_placeholders)

    emit_write_a_to_addr(program, SUCCESS_VALUE, 0xA000)
    emit_finish(program, jp_placeholders)
    return bytes(program)


def build_ram_bank_program(mbc: int) -> bytes:
    program = bytearray()
    program.extend([0x31, 0xF0, 0xDF])  # ld sp,$dff0
    emit_write_a_to_addr(program, 0x0A, 0x0000)  # enable cart RAM

    for bank in range(4):
        emit_select_ram_bank(program, mbc, bank)
        emit_write_a_to_addr(program, 0x60 + bank, 0xA000)

    jp_placeholders: list[int] = []
    for bank in range(4):
        emit_select_ram_bank(program, mbc, bank)
        emit_ld_a_abs(program, 0xA000)
        emit_compare_a_to_value(program, 0x60 + bank, jp_placeholders)

    emit_select_ram_bank(program, mbc, 0)
    emit_write_a_to_addr(program, SUCCESS_VALUE, 0xA000)
    emit_finish(program, jp_placeholders)
    return bytes(program)


def build_rtc_program() -> bytes:
    program = bytearray()
    program.extend([0x31, 0xF0, 0xDF])  # ld sp,$dff0
    emit_write_a_to_addr(program, 0x0A, 0x0000)  # enable RAM and RTC
    emit_write_a_to_addr(program, 0x08, 0x4000)  # select RTC seconds
    emit_write_a_to_addr(program, 10, 0xA000)

    emit_write_a_to_addr(program, 0, 0x6000)
    emit_write_a_to_addr(program, 1, 0x6000)  # latch seconds=10
    emit_write_a_to_addr(program, 20, 0xA000)  # live clock changes only

    jp_placeholders: list[int] = []
    emit_ld_a_abs(program, 0xA000)
    emit_compare_a_to_value(program, 10, jp_placeholders)

    emit_write_a_to_addr(program, 0, 0x6000)
    emit_write_a_to_addr(program, 1, 0x6000)  # re-latch seconds=20
    emit_ld_a_abs(program, 0xA000)
    emit_compare_a_to_value(program, 20, jp_placeholders)

    emit_write_a_to_addr(program, 0, 0x0000)  # disabled RTC reads as FF
    emit_ld_a_abs(program, 0xA000)
    emit_compare_a_to_value(program, 0xFF, jp_placeholders)

    emit_write_a_to_addr(program, 0x0A, 0x0000)
    emit_write_a_to_addr(program, 0, 0x4000)  # select SRAM bank zero
    emit_write_a_to_addr(program, SUCCESS_VALUE, 0xA000)
    emit_finish(program, jp_placeholders)
    return bytes(program)


def emit_finish(program: bytearray, jp_placeholders: list[int]) -> None:
    program.extend([0x18, 0xFE])  # jr $
    fail_addr = START_ADDR + len(program)
    emit_write_a_to_addr(program, FAIL_VALUE, 0xA000)
    program.extend([0x18, 0xFE])  # jr $

    for placeholder in jp_placeholders:
        program[placeholder] = fail_addr & 0xFF
        program[placeholder + 1] = (fail_addr >> 8) & 0xFF


def build_rom(kind: str, probe: str, bank_count: int = 4) -> bytes:
    config = MBC_CONFIG[kind]
    if bank_count not in ROM_SIZE_CODES:
        raise ValueError(f"unsupported ROM bank count: {bank_count}")
    rom = bytearray([0x00] * (bank_count * BANK_SIZE))

    rom[0x0100:0x0104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x0104:0x0134] = NINTENDO_LOGO
    if probe == "rtc":
        if kind != "mbc3":
            raise ValueError("RTC probe requires MBC3")
        title = b"MBC3 RTC"
    elif probe == "ram":
        title = config["title"].replace(b"PROBE", b"RAM")
    elif bank_count > 4:
        title = f"MBC{config['mbc']} ALLBANK".encode("ascii")
    else:
        title = config["title"]
    rom[0x0134:0x0134 + len(title)] = title
    rom[0x0143] = 0x00  # DMG only.
    rom[0x0147] = 0x10 if probe == "rtc" else config["cart_type"]
    rom[0x0148] = ROM_SIZE_CODES[bank_count]
    rom[0x0149] = 0x02 if probe == "rom" else 0x03  # 8 KiB or 32 KiB RAM.

    if probe == "rom":
        program = build_rom_bank_program(config["mbc"], bank_count)
    elif probe == "rtc":
        program = build_rtc_program()
    else:
        program = build_ram_bank_program(config["mbc"])
    rom[START_ADDR:START_ADDR + len(program)] = program
    for bank in range(1, bank_count):
        rom[bank * BANK_SIZE] = bank_marker(bank)

    write_header_checksum(rom)
    write_global_checksum(rom)
    return bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--mbc", choices=tuple(MBC_CONFIG), required=True)
    parser.add_argument("--probe", choices=("rom", "ram", "rtc"), default="rom")
    parser.add_argument("--banks", type=int, choices=tuple(ROM_SIZE_CODES), default=4)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_rom(args.mbc, args.probe, args.banks))
    print(
        f"wrote {args.mbc.upper()} {args.probe} probe ROM "
        f"({args.banks} banks): {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
