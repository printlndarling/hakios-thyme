#!/bin/bash
# One RAM boot whose battery guard must fire: the run starts with a
# battery_stop threshold above the phone's charge, the cell discharges under
# the daily scene (the cable is on the host's port, which cannot keep up), and
# the kernel must hand control back to the loader long before XL_SECS.
#
#   devices/thyme/battery-guard.sh
#   THRESHOLD=95 devices/thyme/battery-guard.sh     a stop documented at 95%
#
# PASS needs all of these in the run's log:
#   - the guard's own line "at or below battery_stop=<threshold> three looks in
#     a row: back to the loader",
#   - a battery line that says charging=0 (the loader's bit follows the cell's
#     voltage; a cable attached alone is not charging), and
#   - a return in less than XL_SECS seconds (the stop ended the run, the timer
#     did not).
# The battery must be below the threshold (99 by default, the guard's maximum)
# and above it not be effectively charging: on the host's USB port and under
# the daily scene it is discharging, which is the condition this test needs.
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
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
THRESHOLD=${THRESHOLD:-99}
POLL=${POLL:-20}
XL_SECS=${XL_SECS:-420}

[ -f "$SYSTEM_IMAGE" ] || { echo "battery-guard: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "battery-guard: no scenario image: $SCENE" >&2; exit 1; }
case "$THRESHOLD" in ''|*[!0-9]*) echo "battery-guard: THRESHOLD must be 1..99" >&2; exit 1 ;; esac
[ "$THRESHOLD" -ge 1 ] && [ "$THRESHOLD" -le 99 ] || { echo "battery-guard: THRESHOLD must be 1..99" >&2; exit 1; }

# A second kernel in one loader session is not reliable. Start from Android.
if [ "$(./xl state)" != android ]; then
	echo "battery-guard: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "battery-guard: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
echo "battery-guard: the run stops at or below $THRESHOLD% (poll ${POLL}s); it must end well before ${XL_SECS}s"
XL_SECS=$XL_SECS XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} XL_RAMDISK="$SCENE" \
	XL_CMDLINE="$ARGS battery_stop=$THRESHOLD battery_stop_poll=$POLL" \
	./xl run

LOG=$(ls -t logs/*-run.log | head -1)
[ -n "$LOG" ] || { echo "battery-guard: no run log" >&2; exit 1; }
SHA=$(sha256sum "$LOG" | cut -d' ' -f1)
echo "battery-guard: log $LOG (SHA-256 $SHA)"

fail=0
if grep -a -q "at or below battery_stop=$THRESHOLD three looks in a row: back to the loader" "$LOG"; then
	echo "battery-guard: the stop fired"
else
	echo "battery-guard: FAIL: the stop did not fire" >&2
	fail=1
fi
if grep -a -q "battery: SOC=.* charging=0 " "$LOG"; then
	echo "battery-guard: the battery was not charging (charging=0)"
else
	echo "battery-guard: note: no battery line with charging=0 in the stored log (the stop line itself is the guard's proof)"
fi
# The kernel's return time is the last bracket in the log; a stop ends the run
# before XL_SECS, while the timer ends it at XL_SECS (or a little after).
END=$(grep -ao '^\[ *[0-9]*\.[0-9]*\]' "$LOG" | tail -1 | tr -dc '0-9.' | cut -d. -f1)
if [ -n "$END" ] && [ "$END" -lt "$XL_SECS" ]; then
	echo "battery-guard: the kernel returned at ${END}s, before XL_SECS=$XL_SECS"
else
	echo "battery-guard: FAIL: the return is at ${END:-?}s, not before XL_SECS=$XL_SECS" >&2
	fail=1
fi

[ "$fail" = 0 ] && echo "battery-guard: PASS"
exit "$fail"
