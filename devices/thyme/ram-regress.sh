#!/bin/sh
# Boot one complete THYME baseline and run the Wi-Fi + Bluetooth regression scenario.
#
# This script exists so a focused test cannot silently drop another subsystem from the kernel
# command line. The scenario may change, but every RAM regression run uses devices/thyme/ram.args.
#
#   devices/thyme/ram-regress.sh [scenario.apfs]
#
# The default scenario performs a Wi-Fi scan, a power cycle and a second scan, then opens Bluetooth.
# ALLOW_DIRTY=1 permits a test build from a modified tracked tree; it is recorded in the output.
set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
cd "$ROOT"

export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
SCENARIO=${1:-out/thyme-boot/root-wirelessregress.apfs}
SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SCENARIO" ] || { echo "ram-regress: no scenario image: $SCENARIO" >&2; exit 1; }
[ -f "$SYSTEM_IMAGE" ] || { echo "ram-regress: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -x hakios-kernel ] || { echo "ram-regress: no built kernel; run make kernel" >&2; exit 1; }

DIRTY=$(git status --porcelain --untracked-files=normal)
if [ -n "$DIRTY" ] && [ "${ALLOW_DIRTY:-0}" != 1 ]; then
	echo "ram-regress: tracked or untracked changes make this build non-reproducible:" >&2
	printf '%s\n' "$DIRTY" >&2
	echo "ram-regress: commit them, or set ALLOW_DIRTY=1 for an explicitly labelled experiment" >&2
	exit 1
fi

HEAD=$(git rev-parse --short=8 HEAD)
DIRTY_MARK=
[ -z "$DIRTY" ] || DIRTY_MARK=-dirty
echo "ram-regress: source=$HEAD$DIRTY_MARK kernel=$(sha256sum hakios-kernel | cut -c1-12) scenario=$(sha256sum "$SCENARIO" | cut -c1-12)"
echo "ram-regress: args=$ARGS"

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
XL_SECS=${XL_SECS:-190} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENARIO" \
XL_CMDLINE="$ARGS" \
	./xl run
