#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bash tools/run_tests.sh

mkdir -p build
rom_path="$(python3 -c 'from pathlib import Path; print(Path("build/sram_probe.gb").resolve())')"
python3 tools/make_sram_probe_rom.py "$rom_path" --value 0x42
node tools/check_pkjs_hash.js "$rom_path"
python3 tools/check_sram_save.py --clear "$rom_path"

log=build/pebbleboy-sram-qemu.log
server_log=build/rom-server-sram-qemu.log
: > "$log"
: > "$server_log"

rom_base="$(basename "$rom_path")"
url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$rom_base")"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
rom_url="http://127.0.0.1:${port}/${url_path}"

pebble kill || true

python3 tools/rom_http_server.py "$port" "$rom_path" > "$server_log" 2>&1 &
server_pid=$!
sleep 0.3
python3 tools/seed_phone_rom.py --scale 1x --audio-disabled "$rom_url"

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

for _ in $(seq 1 180); do
  if grep -q "phone SRAM load requested bank 0 size=4096" "$log" &&
     grep -q "pebbleboy: SRAM load bank 0 size=4096" "$log" &&
     grep -q "phone SRAM bank 0 loaded" "$log" &&
     grep -q "phone SRAM save started bank 0 size=4096" "$log" &&
     grep -q "phone SRAM bank 0 saved" "$log"; then
    break
  fi
  if ! kill -0 "$log_pid" 2>/dev/null; then
    cat "$log"
    echo "pebble install/log command exited before SRAM save" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$log"

if ! grep -q "App install succeeded." "$log"; then
  echo "missing install success marker in $log" >&2
  exit 1
fi
if ! grep -q "phone info title=SRAM PROBE" "$log"; then
  echo "missing SRAM probe phone ROM info log in $log" >&2
  exit 1
fi
if ! grep -q "started SRAM PROBE phone, save=8192" "$log"; then
  echo "SRAM probe did not start with save RAM enabled" >&2
  exit 1
fi
if ! grep -q "phone SRAM load requested bank 0 size=4096" "$log"; then
  echo "SRAM probe did not request an SRAM load" >&2
  exit 1
fi
if ! grep -q "phone SRAM bank 0 loaded" "$log"; then
  echo "SRAM probe did not finish SRAM load" >&2
  exit 1
fi
if ! grep -q "phone SRAM save started bank 0 size=4096" "$log"; then
  echo "SRAM probe did not start SRAM save" >&2
  exit 1
fi
if ! grep -q "phone SRAM bank 0 saved" "$log"; then
  echo "SRAM probe did not finish SRAM save" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource --expect-phone-title "SRAM PROBE" \
  --expect-phone-banks 0 --expect-phone-request-count 1 \
  --expect-phone-request-size 16384 "$log"

if kill -0 "$log_pid" 2>/dev/null; then
  kill "$log_pid" 2>/dev/null || true
  wait "$log_pid" 2>/dev/null || true
fi

python3 tools/check_sram_save.py "$rom_path" --offset 0 --value 0x42

cleanup
trap - EXIT

echo "qemu SRAM smoke passed"
