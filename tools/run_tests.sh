#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 tools/sync_roms.py
mkdir -p build

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/desktop_harness.c \
  src/c/peanut_gb.c src/c/gb_hooks.c src/c/gb_cart.c src/c/gb_video.c \
  -o build/desktop_harness

build/desktop_harness roms/tetris.gb 180

if [ -f roms/pokered.gb ]; then
  build/desktop_harness roms/pokered.gb 240
fi

pebble build

