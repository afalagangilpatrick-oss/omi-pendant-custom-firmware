#!/bin/bash
# Build Ariel's custom CV1 firmware with the exact toolchain Omi CI uses.
# Runs on any Docker host. Reuses the NCS workspace made by the stock
# baseline build (~/pendant-fw/repo/omi/firmware/v2.9.0) read-write,
# mounted under the custom tree. Output: $OUT (default ~/pendant-fw/out-custom).
set -euo pipefail
SRC=${SRC:-$HOME/pendant-fw/custom}
NCS=${NCS:-$HOME/pendant-fw/repo/omi/firmware/v2.9.0}
OUT=${OUT:-$HOME/pendant-fw/out-custom}
IMG=ghcr.io/zephyrproject-rtos/ci:v0.26.13@sha256:b0ac6334d1926cd0971a0a444f7adc6dd020e88ee3ce865aa070b6475a3ac4eb
mkdir -p "$OUT"
docker run --rm --name pendant-fw-custom \
  -v "$SRC/omi:/omi" -v "$NCS:/omi/firmware/v2.9.0" -v "$OUT:/out" "$IMG" bash -c '
set -euo pipefail
export SOURCE_DATE_EPOCH=0 PYTHONHASHSEED=0 TZ=UTC LC_ALL=C.UTF-8
git config --global --add safe.directory "*"
pip3 install --quiet ecdsa==0.19.0 2>/dev/null || pip3 install --quiet --break-system-packages ecdsa==0.19.0
FW=/omi/firmware
python3 "$FW/scripts/check_cv1_mtu.py" "$FW/omi/src/lib/core/transport.c"
cd "$FW/v2.9.0"
west build -o=-j8 -b omi/nrf5340/cpuapp "$FW/omi" --sysbuild -d build-custom --pristine always \
  -- -DBOARD_ROOT="$FW" -Domi_CONF_FILE="$FW/omi/omi.conf"
test -s build-custom/dfu_application.zip
cp build-custom/dfu_application.zip build-custom/omi/zephyr/zephyr.signed.bin build-custom/omi/zephyr/zephyr.elf /out/
cp build-custom/omi/zephyr/.config /out/app.config
COMPILER=$(sed -n "s/^CMAKE_C_COMPILER:[^=]*=//p" build-custom/mcuboot/CMakeCache.txt)
python3 "$FW/scripts/ci/prove_cv1_bootloader.py" --build build-custom --objdump "${COMPILER%gcc}objdump" --out /out/cv1-bootloader
chmod -R a+rwX /out build-custom
echo "CUSTOM BUILD OK"
'
