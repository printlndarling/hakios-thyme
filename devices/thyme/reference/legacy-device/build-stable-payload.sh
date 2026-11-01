#!/bin/bash
# Build the RAM-only daily-use payload. loader_return_after=0 disables the
# test watchdog. When that watchdog expires, the GPU and touch paths are torn
# down, backboardd crashes, and the screen flashes text over SpringBoard.
set -euo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
OUT="$ROOT/out/thyme"
KERNEL="$OUT/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
ROOT_IMAGE="$OUT/root.apfs"
PAYLOAD="$OUT/run-payload-stable.bin"

[ -f "$KERNEL" ] || {
  echo "missing kernel: $KERNEL (run devices/thyme/build-kernel.sh)" >&2
  exit 1
}
[ -f "$ROOT_IMAGE" ] || {
  echo "missing root image: $ROOT_IMAGE (run devices/thyme/build-userland.sh)" >&2
  exit 1
}

cmdline="loader_return_after=0 epoch=$(date +%s) sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 cpufreq=1 gpu=1 battery=1 battery_charge=1 battery_stop=25 battery_stop_poll=30"
python3 "$ROOT/xnuloader/mkpayload.py" -o "$PAYLOAD" "kernel=$KERNEL" "ramdisk=$ROOT_IMAGE" --cmdline "$cmdline"
sha256sum "$PAYLOAD"
