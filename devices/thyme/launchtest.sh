#!/bin/bash
# One cold launch, timed by screenshots (see rc.jobs.launchtest): the screen is captured every
# two seconds from just after boot, so the black (or snapshot) before the first real frame is
# counted. No log follow: a follow and a screenshot share the loader's staged buffer.
#
#   devices/thyme/launchtest.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-out/thyme-boot/system-jb.apfs}
SCENE=${SCENE:-out/thyme-boot/root-launchtest.apfs}
LAUNCH_SECS=${LAUNCH_SECS:-200}
LAUNCH_STEP=${LAUNCH_STEP:-2}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS=${ARGS//battery_stop=25/battery_stop=10}
ARGS=${ARGS//touch_lat=1/touch_lat=0}
EXTRA_ARGS=${EXTRA_ARGS-}
if [ -n "$EXTRA_ARGS" ]; then
	ARGS="$ARGS $EXTRA_ARGS"
fi

[ -f "$SYSTEM_IMAGE" ] || { echo "launchtest: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "launchtest: no scenario image: $SCENE" >&2; exit 1; }
echo "launchtest: ${LAUNCH_SECS} s, a screenshot every ${LAUNCH_STEP} s"
echo "launchtest: our kernel arguments are ${#ARGS} characters (the loader gives 607)"

# A loader that has just been booted has no system image staged, and there is no way to ask
# it: entering is idempotent (it only boots the loader image when not already there) and the
# upload costs about two minutes, so both are done for every run.
./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	elapsed=0
	while [ "$elapsed" -lt $((LAUNCH_SECS - 90)) ]; do
		sleep "$LAUNCH_STEP"
		elapsed=$((elapsed + LAUNCH_STEP))
		./xl screenshot "logs/launch-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/launch-$(date +%H%M%S).log"
XL_FOLLOW= \
XL_SECS="$LAUNCH_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "launchtest: run's own output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "launchtest: what the run showed ($RUNLOG):"
	echo "launchtest: --- the display or a drawn surface all zero"
	found=$(grep -a 'THE DISPLAY IS BLACK\|AUDIT\|all ZERO' "$RUNLOG" | tail -12 || true)
	echo "${found:-launchtest:     (none)}"
	echo "launchtest: --- the app's own launch"
	found=$(grep -a 'com.apple.Preferences' "$RUNLOG" | tail -6 || true)
	echo "${found:-launchtest:     (nothing)}"
fi
