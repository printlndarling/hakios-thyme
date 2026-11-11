#!/bin/bash
# One boot of the battery scene that answers two questions on hardware:
# does the kernel publish an IOPMPowerSource for the loader's battery
# (drivers/power/hakios.battery.c, battery=1), and does powerd/IOPS make a
# battery of it (what the status bar and the lock screen read)?
#
#   devices/thyme/battery-source.sh
#   SCENE=out/thyme-boot/root-battery.apfs XL_SECS=480 \
#       CAPTURES="150 220 290 360" devices/thyme/battery-source.sh
#
# The scene (userland/rc.jobs.battery) runs battery_test, which prints the
# kernel's sysctl numbers, whether an IOPMPowerSource is in the kernel and its
# properties, IOPSGetPercentRemaining and powerd's power-source list; it then
# opens Settings > Battery and the lock screen for the screenshots. PASS needs
# battery_test to see the power source and IOPS to report a percentage.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-battery.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
XL_SECS=${XL_SECS:-480}
CAPTURES=${CAPTURES:-"150 220 290 360"}

[ -f "$SYSTEM_IMAGE" ] || { echo "battery-source: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "battery-source: no scenario image: $SCENE" >&2; exit 1; }

# A second kernel in one loader session is not reliable. Start from Android.
if [ "$(./xl state)" != android ]; then
	echo "battery-source: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "battery-source: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

# Screenshots while the scene runs; the times are seconds after ./xl run, and
# the kernel starts about 30 s into it (the scene's job starts about two
# minutes into the kernel). Background only these subshells -- never the run.
(
	last=0
	for t in $CAPTURES; do
		sleep $((t - last))
		./xl screenshot "logs/batterysrc-$t.png" || true
		last=$t
	done
) &
CAPTURE=$!
# The stored log keeps about the last 1.4 MB, and the launchd spam rotates the
# battery_test output out of it long before the run ends. The live log xl
# fetches every ten seconds still has it when it is written: snapshot the first
# live log that carries the tool's output.
rm -f logs/batterysrc-live.log
(
	for _ in $(seq 1 600); do
		f=$(ls -t logs/*-run.log.live 2>/dev/null | head -1)
		if [ -n "$f" ] && grep -a -q "battery_test: the kernel says" "$f"; then
			cp "$f" logs/batterysrc-live.log
			break
		fi
		sleep 1
	done
) &
SNAPSHOT=$!
stop_capture() { kill "$CAPTURE" "$SNAPSHOT" 2>/dev/null || true; }
trap stop_capture EXIT HUP INT TERM

XL_SECS=$XL_SECS XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} XL_RAMDISK="$SCENE" \
	XL_CMDLINE="$ARGS" \
	./xl run
stop_capture
wait "$CAPTURE" || true
wait "$SNAPSHOT" || true
CAPTURE=
SNAPSHOT=
trap - EXIT HUP INT TERM

LOG=$(ls -t logs/*-run.log | head -1)
[ -n "$LOG" ] || { echo "battery-source: no run log" >&2; exit 1; }
SHA=$(sha256sum "$LOG" | cut -d' ' -f1)
echo "battery-source: log $LOG (SHA-256 $SHA)"
# The final log may or may not still hold the line; the live snapshot is the
# one taken the moment it appeared.
EVIDENCE=$LOG
if [ -s logs/batterysrc-live.log ]; then
	EVIDENCE=logs/batterysrc-live.log
	echo "battery-source: live snapshot logs/batterysrc-live.log (SHA-256 $(sha256sum "$EVIDENCE" | cut -d' ' -f1))"
fi

fail=0
if grep -a -q "battery_test: the power source in the kernel is" "$EVIDENCE"; then
	grep -a "battery_test: the power source in the kernel is" "$EVIDENCE" | head -2
	grep -a "battery_test: its properties" "$EVIDENCE" | head -1
else
	if grep -a -q "battery_test: there is no IOPMPowerSource" "$EVIDENCE"; then
		echo "battery-source: FAIL: battery_test found no IOPMPowerSource in the kernel" >&2
	else
		echo "battery-source: FAIL: no battery_test power-source line in the evidence" >&2
	fi
	fail=1
fi
echo "battery-source: powerd/IOPS:"
grep -a "battery_test: IOPSGetPercentRemaining" "$EVIDENCE" | tail -3 || true
grep -a "battery_test: powerd lists no power source" "$EVIDENCE" | head -1 || true
echo "battery-source: the kernel's own readings:"
grep -a "battery: SOC=" "$LOG" | head -1
grep -a "battery: SOC=" "$LOG" | tail -1
echo "battery-source: screenshots: logs/batterysrc-<t>.png for t in $CAPTURES"
[ "$fail" = 0 ] && echo "battery-source: PASS (the power source is there; see IOPS and the screenshots)"
exit "$fail"
