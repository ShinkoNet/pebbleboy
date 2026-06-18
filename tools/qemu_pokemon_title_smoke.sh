#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

PB_QEMU_PHONE_EXTRA_WAIT="${PB_QEMU_PHONE_EXTRA_WAIT:-35}" \
PB_QEMU_PHONE_CHECK_POKEMON_TITLE=1 \
bash tools/qemu_phone_smoke.sh
