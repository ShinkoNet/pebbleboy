#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh

if [ ! -f roms/tetris.gb ]; then
  echo "missing roms/tetris.gb; audio smoke needs a local ROM served by URL" >&2
  exit 1
fi

log=build/pebbleboy-audio-qemu.log
server_log=build/rom-server-audio-qemu.log
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
sleep 0.3
python3 tools/seed_phone_rom.py --scale 1x --audio-enabled "$rom_url"

pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw > "$log" 2>&1 &
log_pid=$!

cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if kill -0 "$log_pid" 2>/dev/null; then
    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 80); do
  if grep -q "speaker stream enabled" "$log" &&
     grep -q "started TETRIS phone" "$log" &&
     grep -q "phone bank 1 ready" "$log"; then
    break
  fi
  if ! kill -0 "$log_pid" 2>/dev/null; then
    cat "$log"
    echo "pebble install/log command exited before audio startup" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$log"

if ! grep -q "App install succeeded." "$log"; then
  echo "missing install success marker in $log" >&2
  exit 1
fi
if ! grep -q "speaker stream enabled" "$log"; then
  echo "missing speaker stream startup in $log" >&2
  exit 1
fi
if ! grep -q "started TETRIS phone" "$log"; then
  echo "missing Tetris startup in $log" >&2
  exit 1
fi
if ! grep -q "phone bank 1 ready" "$log"; then
  echo "missing streamed bank 1 completion in $log" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource --expect-phone-title TETRIS --expect-phone-banks 0,1 "$log"

cleanup
trap - EXIT

echo "qemu audio smoke passed"
