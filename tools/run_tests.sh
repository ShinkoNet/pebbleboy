#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 tools/sync_roms.py
mkdir -p build

node tools/check_pkjs_hash.js roms/tetris.gb
python3 tools/make_sram_probe_rom.py build/sram_probe.gb --value 0x42

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/desktop_harness.c \
  src/c/peanut_gb.c src/c/gb_hooks.c src/c/gb_audio.c src/c/gb_cart.c src/c/gb_video.c \
  -o build/desktop_harness

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/cart_cache_test.c src/c/gb_cart.c \
  -o build/cart_cache_test
build/cart_cache_test

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/audio_mixer_test.c src/c/gb_audio.c \
  -o build/audio_mixer_test
build/audio_mixer_test

tetris_out="$(build/desktop_harness roms/tetris.gb 180)"
echo "$tetris_out"
case "$tetris_out" in
  *'title="TETRIS"'*'mbc=0 banks=2'*'loads=2 load_banks=0,1 request_banks=none'*)
    ;;
  *)
    echo "unexpected Tetris bank profile" >&2
    exit 1
    ;;
esac

sram_out="$(build/desktop_harness build/sram_probe.gb 20)"
echo "$sram_out"
case "$sram_out" in
  *'title="SRAM PROBE"'*'mbc=1 banks=2 save=8192 save0=42 save_nonff=1'*)
    ;;
  *)
    echo "unexpected SRAM probe profile" >&2
    exit 1
    ;;
esac

if [ -f roms/pokered.gb ]; then
  pokered_boot_out="$(build/desktop_harness roms/pokered.gb 240)"
  echo "$pokered_boot_out"
  case "$pokered_boot_out" in
    *'title="POKEMON RED"'*'mbc=3 banks=64 save=32768'*)
      ;;
    *)
      echo "unexpected Pokemon boot profile" >&2
      exit 1
      ;;
  esac

  pokered_title_out="$(build/desktop_harness roms/pokered.gb 1500 build/pokered-title.bmp)"
  echo "$pokered_title_out"
  case "$pokered_title_out" in
    *'title="POKEMON RED"'*'hash=00ec8ed4'*)
      ;;
    *)
      echo "unexpected Pokemon title frame hash" >&2
      exit 1
      ;;
  esac
  python3 tools/check_pokemon_title.py build/pokered-title.bmp
fi

pebble build
