#!/usr/bin/env python3
"""Inject Pebble button clicks through the running QEMU monitor."""

from __future__ import annotations

import argparse
import json
import socket
import time
from pathlib import Path


QEMU_KEYS = {
    "back": "left",
    "up": "up",
    "select": "right",
    "down": "down",
}


def running_qemu(platform: str) -> tuple[str, int]:
    registry = json.loads(Path("/tmp/pb-emulator.json").read_text())
    versions = registry.get(platform, {})
    for version in sorted(versions, reverse=True):
        qemu = versions[version].get("qemu", {})
        if qemu.get("monitor"):
            return "127.0.0.1", int(qemu["monitor"])
    raise RuntimeError(f"no running {platform} QEMU monitor found")


def click(host: str, port: int, button: str) -> None:
    with socket.create_connection((host, port), timeout=2) as sock:
        sock.settimeout(0.2)
        time.sleep(0.05)
        try:
            sock.recv(4096)
        except (TimeoutError, OSError):
            pass
        sock.sendall(f"sendkey {QEMU_KEYS[button]} 100\n".encode("ascii"))
        time.sleep(0.15)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("button", nargs="+", choices=tuple(QEMU_KEYS))
    parser.add_argument("--platform", default="emery")
    args = parser.parse_args()
    host, port = running_qemu(args.platform)
    for button in args.button:
        click(host, port, button)
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
