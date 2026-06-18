#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh

if [ ! -f roms/pokered.gb ]; then
  echo "missing roms/pokered.gb; phone streaming smoke needs a local large test ROM" >&2
  exit 1
fi

mkdir -p build
log=build/pebbleboy-phone-qemu.log
screenshot=build/pebbleboy-phone-qemu.png
server_log=build/rom-server-phone-qemu.log
: > "$log"
: > "$server_log"

rom_path="$(python3 -c 'from pathlib import Path; print(Path("roms/pokered.gb").resolve())')"
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
  if grep -q "started POKEMON RED phone" "$log" &&
     grep -q "phone bank 1 ready" "$log" &&
     grep -q "phone bank 4 ready" "$log" &&
     grep -q "phone bank 28 ready" "$log" &&
     grep -q "fps=" "$log"; then
    break
  fi
  if ! kill -0 "$log_pid" 2>/dev/null; then
    cat "$log"
    echo "pebble install/log command exited before phone ROM startup" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$log"

if ! grep -q "App install succeeded." "$log"; then
  echo "missing install success marker in $log" >&2
  exit 1
fi
if ! grep -q "phone info title=POKEMON RED" "$log"; then
  echo "missing phone ROM info log in $log" >&2
  exit 1
fi
if grep -q "started TETRIS local" "$log"; then
  echo "phone smoke unexpectedly started bundled Tetris" >&2
  exit 1
fi
if ! grep -q "phone bank 0 ready" "$log"; then
  echo "missing streamed bank 0 completion in $log" >&2
  exit 1
fi
if ! grep -q "phone bank 1 ready" "$log"; then
  echo "missing streamed switch bank completion in $log" >&2
  exit 1
fi
if ! grep -q "phone bank 4 ready" "$log"; then
  echo "missing later streamed switch bank completion in $log" >&2
  exit 1
fi
if ! grep -q "phone bank 28 ready" "$log"; then
  echo "missing high-bank streamed completion in $log" >&2
  exit 1
fi
if ! grep -q "started POKEMON RED phone" "$log"; then
  echo "missing phone-backed Pokemon startup in $log" >&2
  exit 1
fi
if ! grep -q "phone SRAM window deferred: 4096/32768" "$log"; then
  echo "phone-backed Pokemon did not defer the expected SRAM window" >&2
  exit 1
fi
if ! grep -Eq "started POKEMON RED phone, save=32768, heap free=([8-9][0-9]{3}|[1-9][0-9]{4,})" "$log"; then
  echo "phone-backed Pokemon did not start with deferred SRAM heap headroom" >&2
  exit 1
fi
if ! grep -q "fps=" "$log"; then
  echo "missing frame pacing log in $log" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource --expect-phone-title "POKEMON RED" \
  --expect-phone-request-size 4096 --expect-phone-latencies \
  --allow-pending-phone-request --max-phone-latency-ms 5000 "$log"

if kill -0 "$log_pid" 2>/dev/null; then
  kill "$log_pid" 2>/dev/null || true
  wait "$log_pid" 2>/dev/null || true
fi

sleep "${PB_QEMU_PHONE_EXTRA_WAIT:-5}"
python3 tools/qemu_screendump.py "$screenshot"
python3 tools/check_screenshot.py --allow-loading "$screenshot"

cleanup
trap - EXIT

echo "qemu phone smoke passed: $screenshot"
