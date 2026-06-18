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
if ! grep -q "phone SRAM window deferred: 4096/8192" "$log"; then
  echo "SRAM probe did not defer SRAM allocation at startup" >&2
  exit 1
fi
if ! grep -q "phone SRAM window allocated: 4096/8192" "$log"; then
  echo "SRAM probe did not allocate SRAM on first access" >&2
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
  --expect-phone-request-size 16384 --expect-phone-latencies \
  --max-phone-latency-ms 5000 "$log"

if kill -0 "$log_pid" 2>/dev/null; then
  kill "$log_pid" 2>/dev/null || true
  wait "$log_pid" 2>/dev/null || true
fi

python3 tools/check_sram_save.py "$rom_path" --offset 0 --value 0x42

cleanup
trap - EXIT

ram_rom_path="$(python3 -c 'from pathlib import Path; print(Path("build/mbc3_ram_probe_phone.gb").resolve())')"
python3 tools/make_mbc_probe_rom.py "$ram_rom_path" --mbc mbc3 --probe ram
node tools/check_pkjs_hash.js "$ram_rom_path"
python3 tools/check_sram_save.py --clear "$ram_rom_path"

bank_log=build/pebbleboy-sram-bank-qemu.log
bank_server_log=build/rom-server-sram-bank-qemu.log
: > "$bank_log"
: > "$bank_server_log"

ram_rom_base="$(basename "$ram_rom_path")"
ram_url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$ram_rom_base")"
ram_port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
ram_rom_url="http://127.0.0.1:${ram_port}/${ram_url_path}"

pebble kill || true

python3 tools/rom_http_server.py "$ram_port" "$ram_rom_path" > "$bank_server_log" 2>&1 &
bank_server_pid=$!
sleep 0.3
python3 tools/seed_phone_rom.py --scale 1x --audio-disabled "$ram_rom_url"

pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw > "$bank_log" 2>&1 &
bank_log_pid=$!

cleanup_bank() {
  if kill -0 "$bank_server_pid" 2>/dev/null; then
    kill "$bank_server_pid" 2>/dev/null || true
    wait "$bank_server_pid" 2>/dev/null || true
  fi
  if kill -0 "$bank_log_pid" 2>/dev/null; then
    kill "$bank_log_pid" 2>/dev/null || true
    wait "$bank_log_pid" 2>/dev/null || true
  fi
}
trap cleanup_bank EXIT

for _ in $(seq 1 240); do
  saved_bank0="$(grep -c "phone SRAM bank 0 saved" "$bank_log" || true)"
  if grep -q "phone SRAM window deferred: 4096/32768" "$bank_log" &&
     grep -q "phone SRAM window allocated: 4096/32768" "$bank_log" &&
     [ "$saved_bank0" -ge 2 ] &&
     grep -q "phone SRAM bank 2 saved" "$bank_log" &&
     grep -q "phone SRAM bank 4 saved" "$bank_log" &&
     grep -q "phone SRAM bank 6 saved" "$bank_log"; then
    break
  fi
  if ! kill -0 "$bank_log_pid" 2>/dev/null; then
    cat "$bank_log"
    echo "pebble install/log command exited before multi-bank SRAM save" >&2
    exit 1
  fi
  sleep 0.5
done

cat "$bank_log"

if ! grep -q "App install succeeded." "$bank_log"; then
  echo "missing multi-bank install success marker in $bank_log" >&2
  exit 1
fi
if ! grep -q "phone info title=MBC3 RAM" "$bank_log"; then
  echo "missing MBC3 RAM phone ROM info log in $bank_log" >&2
  exit 1
fi
if ! grep -q "started MBC3 RAM phone, save=32768" "$bank_log"; then
  echo "MBC3 RAM probe did not start with 32 KiB save RAM" >&2
  exit 1
fi
if ! grep -q "phone SRAM window deferred: 4096/32768" "$bank_log"; then
  echo "MBC3 RAM probe did not defer SRAM allocation at startup" >&2
  exit 1
fi
if ! grep -q "phone SRAM window allocated: 4096/32768" "$bank_log"; then
  echo "MBC3 RAM probe did not allocate SRAM on first access" >&2
  exit 1
fi
for page in 0 2 4 6; do
  if ! grep -q "phone SRAM load requested bank ${page} size=4096" "$bank_log"; then
    echo "MBC3 RAM probe did not request SRAM page ${page}" >&2
    exit 1
  fi
  if ! grep -q "phone SRAM bank ${page} saved" "$bank_log"; then
    echo "MBC3 RAM probe did not save SRAM page ${page}" >&2
    exit 1
  fi
done
if [ "$(grep -c "phone SRAM bank 0 saved" "$bank_log" || true)" -lt 2 ]; then
  echo "MBC3 RAM probe did not save final success byte back to page 0" >&2
  exit 1
fi

python3 tools/analyze_bank_log.py --expect-no-resource --expect-phone-title "MBC3 RAM" \
  --expect-phone-banks 0 --expect-phone-request-size 4096 --expect-phone-latencies \
  --max-phone-latency-ms 5000 "$bank_log"

if kill -0 "$bank_log_pid" 2>/dev/null; then
  kill "$bank_log_pid" 2>/dev/null || true
  wait "$bank_log_pid" 2>/dev/null || true
fi

python3 tools/check_sram_save.py "$ram_rom_path" --bank 0 --offset 0 --value 0x42
python3 tools/check_sram_save.py "$ram_rom_path" --bank 2 --offset 0 --value 0x61
python3 tools/check_sram_save.py "$ram_rom_path" --bank 4 --offset 0 --value 0x62
python3 tools/check_sram_save.py "$ram_rom_path" --bank 6 --offset 0 --value 0x63

cleanup_bank
trap - EXIT

echo "qemu SRAM smoke passed"
