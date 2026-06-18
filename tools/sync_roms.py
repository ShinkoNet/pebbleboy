#!/usr/bin/env python3
"""Copy local test ROMs into the Pebble resource tree.

ROMs are intentionally gitignored and must be supplied by the developer.
"""

from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "resources" / "data"
ROMS = ROOT / "roms"


def copy_if_present(name: str, required: bool = False) -> bool:
    candidates = [ROOT / name, ROMS / name]
    src = next((p for p in candidates if p.exists() and p.is_file()), None)
    if not src:
        if required:
            print(f"missing required ROM: {name}", file=sys.stderr)
        return False

    RES.mkdir(parents=True, exist_ok=True)
    ROMS.mkdir(parents=True, exist_ok=True)
    res_dst = RES / name
    rom_dst = ROMS / name
    if src.resolve() != res_dst.resolve():
        shutil.copyfile(src, res_dst)
    if src.resolve() != rom_dst.resolve():
        shutil.copyfile(src, rom_dst)
    print(f"synced {name}: {src.relative_to(ROOT)} -> resources/data/{name}")
    return True


def main() -> int:
    ok = copy_if_present("tetris.gb", required=True)
    copy_if_present("pokered.gb", required=False)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

