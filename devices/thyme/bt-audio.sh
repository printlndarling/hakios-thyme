#!/bin/bash
# Bluetooth audio gate: wait for the paired headset, play a tone through the
# system-selected route and ask AVAudioSession for the explicit HFP/A2DP
# options. The log, not a picture, is the evidence; the final screenshot only
# records the Settings state.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-btfull.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "bt-audio: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "bt-audio: no scenario image: $SCENE" >&2; exit 1; }

if [ "$(./xl state)" != android ]; then
	echo "bt-audio: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "bt-audio: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	sleep "${BT_AUDIO_CAPTURE:-240}"
	./xl screenshot "${BT_AUDIO_SCREENSHOT:-logs/btaudio-final.png}" || true
) &
CAPTURE_PID=$!
stop_capture() {
	if [ -n "${CAPTURE_PID:-}" ]; then
		kill "$CAPTURE_PID" 2>/dev/null || true
	fi
}
trap stop_capture EXIT HUP INT TERM

XL_SECS=${XL_SECS:-260} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run

stop_capture
wait "$CAPTURE_PID" || true
CAPTURE_PID=
trap - EXIT HUP INT TERM

echo "bt-audio: the log has the route, frames and AVAudioSession option results"
