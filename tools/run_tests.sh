#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

python3 tools/sync_roms.py
mkdir -p build

node tools/check_pkjs_hash.js roms/tetris.gb
node tools/pkjs_cache_test.js
python3 tools/make_sram_probe_rom.py build/sram_probe.gb --value 0x42
python3 tools/make_mbc_probe_rom.py build/mbc1_probe.gb --mbc mbc1
python3 tools/make_mbc_probe_rom.py build/mbc3_probe.gb --mbc mbc3
python3 tools/make_mbc_probe_rom.py build/mbc5_probe.gb --mbc mbc5
python3 tools/make_mbc_probe_rom.py build/mbc1_ram_probe.gb --mbc mbc1 --probe ram
python3 tools/make_mbc_probe_rom.py build/mbc3_ram_probe.gb --mbc mbc3 --probe ram
python3 tools/make_mbc_probe_rom.py build/mbc5_ram_probe.gb --mbc mbc5 --probe ram

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/desktop_harness.c \
  src/c/peanut_gb.c src/c/gb_hooks.c src/c/gb_audio.c src/c/gb_cart.c src/c/gb_video.c \
  -o build/desktop_harness

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/cart_cache_test.c src/c/gb_cart.c \
  -o build/cart_cache_test
build/cart_cache_test

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -DPB_CART_CACHE_BANKS=3 -Isrc/c \
  tools/cart_cache_profile.c src/c/gb_cart.c \
  -o build/cart_cache_profile_3
build/cart_cache_profile_3

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -DPB_CART_CACHE_BANKS=4 -Isrc/c \
  tools/cart_cache_profile.c src/c/gb_cart.c \
  -o build/cart_cache_profile_4
build/cart_cache_profile_4

cc -std=c99 -Wall -Wextra -Werror -DPB_DESKTOP -Isrc/c \
  tools/input_test.c src/c/gb_input.c \
  -o build/input_test
build/input_test

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

for mbc in 1 3 5; do
  mbc_out="$(build/desktop_harness "build/mbc${mbc}_probe.gb" 20)"
  echo "$mbc_out"
  case "$mbc_out" in
    *"title=\"MBC${mbc} PROBE\""*"mbc=${mbc} banks=4 save=8192 save0=42 save_nonff=1"*)
      ;;
    *)
      echo "unexpected MBC${mbc} probe profile" >&2
      exit 1
      ;;
  esac
done

for mbc in 1 3 5; do
  mbc_ram_out="$(build/desktop_harness "build/mbc${mbc}_ram_probe.gb" 20)"
  echo "$mbc_ram_out"
  case "$mbc_ram_out" in
    *"title=\"MBC${mbc} RAM\""*"mbc=${mbc} banks=4 save=32768 save0=42 save_nonff=4"*)
      ;;
    *)
      echo "unexpected MBC${mbc} RAM probe profile" >&2
      exit 1
      ;;
  esac
done

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
