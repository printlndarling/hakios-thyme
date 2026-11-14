#!/bin/bash
# The focused black-screen reproduction (see rc.jobs.blackflash): Sileo open, Settings over it,
# home, Settings again; a screenshot every few seconds. touch_lat is dropped so the whole run
# fits the loader's log ring, and the window server's audit says in the log when the display's
# own surface (or a drawn texture's surface) adds up to nothing.
#
#   devices/thyme/blackflash.sh
#   BLACKFLASH_SECS=360 BLACKFLASH_STEP=3 devices/thyme/blackflash.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-out/thyme-boot/system-jb.apfs}
SCENE=${SCENE:-out/thyme-boot/root-blackflash.apfs}
BLACKFLASH_SECS=${BLACKFLASH_SECS:-300}
BLACKFLASH_STEP=${BLACKFLASH_STEP:-3}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS=${ARGS//battery_stop=25/battery_stop=10}
# The frame-by-frame touch timings are most of the log's bytes; this run wants the audit lines
# from beginning to end more than it wants those timings.
ARGS=${ARGS//touch_lat=1/touch_lat=0}
EXTRA_ARGS=${EXTRA_ARGS-}
if [ -n "$EXTRA_ARGS" ]; then
	ARGS="$ARGS $EXTRA_ARGS"
fi

[ -f "$SYSTEM_IMAGE" ] || { echo "blackflash: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "blackflash: no scenario image: $SCENE" >&2; exit 1; }
echo "blackflash: ${BLACKFLASH_SECS} s, a screenshot every ${BLACKFLASH_STEP} s"
echo "blackflash: our kernel arguments are ${#ARGS} characters (the loader gives 607)"

STATE=$(./xl state)
if [ "$STATE" != loader ] && [ "$STATE" != fastboot ]; then
	echo "blackflash: entering the loader and uploading the system image (one time)"
	./xl enter
	./xl ramdisk "$SYSTEM_IMAGE"
fi

(
	elapsed=0
	while [ "$elapsed" -lt $((BLACKFLASH_SECS - 90)) ]; do
		sleep "$BLACKFLASH_STEP"
		elapsed=$((elapsed + BLACKFLASH_STEP))
		./xl screenshot "logs/blackflash-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/blackflash-$(date +%H%M%S).log"
# A screenshot and the log share the loader's staged buffer; with a log follow running the
# two race and a follow can fetch the framebuffer into the log (see CHECKLIST, 2026-11-14).
XL_FOLLOW= \
XL_SECS="$BLACKFLASH_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "blackflash: run's own output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "blackflash: what the run showed ($RUNLOG):"
	echo "blackflash: --- the display or a drawn surface all zero"
	found=$(grep -a 'THE DISPLAY IS BLACK\|AUDIT\|all ZERO' "$RUNLOG" | tail -20 || true)
	echo "${found:-blackflash:     (none)}"
	echo "blackflash: --- the table"
	found=$(grep -a 'there is room for\|images live' "$RUNLOG" | tail -4 || true)
	echo "${found:-blackflash:     (nothing)}"
	echo "blackflash: --- the window server's fault"
	found=$(grep -a 'signal 11' "$RUNLOG" | tail -2 || true)
	echo "${found:-blackflash:     (none)}"
fi
