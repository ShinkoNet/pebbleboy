#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh

rom_source="${PB_PHONE_ROM_PATH:-roms/tetris.gb}"
rom_title="${PB_PHONE_ROM_TITLE:-TETRIS}"
if [ ! -f "$rom_source" ] && [ -z "${PB_PHONE_ROM_URL:-}" ]; then
  echo "missing $rom_source; flash install smoke needs a local test ROM" >&2
  exit 1
fi

mkdir -p build
log=build/pebbleboy-phone-qemu.log
screenshot=build/pebbleboy-phone-qemu.png
server_log=build/rom-server-phone-qemu.log
: > "$log"
: > "$server_log"

rom_path="$(python3 -c 'import sys; from pathlib import Path; print(Path(sys.argv[1]).resolve())' "$rom_source")"
rom_base="$(basename "$rom_path")"
url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$rom_base")"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
rom_url="${PB_PHONE_ROM_URL:-http://127.0.0.1:${port}/${url_path}}"

pebble kill || true

log_pid=
server_pid=
cleanup() {
  if [ -n "${server_pid:-}" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [ -n "${log_pid:-}" ] && kill -0 "$log_pid" 2>/dev/null; then
    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ -z "${PB_PHONE_ROM_URL:-}" ]; then
  python3 tools/rom_http_server.py "$port" "$rom_path" > "$server_log" 2>&1 &
  server_pid=$!
  sleep 0.3
fi

python3 tools/seed_phone_rom.py --scale 1x --audio-disabled "$rom_url"

pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw > "$log" 2>&1 &
log_pid=$!

for _ in $(seq 1 240); do
  if grep -Fq "ROM installed and verified on watch" "$log" &&
     grep -Fq "started ${rom_title} flash" "$log" &&
     grep -q "fps=" "$log"; then
    break
  fi
  if ! kill -0 "$log_pid" 2>/dev/null; then
    cat "$log"
    echo "pebble install/log command exited before flash ROM startup" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$log"

if ! grep -q "App install succeeded." "$log"; then
  echo "missing install success marker in $log" >&2
  exit 1
fi
if ! grep -Fq "phone info title=${rom_title}" "$log"; then
  echo "missing phone ROM info log in $log" >&2
  exit 1
fi
if ! grep -q "ROM installed and verified on watch" "$log"; then
  echo "phone did not confirm the flash install in $log" >&2
  exit 1
fi
if ! grep -Fq "started ${rom_title} flash" "$log"; then
  echo "installed $rom_title did not start from flash in $log" >&2
  exit 1
fi
if grep -q "URL installs require CFW" "$log"; then
  echo "CFW build fell back to the stock URL-install path" >&2
  exit 1
fi
if grep -q "cart: phone request bank" "$log"; then
  echo "flash-backed ROM unexpectedly requested a phone bank" >&2
  exit 1
fi
if ! grep -q "fps=" "$log"; then
  echo "missing frame pacing log in $log" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource \
  --expect-phone-request-count 0 "$log"

if kill -0 "$log_pid" 2>/dev/null; then
  kill "$log_pid" 2>/dev/null || true
  wait "$log_pid" 2>/dev/null || true
fi

sleep "${PB_QEMU_PHONE_EXTRA_WAIT:-5}"
python3 tools/qemu_screendump.py "$screenshot"
python3 tools/check_screenshot.py --allow-loading "$screenshot"
if [ "${PB_QEMU_PHONE_CHECK_POKEMON_TITLE:-0}" = "1" ]; then
  python3 tools/check_pokemon_title.py "$screenshot"
fi

cleanup
trap - EXIT

echo "qemu phone smoke passed: $screenshot"
