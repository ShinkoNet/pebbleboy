#!/usr/bin/env python3
"""Capture the active Pebble QEMU framebuffer through the monitor port."""

import json
import socket
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path


def monitor_port() -> int:
    with open("/tmp/pb-emulator.json", "r", encoding="utf-8") as fh:
        data = json.load(fh)
    emery = data["emery"]
    sdk = next(iter(emery.values()))
    return int(sdk["qemu"]["monitor"])


def send_monitor_command(port: int, command: str) -> None:
    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as sock:
        sock.settimeout(0.5)
        try:
            sock.recv(4096)
        except Exception:
            pass
        sock.sendall((command.rstrip() + "\n").encode("utf-8"))
        try:
            sock.recv(4096)
        except Exception:
            pass


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            return token()
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        return data[start:pos]

    magic = token()
    if magic != b"P6":
        raise ValueError("expected binary PPM screendump")
    width = int(token())
    height = int(token())
    maxval = int(token())
    if maxval != 255:
        raise ValueError("expected 8-bit PPM screendump")
    while pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1
    rgb = data[pos:]
    if len(rgb) != width * height * 3:
        raise ValueError("unexpected PPM payload length")
    return width, height, rgb


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)
        row = rgb[y * stride:(y + 1) * stride]
        for x in range(0, len(row), 3):
            rows.extend(row[x:x + 3])
            rows.append(255)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) +
            kind +
            payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
    png.extend(chunk(b"IDAT", zlib.compress(bytes(rows))))
    png.extend(chunk(b"IEND", b""))
    path.write_bytes(bytes(png))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUT.png", file=sys.stderr)
        return 2

    out_path = Path(sys.argv[1])
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pebble-qemu-shot-") as tmp:
        ppm_path = Path(tmp) / "screen.ppm"
        send_monitor_command(monitor_port(), f"screendump {ppm_path}")
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if ppm_path.exists() and ppm_path.stat().st_size > 0:
                width, height, rgb = read_ppm(ppm_path)
                write_png(out_path, width, height, rgb)
                print(f"Saved QEMU screendump to {out_path}")
                return 0
            time.sleep(0.02)

    print("timed out waiting for QEMU screendump", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
