#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh
python3 tools/seed_phone_rom.py --clear

log=build/pebbleboy-qemu.log
: > "$log"

pebble kill || true

pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw > "$log" 2>&1 &
log_pid=$!

cleanup() {
  if kill -0 "$log_pid" 2>/dev/null; then
    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 60); do
  if grep -q "fps=" "$log"; then
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

if ! grep -q "started TETRIS local" "$log"; then
  echo "missing app startup log in $log" >&2
  exit 1
fi

if ! grep -q "fps=" "$log"; then
  echo "missing frame pacing log in $log" >&2
  exit 1
fi

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
