#!/usr/bin/env python3
"""Check Pokemon Red title-screen sprite rendering from a BMP frame or QEMU PNG."""

from __future__ import annotations

import struct
import sys
from collections import Counter
from pathlib import Path


def normalize_shades(values: list[list[int]]) -> list[list[int]]:
    shades = sorted({value for row in values for value in row})
    if len(shades) != 4:
        raise ValueError(f"expected four grayscale shades, saw {shades}")
    mapping = {shade: index * 85 for index, shade in enumerate(shades)}
    return [[mapping[value] for value in row] for row in values]


def read_bmp(path: Path) -> tuple[int, int, list[list[int]]]:
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise ValueError("expected BMP")

    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError("unsupported BMP DIB header")

    width, height = struct.unpack_from("<ii", data, 18)
    planes, bpp = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if planes != 1 or bpp != 24 or compression != 0:
        raise ValueError("expected uncompressed 24-bit BMP")
    if width <= 0 or height <= 0:
        raise ValueError("expected bottom-up BMP with positive dimensions")

    stride = ((width * 3 + 3) // 4) * 4
    pixels: list[list[int]] = []
    for y in range(height):
        row = []
        src = offset + (height - 1 - y) * stride
        for x in range(width):
            b, g, r = data[src + x * 3:src + x * 3 + 3]
            if r != g or g != b:
                raise ValueError("expected grayscale RGB pixels")
            row.append(r)
        pixels.append(row)
    return width, height, pixels


def read_png_viewport(path: Path) -> tuple[int, int, list[list[int]]]:
    from check_screenshot import read_png

    width, height, rgba = read_png(path)
    if width != 200 or height != 228:
        raise ValueError(f"unexpected QEMU screenshot size {width}x{height}")

    vx0 = (width - 160) // 2
    vy0 = (height - 144) // 2
    pixels: list[list[int]] = []
    for y in range(vy0, vy0 + 144):
        row = []
        for x in range(vx0, vx0 + 160):
            i = (y * width + x) * 4
            r, g, b = rgba[i:i + 3]
            if r != g or g != b:
                raise ValueError("expected grayscale RGB pixels")
            row.append(r)
        pixels.append(row)
    return 160, 144, normalize_shades(pixels)


def read_frame(path: Path) -> tuple[int, int, list[list[int]]]:
    data = path.read_bytes()[:8]
    if data.startswith(b"BM"):
        return read_bmp(path)
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return read_png_viewport(path)
    raise ValueError("expected BMP or PNG")


def region_values(pixels: list[list[int]], x0: int, y0: int, x1: int, y1: int) -> Counter[int]:
    values: list[int] = []
    for y in range(y0, y1):
        values.extend(pixels[y][x0:x1])
    return Counter(values)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} POKEMON_TITLE.bmp", file=sys.stderr)
        return 2

    width, height, pixels = read_frame(Path(sys.argv[1]))
    if (width, height) != (160, 144):
        return fail(f"unexpected frame size {width}x{height}")

    full = region_values(pixels, 0, 0, width, height)
    sprites = region_values(pixels, 38, 78, 126, 120)
    logo = region_values(pixels, 20, 10, 140, 60)
    expected_shades = {0, 85, 170, 255}
    if set(full) != expected_shades:
        return fail(f"unexpected full-frame shades {sorted(full)}")
    if set(sprites) != expected_shades:
        return fail(f"pokemon title sprites lost shade detail: {sorted(sprites)}")
    if set(logo) != expected_shades:
        return fail(f"pokemon logo lost shade detail: {sorted(logo)}")

    sprite_total = sum(sprites.values())
    sprite_dark = sprites[0] + sprites[85]
    sprite_white = sprites[255]
    dark_frac = sprite_dark / sprite_total
    white_frac = sprite_white / sprite_total

    print(
        "pokemon title: "
        f"sprite_dark={dark_frac:.3f} sprite_white={white_frac:.3f} "
        f"shades={sorted(sprites)}"
    )
    if dark_frac >= 0.35:
        return fail("pokemon title sprites look too dark; possible black-box regression")
    if white_frac <= 0.50:
        return fail("pokemon title sprite background is not mostly clear")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
