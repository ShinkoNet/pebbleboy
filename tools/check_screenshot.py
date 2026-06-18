#!/usr/bin/env python3
"""Sanity-check a Pebble/QEMU RGBA PNG screenshot."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
      raise ValueError("not a PNG")

    pos = 8
    width = height = color_type = bit_depth = interlace = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _compression, _filter, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
        elif kind == b"IDAT":
            idat.extend(payload)
        elif kind == b"IEND":
            break

    if width is None or height is None:
        raise ValueError("missing IHDR")
    if bit_depth != 8 or color_type != 6 or interlace != 0:
        raise ValueError("expected 8-bit non-interlaced RGBA PNG")

    raw = zlib.decompress(bytes(idat))
    bpp = 4
    stride = width * bpp
    out = bytearray(height * stride)
    src = 0
    prev = bytearray(stride)
    for y in range(height):
        filter_type = raw[src]
        src += 1
        row = bytearray(raw[src:src + stride])
        src += stride
        for x in range(stride):
            left = row[x - bpp] if x >= bpp else 0
            up = prev[x]
            up_left = prev[x - bpp] if x >= bpp else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                row[x] = (row[x] + paeth(left, up, up_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}")
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return width, height, bytes(out)


def main() -> int:
    allow_loading = False
    fullscreen = False
    aspect_fit = False
    args = sys.argv[1:]
    while args and args[0].startswith("--"):
        if args[0] == "--allow-loading":
            allow_loading = True
        elif args[0] == "--fullscreen":
            fullscreen = True
        elif args[0] == "--aspect-fit":
            aspect_fit = True
        else:
            print(f"unknown option: {args[0]}", file=sys.stderr)
            return 2
        args = args[1:]
    if len(args) != 1:
        print(
            f"usage: {sys.argv[0]} [--allow-loading] [--fullscreen|--aspect-fit] SCREENSHOT.png",
            file=sys.stderr,
        )
        return 2
    width, height, rgba = read_png(Path(args[0]))
    colors = set()
    nonblack = 0
    viewport_colors = set()
    viewport_nonblack = 0
    outside_nonblack = 0
    vx0 = (width - 160) // 2
    vy0 = (height - 144) // 2
    vx1 = vx0 + 160
    vy1 = vy0 + 144
    for i in range(0, len(rgba), 4):
        rgb = rgba[i:i + 3]
        colors.add(rgb)
        if rgb != b"\x00\x00\x00":
            nonblack += 1
        pixel = i // 4
        x = pixel % width
        y = pixel // width
        if vx0 <= x < vx1 and vy0 <= y < vy1:
            viewport_colors.add(rgb)
            if rgb != b"\x00\x00\x00":
                viewport_nonblack += 1
        elif rgb != b"\x00\x00\x00":
            outside_nonblack += 1

    print(
        f"screenshot={width}x{height} colors={len(colors)} nonblack={nonblack} "
        f"viewport_colors={len(viewport_colors)} "
        f"viewport_nonblack={viewport_nonblack} outside_nonblack={outside_nonblack}"
    )
    if width != 200 or height != 228:
        print("unexpected Emery screenshot dimensions", file=sys.stderr)
        return 1
    if fullscreen:
        active_fullscreen = len(colors) >= 2 and nonblack >= 25000 and outside_nonblack >= 5000
        if not active_fullscreen:
            print("screenshot does not show fullscreen Game Boy video", file=sys.stderr)
            return 1
        return 0
    if aspect_fit:
        active_aspect_fit = len(colors) >= 2 and nonblack >= 25000 and outside_nonblack >= 5000
        if not active_aspect_fit:
            print("screenshot does not show aspect-fit Game Boy video", file=sys.stderr)
            return 1
        top_bar_black = all(
            rgba[(y * width + x) * 4:(y * width + x) * 4 + 3] == b"\x00\x00\x00"
            for y in range(0, 20)
            for x in range(width)
        )
        bottom_bar_black = all(
            rgba[(y * width + x) * 4:(y * width + x) * 4 + 3] == b"\x00\x00\x00"
            for y in range(height - 20, height)
            for x in range(width)
        )
        if not top_bar_black or not bottom_bar_black:
            print("aspect-fit screenshot is missing vertical letterbox bars", file=sys.stderr)
            return 1
        return 0
    active_video = len(viewport_colors) >= 2 and viewport_nonblack >= 5000
    loading_app = allow_loading and viewport_nonblack >= 5000 and outside_nonblack <= 2000
    if not active_video and not loading_app:
        print("screenshot does not show active Game Boy video", file=sys.stderr)
        return 1
    if outside_nonblack > 1000 and not loading_app:
        print("screenshot still looks like the Pebble launcher", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
