#!/bin/bash
# One clean sensor DIAG probe, carrying the two rules the failed attempts taught
# (devices/thyme/docs/sensors-status.md, "Running the DIAG probe on the phone"):
#
#   - a fresh loader session. A second kernel in one session leaves the sensor
#     DSP down ("THE SENSOR DSP DID NOT COME UP: step 2 failed", logs/101037-run.log),
#     so a loader or fastboot state is cold-reset to Android first;
#   - a command line short enough that diag_mask=1 survives. The loader cuts its
#     607 characters silently in the kernel (the tag is at the end of the long
#     daily set and was lost with 99 others), so the probe replaces the daily
#     arguments with the four it needs. slpi=1 is what starts the DSP; sns=1
#     alone logs nothing from the driver.
#
#   devices/thyme/diag-probe.sh
#   PROBE_SECS=300 devices/thyme/diag-probe.sh
#
# Then it reads the run's log back: the channel's open-ack, every ask the
# driver sent, and any packet the DSP's diag router answered with.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

# The sensor DSP firmware is 7.9 MB and the loader's payload area is 64 MiB:
# the daily scene with it makes a 71.9 MB payload, whose stage fails and can
# leave the USB endpoint unresponsive (docs/slpi-fastrpc.md). The probe boots
# the test scene instead, built without the audio firmware by that document's
# recipe: REST=1 BLUETOOTH=1 GL=1 SLPI=1, copied to root-slpitest.apfs.
export SCENE=${SCENE:-out/thyme-boot/root-slpitest.apfs}
[ -f "$SCENE" ] || {
	echo "diag-probe: no $SCENE; build it: cd userland && REST=1 BLUETOOTH=1 GL=1 SLPI=1 RC_JOBS=rc.jobs.wirelessfull bash build.sh, then cp root.apfs ../out/thyme-boot/root-slpitest.apfs" >&2
	exit 1
}

STATE=$(./xl state)
if [ "$STATE" = loader ] || [ "$STATE" = fastboot ]; then
	echo "diag-probe: a kernel session is still open ($STATE); cold-resetting to Android"
	./xl reboot
	sleep 25
	STATE=$(./xl state)
fi
if [ "$STATE" != android ]; then
	echo "diag-probe: the phone is not in Android ($STATE); power-cycle it and run this again" >&2
	exit 1
fi

# **The full daily set, never a trimmed one** (the owner's rule, 13 November: a probe with only
# the sensor arguments booted a phone with no touch, a wrong charge and no GPU and read as a
# broken build). ram.args as it is, plus the three the probe needs; the length is checked against
# the loader's 607 characters, and if it ever stops fitting, the probe's own three go, not the
# daily ones.
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
ARGS="$ARGS slpi=1 sns=1 diag_mask=1"
# xl adds about 310 characters of its own (rd=, the kernel version, the hashes, the epoch).
if [ $(( ${#ARGS} + 310 )) -gt 607 ]; then
	echo "diag-probe: the full set with the probe's three is $(( ${#ARGS} + 310 )) characters, over the loader's 607" >&2
	echo "diag-probe: drop probe arguments, not the daily ones" >&2
	exit 1
fi
BASE_ARGS="$ARGS" \
XL_SECS=${PROBE_SECS:-180} \
	devices/thyme/slpi-run.sh

LOG=$(ls -t logs/*-run.log | head -1)
echo "diag-probe: what the run showed ($LOG):"
found=$(grep -n 'open-ack 5\|slpi: diag:' "$LOG" | head -12 || true)
echo "${found:-diag-probe:     (no DIAG line at all: was slpi=1 in the command line?)}"
found=$(grep -n 'slpi: diag: [0-9][0-9]* bytes' "$LOG" | head -6 || true)
echo "diag-probe: replies from the router: ${found:-(none)}"
found=$(grep -n 'slpi: diag:.*asks.*replies' "$LOG" | tail -1 || true)
echo "diag-probe: teardown summary: ${found:-(none: no teardown summary in the log)}"
