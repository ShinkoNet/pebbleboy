#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/Pebbleboy.pbw ]; then
  pebble build
fi
if [ ! -f roms/tetris.gb ]; then
  echo "missing roms/tetris.gb; library smoke needs a local test ROM" >&2
  exit 1
fi

log=build/pebbleboy-library-qemu.log
server_log=build/rom-server-library-qemu.log
screenshot=build/pebbleboy-library-qemu.png
: > "$log"
: > "$server_log"

rom_path="$(python3 -c 'from pathlib import Path; print(Path("roms/tetris.gb").resolve())')"
rom_base="$(basename "$rom_path")"
url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$rom_base")"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
rom_url="http://127.0.0.1:${port}/${url_path}"

pebble kill || true
python3 tools/rom_http_server.py "$port" "$rom_path" > "$server_log" 2>&1 &
server_pid=$!
log_pid=

cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [ -n "${log_pid:-}" ] && kill -0 "$log_pid" 2>/dev/null; then
    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

sleep 0.3
python3 tools/seed_phone_rom.py --name Tetris --extra-rom "Second Game=${rom_url}" \
  --audio-disabled "$rom_url"

pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw > "$log" 2>&1 &
log_pid=$!

for _ in $(seq 1 60); do
  if grep -q "ROM library entries=2" "$log"; then
    break
  fi
  sleep 0.25
done
if ! grep -q "ROM library entries=2" "$log"; then
  cat "$log"
  echo "watch did not receive the two-entry ROM library" >&2
  exit 1
fi

sleep 1
python3 tools/qemu_screendump.py "$screenshot"
python3 tools/qemu_button.py down select

for _ in $(seq 1 120); do
  if grep -q "selected ROM 1 Second Game" "$log" &&
     grep -q "started TETRIS phone" "$log"; then
    break
  fi
  sleep 0.25
done

cat "$log"
if ! grep -q "selected ROM 1 Second Game" "$log"; then
  echo "watch selector did not choose the second ROM" >&2
  exit 1
fi
if ! grep -q "started TETRIS phone" "$log"; then
  echo "selected ROM did not start" >&2
  exit 1
fi

cleanup
trap - EXIT
echo "qemu ROM library smoke passed: $screenshot"
