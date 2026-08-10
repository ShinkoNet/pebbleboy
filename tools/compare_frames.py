#!/usr/bin/env python3
"""Compare a desktop-harness BMP with a reference PNG."""

from __future__ import annotations

import sys
from pathlib import Path

from check_pokemon_title import read_bmp
from check_screenshot import read_png


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} ACTUAL.bmp EXPECTED.png", file=sys.stderr)
        return 2

    actual_width, actual_height, actual = read_bmp(Path(sys.argv[1]))
    expected_width, expected_height, rgba = read_png(Path(sys.argv[2]))
    if (actual_width, actual_height) != (expected_width, expected_height):
        print(
            f"frame dimensions differ: actual={actual_width}x{actual_height} "
            f"expected={expected_width}x{expected_height}",
            file=sys.stderr,
        )
        return 1

    mismatches = []
    mismatch_count = 0
    for y in range(actual_height):
        for x in range(actual_width):
            i = (y * expected_width + x) * 4
            expected = tuple(rgba[i:i + 3])
            value = actual[y][x]
            if expected != (value, value, value):
                mismatch_count += 1
                if len(mismatches) < 8:
                    mismatches.append((x, y, value, expected))

    print(
        f"frames={actual_width}x{actual_height} mismatches={mismatch_count} "
        f"examples={mismatches}"
    )
    return 1 if mismatch_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
