#!/bin/bash
# The black-box reproduction (see rc.jobs.blackbox): Settings opened and flung to the
# bottom, three times, then the keyboard; a screenshot every few seconds through it.
# The window server's surface audit (boot arg glaudit=N frames, default 60) says in the
# run's log whether a surface it drew with added up to nothing.
#
#   devices/thyme/blackbox.sh
#   BLACKBOX_SECS=420 BLACKBOX_STEP=4 devices/thyme/blackbox.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-out/thyme-boot/system-jb.apfs}
SCENE=${SCENE:-out/thyme-boot/root-blackbox.apfs}
BLACKBOX_SECS=${BLACKBOX_SECS:-300}
BLACKBOX_STEP=${BLACKBOX_STEP:-6}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS=${ARGS//battery_stop=25/battery_stop=10}
case "$ARGS" in
*"battery_stop=10"*) ;;
*) echo "blackbox: warning: battery_stop=25 is not in ram.args" >&2 ;;
esac
EXTRA_ARGS=${EXTRA_ARGS-}
if [ -n "$EXTRA_ARGS" ]; then
	ARGS="$ARGS $EXTRA_ARGS"
fi

[ -f "$SYSTEM_IMAGE" ] || { echo "blackbox: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "blackbox: no scenario image: $SCENE" >&2; exit 1; }
echo "blackbox: ${BLACKBOX_SECS} s, a screenshot every ${BLACKBOX_STEP} s"
echo "blackbox: our kernel arguments are ${#ARGS} characters (the loader gives 607)"

STATE=$(./xl state)
if [ "$STATE" != loader ]; then
	echo "blackbox: entering the loader and uploading the system image (one time)"
	./xl enter
	./xl ramdisk "$SYSTEM_IMAGE"
fi

(
	elapsed=0
	while [ "$elapsed" -lt $((BLACKBOX_SECS - 90)) ]; do
		sleep "$BLACKBOX_STEP"
		elapsed=$((elapsed + BLACKBOX_STEP))
		./xl screenshot "logs/blackbox-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/blackbox-$(date +%H%M%S).log"
# A screenshot and the log share the loader's staged buffer; with a log follow running the
# two race and a follow can fetch the framebuffer into the log (see CHECKLIST, 2026-11-14).
XL_FOLLOW= \
XL_SECS="$BLACKBOX_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "blackbox: run's own output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "blackbox: what the run showed ($RUNLOG):"
	echo "blackbox: --- surfaces the audit found all zero"
	found=$(grep -a 'AUDIT\|all ZERO' "$RUNLOG" | tail -20 || true)
	echo "${found:-blackbox:     (none)}"
	echo "blackbox: --- the window server's fault"
	found=$(grep -a 'signal 11' "$RUNLOG" | tail -2 || true)
	echo "${found:-blackbox:     (none)}"
fi
