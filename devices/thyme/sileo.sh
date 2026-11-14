#!/bin/bash
# One Sileo session on the phone: the package page, the GET tap that queues it,
# the scene's own apt simulation against Sileo's index, and screenshots through
# the run (rc.jobs.sileo does the work; see userland/rc.jobs.sileo).
#
#   devices/thyme/sileo.sh
#   EXTRA_ARGS="gl=0" devices/thyme/sileo.sh     (CoreAnimation's own software
#       renderer instead of ours: that GL path was where the window server's
#       SIGSEGV at 0x18 came from -- fd6_build_bindless_state on a NULL
#       buffer; CHECKLIST, 2026-11-13 -- so gl=0 is also the way to run the UI
#       without it if a regression is ever suspected there)
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

# The system image with the package manager's prefix as /jb, which init grafts
# onto /var/jb (out/thyme-boot/system-jb.apfs, made by userland/build.sh SYSTEM=1
# with tools/mk-jb.sh's tree in jb/). The prebuilt image has no /jb, and the
# scene then dies at its first line: "/var/jb/bin/sh: No such file or directory".
SYSTEM_IMAGE=${SYSTEM_IMAGE:-out/thyme-boot/system-jb.apfs}
SCENE=${SCENE:-out/thyme-boot/root-sileo.apfs}
# The scene's GET taps come ~610 s after the demo job starts, which is ~740 s
# of kernel time (boot included): the 01:01 run reached them at 737 s of a
# 749 s run, and the 572 s run before it ended before them. 900 leaves room
# for the apt simulation and the final dpkg-query/tree.
SILEO_SECS=${SILEO_SECS:-900}
# every half minute: the confirm taps and the install that follows fall inside
# a few of them, and a run is expensive enough to want the picture
STEP=${STEP:-30}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS=${ARGS//battery_stop=25/battery_stop=10}
case "$ARGS" in
*"battery_stop=10"*) ;;
*) echo "sileo: warning: battery_stop=25 is not in ram.args; the guard is whatever it names" >&2 ;;
esac
# The DSP's diag router is asked for the sensor framework's log in the same run
# (docs/sensors-status.md): one boot answers both the install question and
# whether that channel carries anything. EXTRA_ARGS="" runs without it.
EXTRA_ARGS=${EXTRA_ARGS-diag_mask=1}
if [ -n "$EXTRA_ARGS" ]; then
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
	while [ "$elapsed" -lt $((SILEO_SECS - 90)) ]; do
		sleep "$STEP"
		elapsed=$((elapsed + STEP))
		./xl screenshot "logs/sileo-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/sileo-$(date +%H%M%S).log"
# A screenshot and the log share the loader's staged buffer; with a log follow running the
# two race and a follow can fetch the framebuffer into the log (see CHECKLIST, 2026-11-14).
XL_FOLLOW= \
XL_SECS="$SILEO_SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "sileo: run's own output in $LOG"

# What to look at first, out of the run's own log: the DIAG answer (if any), the
# DRM shim's refusals, what apt/dpkg said about tree, and whether the window
# server faulted. A run that ends before the scene's last lines leaves these empty.
RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "sileo: what the run showed ($RUNLOG):"
	echo "sileo: --- the DSP's diag channel"
	found=$(grep -n 'slpi: diag:' "$RUNLOG" | tail -6 || true)
	echo "${found:-sileo:     (nothing)}"
	echo "sileo: --- refusals the DRM shim named"
	found=$(grep -n 'hakios-drm: method\|hakios-drm: 16 refusals' "$RUNLOG" | tail -6 || true)
	echo "${found:-sileo:     (nothing)}"
	echo "sileo: --- apt and dpkg on tree"
	found=$(grep -n 'Setting up tree\|dpkg-query\|/usr/bin/tree\|E: ' "$RUNLOG" | tail -10 || true)
	echo "${found:-sileo:     (nothing)}"
	echo "sileo: --- the window server's fault"
	found=$(grep -n 'signal 11 at 0x18' "$RUNLOG" | tail -2 || true)
	echo "${found:-sileo:     (none)}"
fi
