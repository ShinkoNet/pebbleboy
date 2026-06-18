#!/usr/bin/env python3
"""Seed pypkjs localStorage with the phone-backed ROM URL for QEMU tests."""

from __future__ import annotations

import argparse
import base64
import dbm.dumb
import hashlib
import json
from pathlib import Path


APP_UUID = "53852c6a-202c-4a64-ae41-a7ed891bd8cf"
DEFAULT_PERSIST_DIR = Path.home() / ".pebble-sdk" / "4.15.0-dirty-local" / "emery"
DEFAULT_CHUNK_SIZE = 8192


def storage_base(persist_dir: Path) -> Path:
    return persist_dir / "localstorage" / APP_UUID


def rom_title(data: bytes) -> str:
    out = []
    for value in data[0x134:0x144]:
        if value < 32 or value > 95:
            break
        out.append(chr(value))
    return "".join(out) or "DMG ROM"


def seed_cached_rom(db, rom_url: str, rom_file: Path, chunk_size: int) -> None:
    data = rom_file.read_bytes()
    if len(data) < 0x150:
        raise ValueError(f"{rom_file} is too small to be a Game Boy ROM")

    chunks = (len(data) + chunk_size - 1) // chunk_size
    for index in range(chunks):
        start = index * chunk_size
        end = min(start + chunk_size, len(data))
        db[f"romChunk{index}"] = base64.b64encode(data[start:end]).decode("ascii")

    meta = {
        "size": len(data),
        "sha1": hashlib.sha1(data).hexdigest(),
        "title": rom_title(data),
        "cartType": data[0x147],
        "url": rom_url,
        "chunks": chunks,
    }
    db["romMeta"] = json.dumps(meta, separators=(",", ":"))
    print(f"seeded cached ROM: {rom_file} title={meta['title']} chunks={chunks}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rom_url", nargs="?")
    parser.add_argument("--persist-dir", type=Path, default=DEFAULT_PERSIST_DIR)
    parser.add_argument("--rom-file", type=Path)
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)
    parser.add_argument("--audio-enabled", dest="audio_enabled", action="store_true", default=False)
    parser.add_argument("--audio-disabled", dest="audio_enabled", action="store_false")
    parser.add_argument("--clear", action="store_true")
    args = parser.parse_args()

    base = storage_base(args.persist_dir)
    base.parent.mkdir(parents=True, exist_ok=True)
    db = dbm.dumb.open(str(base), "c")
    try:
        for key in list(db.keys()):
            name = key.decode("utf-8", errors="replace") if isinstance(key, bytes) else str(key)
            if name in ("romUrl", "romMeta", "audioEnabled") or name.startswith("romChunk"):
                del db[key]
        if args.clear:
            print("cleared phone ROM URL and cache")
            return 0
        if not args.rom_url:
            raise SystemExit("rom_url is required unless --clear is used")
        db["romUrl"] = args.rom_url
        db["audioEnabled"] = "1" if args.audio_enabled else "0"
        if args.rom_file:
            seed_cached_rom(db, args.rom_url, args.rom_file, args.chunk_size)
    finally:
        db.close()

    print(f"seeded phone ROM URL: {args.rom_url}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
