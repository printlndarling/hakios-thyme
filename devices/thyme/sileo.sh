#!/bin/bash
# One Sileo session on the phone: the package page, the GET tap that queues it,
# the scene's own apt simulation against Sileo's index, and screenshots through
# the run (rc.jobs.sileo does the work; see userland/rc.jobs.sileo).
#
#   devices/thyme/sileo.sh
#   EXTRA_ARGS="gl=0" devices/thyme/sileo.sh     (CoreAnimation's own software
#       renderer instead of ours: whether the window server's mid-run SIGSEGV
#       is in the GL path is what that comparison answers -- CHECKLIST, 2026-11-13)
#
# The battery guard stops at 25 % by default, which ended a 600 s run after
# 316 s once; the scene needs its whole length, so the stop is moved to 10 %.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-sileo.apfs}
SILEO_SECS=${SILEO_SECS:-600}
STEP=${STEP:-60}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS=${ARGS//battery_stop=25/battery_stop=10}
case "$ARGS" in
*"battery_stop=10"*) ;;
*) echo "sileo: warning: battery_stop=25 is not in ram.args; the guard is whatever it names" >&2 ;;
esac
if [ -n "${EXTRA_ARGS:-}" ]; then
	ARGS="$ARGS $EXTRA_ARGS"
fi

[ -f "$SYSTEM_IMAGE" ] || { echo "sileo: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "sileo: no scenario image: $SCENE" >&2; exit 1; }
echo "sileo: ${SILEO_SECS} s of the Sileo scene, a screenshot every ${STEP} s"
echo "sileo: our kernel arguments are ${#ARGS} characters (the loader gives 607)"

STATE=$(./xl state)
if [ "$STATE" != loader ] && [ "$STATE" != fastboot ]; then
	echo "sileo: entering the loader and uploading the system image (one time)"
	./xl enter
	./xl ramdisk "$SYSTEM_IMAGE"
fi

(
	elapsed=0
	while [ "$elapsed" -lt $((SILEO_SECS - 30)) ]; do
		sleep "$STEP"
		elapsed=$((elapsed + STEP))
		./xl screenshot "logs/sileo-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/sileo-$(date +%H%M%S).log"
XL_SECS="$SILEO_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "sileo: run's own output in $LOG"
