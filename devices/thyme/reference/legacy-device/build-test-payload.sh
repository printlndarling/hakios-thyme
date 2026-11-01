#!/bin/bash
# Build a RAM-only diagnostic payload from the current THYME kernel and root.
#
# usage: devices/thyme/build-test-payload.sh NAME "extra kernel arguments"
#
# The stable GPU, touch, keys, charging and battery-safety arguments are always
# present. The caller adds only the feature under test, so failures stay
# attributable to one subsystem.
set -euo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
OUT="$ROOT/out/thyme"
KERNEL="$OUT/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
ROOT_IMAGE="$OUT/root.apfs"

[ "$#" -ge 1 ] || {
  echo "usage: $0 NAME [extra kernel arguments]" >&2
  exit 2
}

name="$1"
shift
[[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || {
  echo "invalid payload name: $name" >&2
  exit 2
}
[ -f "$KERNEL" ] || {
  echo "missing kernel: $KERNEL (run devices/thyme/build-kernel.sh)" >&2
  exit 1
}
[ -f "$ROOT_IMAGE" ] || {
  echo "missing root image: $ROOT_IMAGE (run devices/thyme/build-userland.sh)" >&2
  exit 1
}

base="loader_return_after=0 epoch=$(date +%s) sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 cpufreq=1 gpu=1 battery=1 battery_charge=1 battery_stop=25 battery_stop_poll=30"
extra="$*"
payload="$OUT/run-payload-$name.bin"

python3 "$ROOT/xnuloader/mkpayload.py" -o "$payload" \
  "kernel=$KERNEL" "ramdisk=$ROOT_IMAGE" --cmdline "$base $extra"
sha256sum "$payload"
