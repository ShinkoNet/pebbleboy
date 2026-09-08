#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/build-rom.sh ROM.gb[c] [options]

Build a personal Pebbleboy PBW containing a ROM from your filesystem.

Options:
  --output FILE       Output PBW path (default: dist/Pebbleboy-ROM.pbw).
  -h, --help          Show this help.

The unified build supports ROMs up to 8 MiB and uses the 24 KiB cache and
speaker audio. It currently requires Pebbleboy CFW until its APIs land upstream.
No ROM is copied into the Pebbleboy source tree or retained after the build.
EOF
}

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
invocation_dir=$PWD
rom_path=
output_path=
while (($#)); do
  case "$1" in
    --output)
      (($# >= 2)) || { echo "--output requires a path" >&2; exit 2; }
      output_path=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      if [[ -n $rom_path ]]; then
        echo "Only one ROM may be supplied" >&2
        exit 2
      fi
      rom_path=$1
      shift
      ;;
  esac
done

[[ -n $rom_path ]] || { echo "A ROM path is required" >&2; usage >&2; exit 2; }
command -v pebble >/dev/null 2>&1 || {
  echo "pebble was not found in PATH; install and activate the Pebble SDK first" >&2
  exit 1
}

rom_path=$(realpath -- "$rom_path")
[[ -f $rom_path && -r $rom_path ]] || { echo "ROM is not a readable file: $rom_path" >&2; exit 1; }
rom_size=$(stat -c %s -- "$rom_path")
min_size=$((32 * 1024))
max_size=$((8 * 1024 * 1024))
if ((rom_size < min_size || rom_size > max_size || rom_size % 16384 != 0)); then
  echo "Invalid ROM size: $rom_size bytes" >&2
  echo "builds require a 32 KiB-${max_size}-byte ROM in complete 16 KiB banks" >&2
  exit 1
fi

rom_name=$(basename -- "$rom_path")
rom_name=${rom_name%.*}
slug=$(printf '%s' "$rom_name" | sed -E 's/[^A-Za-z0-9._-]+/-/g; s/^-+//; s/-+$//')
[[ -n $slug ]] || slug=game
if [[ -z $output_path ]]; then
  output_path="$repo_dir/dist/Pebbleboy-$slug.pbw"
elif [[ $output_path != /* ]]; then
  output_path="$invocation_dir/$output_path"
fi

temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/pebbleboy-rom-build.XXXXXX")
cleanup() {
  rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

cp -a -- "$repo_dir/package.json" "$repo_dir/wscript" "$repo_dir/src" "$temp_dir/"
mkdir -p -- "$temp_dir/resources/data"
cp -a -- "$repo_dir/resources/images" "$temp_dir/resources/"
install -m 0644 -- "$rom_path" "$temp_dir/resources/data/cartridge.gb"

build_env=(PEBBLEBOY_EMBED_ROM=1)

echo "Building PBW with $rom_name ($rom_size bytes)..."
(cd -- "$temp_dir" && env "${build_env[@]}" pebble build)

pbw_path=$(find "$temp_dir/build" -maxdepth 1 -type f -name '*.pbw' -print -quit)
[[ -n $pbw_path ]] || { echo "Pebble SDK did not produce a PBW" >&2; exit 1; }
mkdir -p -- "$(dirname -- "$output_path")"
install -m 0644 -- "$pbw_path" "$output_path"
echo "Created $output_path"
sha256sum -- "$output_path"
echo "This PBW contains your ROM; keep it personal unless you have redistribution rights."
