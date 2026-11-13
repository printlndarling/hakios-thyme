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

BASE_ARGS='debug=0x8 slpi=1 sns=1 diag_mask=1' \
XL_SECS=${PROBE_SECS:-180} \
	devices/thyme/slpi-run.sh

LOG=$(ls -t logs/*-run.log | head -1)
echo "diag-probe: what the run showed ($LOG):"
found=$(grep -n 'open-ack 5\|slpi: diag:' "$LOG" | head -12 || true)
echo "${found:-diag-probe:     (no DIAG line at all: was slpi=1 in the command line?)}"
found=$(grep -n 'slpi: diag: [0-9][0-9]* bytes' "$LOG" | head -6 || true)
echo "diag-probe: replies from the router: ${found:-(none)}"
