#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 tools/sync_roms.py
mkdir -p build

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/desktop_harness.c \
  src/c/peanut_gb.c src/c/gb_hooks.c src/c/gb_audio.c src/c/gb_cart.c src/c/gb_video.c \
  -o build/desktop_harness

tetris_out="$(build/desktop_harness roms/tetris.gb 180)"
echo "$tetris_out"
case "$tetris_out" in
  *'title="TETRIS"'*'mbc=0 banks=2'*'load_banks=0,1 request_banks=none'*)
    ;;
  *)
    echo "unexpected Tetris bank profile" >&2
    exit 1
    ;;
esac

if [ -f roms/pokered.gb ]; then
  build/desktop_harness roms/pokered.gb 240
fi

pebble build
