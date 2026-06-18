#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
  cat <<'EOF'
usage: tools/launch-emu.sh [--no-build] ROM.gb
       tools/launch-emu.sh [--no-build] --ROM.gb
       tools/launch-emu.sh [--no-build] --rom ROM.gb

Builds Pebbleboy, hosts the ROM over a local HTTP URL, configures pypkjs with
that URL, and launches the visible Emery emulator with logs attached.
EOF
}

build=1
rom_arg=

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --no-build)
      build=0
      ;;
    --rom)
      shift
      if [ "$#" -eq 0 ]; then
        echo "--rom needs a ROM path" >&2
        exit 1
      fi
      rom_arg="$1"
      ;;
    --*.gb|--*.gbc)
      rom_arg="${1#--}"
      ;;
    *)
      if [ -n "$rom_arg" ]; then
        echo "only one ROM may be supplied" >&2
        exit 1
      fi
      rom_arg="$1"
      ;;
  esac
  shift
done

if [ -z "$rom_arg" ]; then
  usage >&2
  exit 1
fi

rom_path=
for candidate in "$rom_arg" "roms/$rom_arg"; do
  if [ -f "$candidate" ]; then
    rom_path="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$candidate")"
    break
  fi
done

if [ -z "$rom_path" ]; then
  echo "ROM not found: $rom_arg" >&2
  exit 1
fi

rom_base="$(basename "$rom_path")"
url_path="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$rom_base")"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
rom_url="http://127.0.0.1:${port}/${url_path}"
server_log="build/rom-server.log"

mkdir -p build

if [ "$build" -eq 1 ]; then
  pebble build
fi

python3 tools/rom_http_server.py "$port" "$rom_path" > "$server_log" 2>&1 &
server_pid=$!

cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

sleep 0.3
if ! kill -0 "$server_pid" 2>/dev/null; then
  cat "$server_log" >&2
  echo "ROM HTTP server failed to start" >&2
  exit 1
fi

python3 tools/seed_phone_rom.py "$rom_url"

echo "ROM URL: $rom_url"
echo "Server log: $server_log"

pebble kill || true
pebble install --emulator emery --vnc --logs build/Pebbleboy.pbw
