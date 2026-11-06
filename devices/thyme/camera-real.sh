#!/bin/bash
# Run the iOS camera sessions against the S5KHMX itself (cam=5 cam_real=1) and return to Android.
# Build the scene first: SCENE=camera RC_JOBS=rc.jobs.camera devices/thyme/build-root.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-camera.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "camera-real: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "camera-real: no scene: $SCENE (build it with SCENE=camera RC_JOBS=rc.jobs.camera devices/thyme/build-root.sh)" >&2; exit 1; }

if [ "$(./xl state)" != android ]; then
	echo "camera-real: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "camera-real: Android did not come back" >&2; exit 1; }
fi

# Let Android finish booting before the loader takes the PMIC over SPMI.
for _ in $(seq 1 90); do
	[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
	sleep 2
done

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
XL_SECS=${XL_SECS:-300} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS cam=5 cam_real=1 cam_keep_mb=24${EXTRA_ARGS:+ $EXTRA_ARGS}" \
	./xl run
./xl reboot
echo "camera-real: session complete"
