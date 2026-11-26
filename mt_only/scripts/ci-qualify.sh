#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Each private x86-64 Ubuntu runner uses an isolated, job-local tool directory.
set -euo pipefail
group=${1:?usage: ci-qualify.sh native|nordic|esp32}
case "$group" in native|nordic|esp32) ;; *) exit 2 ;; esac
root=$(git rev-parse --show-toplevel)
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/zephtastic-build.XXXXXXXX")
if [[ -n ${GITHUB_ENV:-} ]]; then
  printf 'ZT_CI_WORK=%s\n' "$work" >> "$GITHUB_ENV"
fi
cd "$root"
source mt_only/scripts/ci-runner-tools.sh
install_runner_tools "$group" "$work"

fetch_commit() {
  local dest=$1 url=$2 commit=$3
  git init -q "$dest"
  git -C "$dest" fetch -q --depth=1 "$url" "$commit"
  git -C "$dest" checkout -q --detach FETCH_HEAD
  test "$(git -C "$dest" rev-parse HEAD)" = "$commit"
}

if [[ $group == native ]]; then
  fetch_commit "$work/stock" https://github.com/meshtastic/firmware.git d5f16191ca28b01b5024f590f04a67f25d66f1b1
  export CC=clang CXX=clang++
  ulimit -s 65536
  python -m pip -q install meshtastic==2.7.11
  python mt_only/scripts/test-upstream-282.py --stock "$work/stock" --output "$work/upstream"
  for abi in native short; do
    args=()
    if [[ $abi == short ]]; then args+=(--short-enums); fi
    python3 mt_only/scripts/test-role-channel.py --stock "$work/stock" --output "$work/roles-$abi" "${args[@]}"
  done
  python mt_only/tests/stock_operating_roles/run.py --release 2.8.2 --stock "$work/stock"
  exit 0
fi

sdk="$work/zephyr-sdk-1.0.1"
install_sdk_archive() {
  local name=$1 digest=$2 dest=$3
  local archive="$work/$name"
  curl --fail --silent --show-error --location --retry 3 --output "$archive" \
    "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/$name"
  printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
  mkdir -p "$dest"
  tar -xJf "$archive" -C "$dest"
}
install_sdk_archive zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz \
  ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb "$work"
if [[ $group == nordic ]]; then
  target=arm-zephyr-eabi
  digest=21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634
  profiles=(t096-680 t096-728 t114-v2 rak4631-mini)
else
  target=xtensa-espressif_esp32s3_zephyr-elf
  digest=904d42b75e4d819c58b0db640783911e508b9ee2a627eb5daffb907465a34be1
  profiles=(xiao-ordinary v4.2-r2 v4.3-r2 v4.3-r8)
fi
install_sdk_archive "toolchain_gnu_linux-x86_64_${target}.tar.xz" "$digest" "$sdk/gnu"
python -m pip -q install cmake==3.28.4 ninja==1.11.1.4
# Zephyr finds the compiler in gnu/ and the release builder also reads this alias.
# DTC and gperf come from the runner's job-local packages.
ln -s "gnu/$target" "$sdk/$target"
command -v dtc gperf
python mt_only/scripts/prepare-deps.py "$work/deps"
python -m pip -q install -r "$work/deps/zephyr/scripts/requirements-base.txt"
args=()
if [[ $group == esp32 ]]; then
  fetch_commit "$work/mcuboot" https://github.com/mcu-tools/mcuboot.git aa32eaaad41cc345e6f6e6633368a4766fe23c25
  python -m pip -q install -r "$work/deps/modules/hal/espressif/zephyr/requirements.txt" "$work/mcuboot/scripts"
  args+=(--mcuboot "$work/mcuboot")
fi
for profile in "${profiles[@]}"; do
  python mt_only/scripts/ci-board-build.py "$profile" --deps "$work/deps" \
    --sdk "$sdk" --build "$work/$profile" --jobs 4 "${args[@]}"
done
