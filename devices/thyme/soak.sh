#!/bin/bash
# One long daily-scene soak: the canonical ram.args, the wirelessfull scene, a
# screenshot every SOAK_STEP seconds (default 300) and the loader's return at
# SOAK_SECS (default 1800, the 30-minute soak the checklist asks for).
#
#   devices/thyme/soak.sh
#   SOAK_SECS=1200 SOAK_STEP=240 devices/thyme/soak.sh
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
SOAK_SECS=${SOAK_SECS:-1800}
SOAK_STEP=${SOAK_STEP:-300}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

[ -f "$SYSTEM_IMAGE" ] || { echo "soak: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "soak: no scenario image: $SCENE" >&2; exit 1; }
echo "soak: ${SOAK_SECS} s of the daily scene, a screenshot every ${SOAK_STEP} s"
echo "soak: our kernel arguments are ${#ARGS} characters"

STATE=$(./xl state)
if [ "$STATE" != loader ] && [ "$STATE" != fastboot ]; then
	echo "soak: entering the loader and uploading the system image (one time)"
	./xl enter
	./xl ramdisk "$SYSTEM_IMAGE"
fi

(
	elapsed=0
	while [ "$elapsed" -lt $((SOAK_SECS - 60)) ]; do
		sleep "$SOAK_STEP"
		elapsed=$((elapsed + SOAK_STEP))
		./xl screenshot "logs/soak-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

XL_SECS="$SOAK_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
