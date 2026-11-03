#!/bin/bash
# Capture the THYME CSID/IFE test pattern through the real DMA path.
# This is a focused camera diagnostic; it does not enable sensor streaming.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-wirelessfull.apfs}
PREFIX=${1:-logs/cam-tpg}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "camera-tpg: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "camera-tpg: no full scene: $SCENE" >&2; exit 1; }

# A second kernel in one loader session is not reliable. Start from Android.
if [ "$(./xl state)" != android ]; then
	echo "camera-tpg: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "camera-tpg: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
XL_SECS=${XL_SECS:-210} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS cam=6 cam_after=0" \
	./xl run

./xl camera "$PREFIX"
./xl reboot
echo "camera-tpg: frames are ${PREFIX}-*.png"
