#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f roms/pokered.gb ]; then
  echo "missing roms/pokered.gb; phone streaming smoke needs a local large test ROM" >&2
  exit 1
fi

bash tools/run_tests.sh

mkdir -p build
log=build/pebbleboy-phone-qemu.log
screenshot=build/pebbleboy-phone-qemu.png
: > "$log"

rom_url="${PB_PHONE_ROM_URL:-phone-cache://pokered.gb}"

pebble kill || true

log_pid=
cleanup() {
  if [ -n "${log_pid:-}" ] && kill -0 "$log_pid" 2>/dev/null; then
    kill "$log_pid" 2>/dev/null || true
    wait "$log_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

python3 tools/seed_phone_rom.py "$rom_url" --rom-file roms/pokered.gb

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
if ! grep -Eq "started POKEMON RED phone, save=0, heap free=1[0-9]{4}" "$log"; then
  echo "phone-backed Pokemon did not start with expected save-disabled heap margin" >&2
  exit 1
fi
if ! grep -q "fps=" "$log"; then
  echo "missing frame pacing log in $log" >&2
  exit 1
fi

sleep 5
bridge_port="$(python3 - <<'PY'
import json
with open('/tmp/pb-emulator.json') as f:
    print(json.load(f)['emery']['4.15.0-dirty-local']['pypkjs']['port'])
PY
)"
pebble screenshot --phone "localhost:${bridge_port}" "$screenshot"
python3 tools/check_screenshot.py --allow-loading "$screenshot"

cleanup
trap - EXIT

echo "qemu phone smoke passed: $screenshot"
