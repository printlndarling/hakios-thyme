#!/bin/bash
# Run one THYME camera sensor diagnostic and return to Android.
# The default cam=3 programs the S5KHMX mode table without starting a stream.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

MODE=${CAM_MODE:-3}
SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-wirelessfull.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

case "$MODE" in
1|2|3|4) ;;
*) echo "camera-mode: mode must be one of 1, 2, 3, 4" >&2; exit 2 ;;
esac

[ -f "$SYSTEM_IMAGE" ] || { echo "camera-mode: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "camera-mode: no full scene: $SCENE" >&2; exit 1; }

if [ "$(./xl state)" != android ]; then
	echo "camera-mode: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "camera-mode: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
XL_SECS=${XL_SECS:-180} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS cam=$MODE cam_after=0" \
	./xl run
./xl reboot
echo "camera-mode: cam=$MODE diagnostic complete"
