#!/bin/bash
# Run one SLPI/sensor diagnostic boot, keeping the big system image resident in
# the phone's RAM between runs. The first run uploads it (about 95 s); a run
# after that only stages the kernel and the small root image.
#
#   devices/thyme/slpi-run.sh
#   RAM_EXTRA_ARGS='slpi_step=10 slpi_keep=1 sns=1' XL_SECS=40 devices/thyme/slpi-run.sh
#   devices/thyme/slpi-run.sh --reboot      afterwards, send the phone to Android
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
[ -z "${RAM_EXTRA_ARGS:-}" ] || ARGS="$ARGS $RAM_EXTRA_ARGS"

[ -f "$SYSTEM_IMAGE" ] || { echo "slpi-run: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "slpi-run: no scenario image: $SCENE" >&2; exit 1; }

STATE=$(./xl state)
if [ "$STATE" != loader ] && [ "$STATE" != fastboot ]; then
	echo "slpi-run: entering the loader and uploading the system image (one time)"
	./xl enter
	./xl ramdisk "$SYSTEM_IMAGE"
fi

XL_SECS=${XL_SECS:-40} \
XL_REBOOT_AFTER=0 \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run

if [ "${1:-}" = "--reboot" ]; then
	./xl reboot
fi
