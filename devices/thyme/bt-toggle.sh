#!/bin/bash
# One focused Bluetooth power-cycle run: open Settings, wait for the pane,
# turn Bluetooth off, then on again, and capture each state.
#
#   devices/thyme/bt-toggle.sh
#
# The scenario is userland/rc.jobs.bttoggle, built into
# out/thyme-boot/root-bttoggle.apfs. It uses the same complete ram.args as the
# wireless regression, so a pass cannot come from a boot with Bluetooth or
# Wi-Fi left out.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-bttoggle.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "bt-toggle: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "bt-toggle: no scenario image: $SCENE" >&2; exit 1; }

# A second kernel in one loader session is not reliable. Start from Android.
if [ "$(./xl state)" != android ]; then
	echo "bt-toggle: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "bt-toggle: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	sleep "${BT_OFF_CAPTURE:-118}"
	./xl screenshot "${BT_OFF_SCREENSHOT:-logs/bttoggle-off.png}" || true
	sleep "$(( ${BT_ON_CAPTURE:-148} - ${BT_OFF_CAPTURE:-118} ))"
	./xl screenshot "${BT_ON_SCREENSHOT:-logs/bttoggle-on.png}" || true
	sleep "$(( ${BT_FINAL_CAPTURE:-200} - ${BT_ON_CAPTURE:-148} ))"
	./xl screenshot "${BT_FINAL_SCREENSHOT:-logs/bttoggle-final.png}" || true
) &
CAPTURE_PID=$!
stop_capture() {
	if [ -n "${CAPTURE_PID:-}" ]; then
		kill "$CAPTURE_PID" 2>/dev/null || true
	fi
}
trap stop_capture EXIT HUP INT TERM

XL_SECS=${XL_SECS:-220} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run

stop_capture
wait "$CAPTURE_PID" || true
CAPTURE_PID=
trap - EXIT HUP INT TERM

echo "bt-toggle: screenshots are logs/bttoggle-off.png, logs/bttoggle-on.png and logs/bttoggle-final.png"
