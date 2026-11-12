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
# The daily args bring the camera up (cam=5) so the Camera icon is always there;
# a diagnostic run replaces that with its own cam= selection.
ARGS=$(printf '%s\n' "$ARGS" | sed -E 's/(^| +)(cam|cam_real|cam_keep_mb)=[^ ]*//g')

case "$MODE" in
1|2|3|4|6|7) ;;
*) echo "camera-mode: mode must be one of 1, 2, 3, 4, 6, 7" >&2; exit 2 ;;
esac
SENSOR_MODE=${SENSOR_MODE:-}

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

# Let Android finish booting: its PMIC/regulator drivers must be at rest before
# the loader takes the PM8150L GPIO channels over SPMI (a warm reboot in the
# middle of Android's boot left one transaction failed with status 0x9).
for _ in $(seq 1 90); do
	[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
	sleep 2
done

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
XL_SECS=${XL_SECS:-180} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS cam=$MODE cam_after=0${SENSOR_MODE:+ cam_mode=$SENSOR_MODE}${EXTRA_ARGS:+ $EXTRA_ARGS}" \
	./xl run
case "$MODE" in
6|7) ./xl camera "${PREFIX:-logs/camera}" ;;
esac
./xl reboot
echo "camera-mode: cam=$MODE diagnostic complete"
