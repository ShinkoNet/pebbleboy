#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

PB_QEMU_PHONE_EXTRA_WAIT="${PB_QEMU_PHONE_EXTRA_WAIT:-35}" \
PB_PHONE_ROM_PATH="${PB_PHONE_ROM_PATH:-Pokemon - Crystal Version (UE) (V1.1) [C][!].gbc}" \
PB_PHONE_ROM_TITLE="${PB_PHONE_ROM_TITLE:-PM_CRYSTAL}" \
PB_QEMU_PHONE_CHECK_POKEMON_TITLE=1 \
bash tools/qemu_phone_smoke.sh
