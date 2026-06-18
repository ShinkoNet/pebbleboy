#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh

if [ ! -f roms/tetris.gb ]; then
  echo "missing roms/tetris.gb; smoke needs a local ROM served by URL" >&2
  exit 1
fi

log=build/pebbleboy-qemu.log
: > "$log"
server_log=build/rom-server-qemu.log
: > "$server_log"

rom_path="$(python3 -c 'from pathlib import Path; print(Path("roms/tetris.gb").resolve())')"
rom_base="$(basename "$rom_path")"
url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$rom_base")"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
rom_url="http://127.0.0.1:${port}/${url_path}"
python3 tools/rom_http_server.py "$port" "$rom_path" > "$server_log" 2>&1 &
server_pid=$!
sleep 0.3
python3 tools/seed_phone_rom.py "$rom_url"

pebble kill || true

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

for _ in $(seq 1 60); do
  if grep -q "started TETRIS phone" "$log" &&
     grep -q "fps=" "$log"; then
    break
  fi
  if ! kill -0 "$log_pid" 2>/dev/null; then
    cat "$log"
    echo "pebble install/log command exited before app startup" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$log"

if ! grep -q "App install succeeded." "$log"; then
  echo "missing install success marker in $log" >&2
  exit 1
fi

if ! grep -q "phone info title=TETRIS" "$log"; then
  echo "missing phone ROM info log in $log" >&2
  exit 1
fi

if ! grep -q "started TETRIS phone" "$log"; then
  echo "missing app startup log in $log" >&2
  exit 1
fi

if ! grep -q "fps=" "$log"; then
  echo "missing frame pacing log in $log" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource --expect-phone-title TETRIS --expect-phone-banks 0,1 "$log"

sleep 2
bridge_port="$(python3 - <<'PY'
import json
with open('/tmp/pb-emulator.json') as f:
    print(json.load(f)['emery']['4.15.0-dirty-local']['pypkjs']['port'])
PY
)"
pebble screenshot --phone "localhost:${bridge_port}" build/pebbleboy-qemu.png
python3 tools/check_screenshot.py build/pebbleboy-qemu.png

cleanup
trap - EXIT

echo "qemu smoke passed: build/pebbleboy-qemu.png"
