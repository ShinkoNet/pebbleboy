#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

rom_root="${GB_TEST_ROMS_ROOT:-../game-boy-test-roms/releases/v7.0/roms}"
if [ ! -d "$rom_root" ]; then
  echo "Game Boy test ROMs not found at $rom_root" >&2
  echo "Set GB_TEST_ROMS_ROOT to the releases/v7.0/roms directory." >&2
  exit 2
fi

mkdir -p build/compat
cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/desktop_harness.c \
  src/c/peanut_gb.c src/c/gb_hooks.c src/c/gb_audio.c src/c/gb_cart.c \
  src/c/gb_video.c -o build/desktop_harness

run_frame_test() {
  local name="$1"
  local rom="$2"
  local frames="$3"
  local reference="$4"
  local actual="build/compat/${name}.bmp"
  local log="build/compat/${name}.log"

  PB_DESKTOP_SERIAL=1 build/desktop_harness "$rom" "$frames" "$actual" >"$log"
  python3 tools/compare_frames.py "$actual" "$reference"
  echo "$name passed at frame $frames"
}

run_frame_test \
  cpu_instrs \
  "$rom_root/blargg/cpu_instrs/cpu_instrs.gb" \
  3300 \
  "$rom_root/blargg/cpu_instrs/cpu_instrs-dmg-cgb.png"

run_frame_test \
  instr_timing \
  "$rom_root/blargg/instr_timing/instr_timing.gb" \
  120 \
  "$rom_root/blargg/instr_timing/instr_timing-dmg-cgb.png"

run_frame_test \
  dmg_acid2 \
  "$rom_root/dmg-acid2/dmg-acid2.gb" \
  100 \
  "$rom_root/dmg-acid2/dmg-acid2-dmg.png"

echo "Focused DMG compatibility tests passed."
echo "Cycle-exact halt_bug and mem_timing suites are intentionally non-gating."
