#!/bin/bash
# One HakiOS session that opens Settings at Privacy & Security three times and
# holds it, to see whether libhakios.prefs.dylib's Location Services row is
# there (rc.jobs.locrow8 does the work; see userland/src/inject_prefs.c).
#
#   devices/thyme/locrow.sh
#
# The scene and the kernel log answer both questions: "hakios-prefs:" lines in
# the run's log name every list controller that loaded or appeared, and whether
# the row was added; the screenshots show the page itself.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH="$PATH:$HOME/hakios-tools:/usr/local/bin:/usr/bin:/bin"
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-userland/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-locrow8.apfs}
SECS=${SECS:-150}
STEP=${STEP:-20}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone

[ -f "$SYSTEM_IMAGE" ] || { echo "locrow: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "locrow: no scenario image: $SCENE" >&2; exit 1; }

# A loader session that has already run a kernel dies on a second one (xl's
# note); the cold reboot also makes the system image below a fresh upload.
if [ "$(./xl state)" = loader ] && [ -f "$PHONE.ran" ]; then
	echo "locrow: this loader session has already run a kernel; cold reboot first"
	./xl reboot
	for _ in $(seq 1 90); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
fi
./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	elapsed=0
	while [ "$elapsed" -lt $((SECS - 30)) ]; do
		sleep "$STEP"
		elapsed=$((elapsed + STEP))
		./xl screenshot "logs/locrow-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/locrow-$(date +%H%M%S).log"
XL_FOLLOW= \
XL_SECS="$SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "locrow: run's own output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
echo "locrow: run log: ${RUNLOG:-none}"
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "locrow: --- injection and the row"
	grep -a -n 'hakios-prefs\|library insertion' "$RUNLOG" | head -60 || echo "locrow:     (nothing)"
	echo "locrow: --- Preferences in launchd's log"
	grep -a -n 'Preferences\[' "$RUNLOG" | tail -12 || echo "locrow:     (nothing)"
fi
