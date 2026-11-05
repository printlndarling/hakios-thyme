#!/bin/bash
# Controlled frame-pacing run: home screen edit mode with the GPU userspace
# (GL=1). The log's tl: v ... shown timestamps and the hakios-gl lines are the
# evidence; a run with no hakios-gl lines has fallen back to a slow path.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-jiggle-gl.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "jiggle: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "jiggle: no scenario image: $SCENE" >&2; exit 1; }

if [ "$(./xl state)" != android ]; then
	echo "jiggle: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "jiggle: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	sleep "${JIGGLE_CAPTURE:-95}"
	./xl screenshot "${JIGGLE_SCREENSHOT:-logs/jiggle-gl.png}" || true
) &
CAPTURE_PID=$!
stop_capture() {
	if [ -n "${CAPTURE_PID:-}" ]; then
		kill "$CAPTURE_PID" 2>/dev/null || true
	fi
}
trap stop_capture EXIT HUP INT TERM

XL_SECS=${XL_SECS:-120} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run

stop_capture
wait "$CAPTURE_PID" || true
CAPTURE_PID=
trap - EXIT HUP INT TERM

echo "jiggle: inspect the newest logs/<time>-run.log for tl: windows and hakios-gl lines"
