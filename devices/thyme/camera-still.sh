#!/bin/bash
# Camera.app's shutter on the S5KHMX, through the iOS capture stack, and the
# return to Android.  The scene (rc.jobs.camera108) opens Camera.app and taps
# the shutter twice; the log's "the photograph ... averaged / given / encoded"
# lines are the evidence, and the screenshots catch the preview and the shots.
# Build the scene first:
#   SCENE=camera108 RC_JOBS=rc.jobs.camera108 devices/thyme/build-root.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH="$PATH:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin"
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-camera108.apfs}
SECS=${SECS:-560}
SHOT_FROM=${SHOT_FROM:-380}
SHOT_EVERY=${SHOT_EVERY:-20}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
# The runtime arguments may carry a cam= selection; this script makes its own,
# as camera-real.sh does, so the daily args do not leak in twice.
ARGS=$(printf '%s\n' "$ARGS" | sed -E 's/(^| +)(cam|cam_real|cam_keep_mb)=[^ ]*//g')

[ -f "$SYSTEM_IMAGE" ] || { echo "camera-still: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "camera-still: no scene: $SCENE (build it with SCENE=camera108 RC_JOBS=rc.jobs.camera108 devices/thyme/build-root.sh)" >&2; exit 1; }

if [ "$(./xl state)" != android ]; then
	echo "camera-still: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "camera-still: Android did not come back" >&2; exit 1; }
fi

# Let Android finish booting before the loader takes the PMIC over SPMI.
for _ in $(seq 1 90); do
	[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
	sleep 2
done

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	elapsed=0
	while [ "$elapsed" -lt $((SECS - 30)) ]; do
		sleep "$SHOT_EVERY"
		elapsed=$((elapsed + SHOT_EVERY))
		if [ "$elapsed" -ge "$SHOT_FROM" ]; then
			./xl screenshot "logs/camera108-${elapsed}.png" || true
		fi
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/camera108-$(date +%H%M%S).log"
# cam_keep_mb=48: the boot-time piece carries the stream's ring in its first half
# and the host's photograph in the second (24 MB): a 4032x3024 NV12 picture is
# 18.3 MB, so the 24 MB piece's 12 MB picture half answered ENOSPC where the
# photograph goes to xl camera (logs/121949-run.log).
XL_SECS="$SECS" \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS cam=5 cam_real=1 cam_keep_mb=48${EXTRA_ARGS:+ $EXTRA_ARGS}" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
./xl reboot
echo "camera-still: session complete; run output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
echo "camera-still: run log: ${RUNLOG:-none}"
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "camera-still: --- the shutter and the photographs"
	grep -a 'hakios: camera: stream: a photograph asked\|hakios: camera: the photograph\|the photograph of\|hakios: mediaserverd: a photograph' "$RUNLOG" | tail -24 || true
	echo "camera-still: --- the session and its frames"
	grep -a 'camera: the frames for iOS\|UMI camera: iOS session armed\|the frames for iOS stopped' "$RUNLOG" | tail -8 || true
fi
