#!/usr/bin/env python3
"""Inspect pypkjs localStorage for a Pebbleboy SRAM byte keyed by ROM SHA-1."""

from __future__ import annotations

import argparse
import base64
import dbm.dumb
import hashlib
from pathlib import Path


APP_UUID = "53852c6a-202c-4a64-ae41-a7ed891bd8cf"
DEFAULT_PERSIST_DIR = Path.home() / ".pebble-sdk" / "4.15.0-dirty-local" / "emery"
MSG_CHUNK = 512


def storage_base(persist_dir: Path) -> Path:
    return persist_dir / "localstorage" / APP_UUID


def sram_prefix(rom_file: Path) -> str:
    digest = hashlib.sha1(rom_file.read_bytes()).hexdigest()
    return f"sram:{digest}:"


def db_value(db, key: str) -> str | None:
    try:
        value = db[key]
    except KeyError:
        return None
    if isinstance(value, bytes):
        return value.decode("ascii")
    return str(value)


def clear_sram(db, prefix: str) -> int:
    removed = 0
    for key in list(db.keys()):
        name = key.decode("utf-8", errors="replace") if isinstance(key, bytes) else str(key)
        if name.startswith(prefix):
            del db[key]
            removed += 1
    return removed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rom_file", type=Path)
    parser.add_argument("--persist-dir", type=Path, default=DEFAULT_PERSIST_DIR)
    parser.add_argument("--bank", type=int, default=0)
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--value", type=lambda text: int(text, 0), default=0x42)
    parser.add_argument("--clear", action="store_true")
    args = parser.parse_args()

    prefix = sram_prefix(args.rom_file)
    base = storage_base(args.persist_dir)
    base.parent.mkdir(parents=True, exist_ok=True)
    db = dbm.dumb.open(str(base), "c")
    try:
        if args.clear:
            removed = clear_sram(db, prefix)
            print(f"cleared SRAM keys: prefix={prefix} removed={removed}")
            return 0

        if not 0 <= args.value <= 0xFF:
            raise SystemExit("--value must be a byte")
        if args.offset < 0:
            raise SystemExit("--offset must be non-negative")

        chunk_index = args.offset // MSG_CHUNK
        chunk_offset = args.offset % MSG_CHUNK
        key = f"{prefix}bank:{args.bank}:chunk:{chunk_index}"
        stored = db_value(db, key)
        if stored is None:
            raise SystemExit(f"missing SRAM chunk: {key}")
        data = base64.b64decode(stored)
        if chunk_offset >= len(data):
            raise SystemExit(f"SRAM chunk too short: {key} len={len(data)}")
        actual = data[chunk_offset]
        print(f"sram key={key} offset={args.offset} value=0x{actual:02x}")
        if actual != args.value:
            raise SystemExit(f"expected 0x{args.value:02x}, saw 0x{actual:02x}")
        return 0
    finally:
        db.close()


if __name__ == "__main__":
    raise SystemExit(main())
