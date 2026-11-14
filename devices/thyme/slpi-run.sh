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
# BASE_ARGS replaces the daily argument set when a probe needs the room: the
# loader's cmdline buffer is 607 characters and cuts the tail silently, and
# the sensor arguments sit at the end.
ARGS=${BASE_ARGS:-$(tr -d '\n' < devices/thyme/ram.args)}
[ -z "${RAM_EXTRA_ARGS:-}" ] || ARGS="$ARGS $RAM_EXTRA_ARGS"
echo "slpi-run: our kernel arguments are ${#ARGS} characters (about 420 fit before the loader cuts)"

[ -f "$SYSTEM_IMAGE" ] || { echo "slpi-run: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "slpi-run: no scenario image: $SCENE" >&2; exit 1; }

# A loader that has just been booted has no system image staged, and there is no way to ask
# it: entering is idempotent (it only boots the loader image when not already there) and the
# upload costs about two minutes, so both are done for every run.
./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

XL_SECS=${XL_SECS:-40} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-0} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run

if [ "${1:-}" = "--reboot" ]; then
	./xl reboot
fi
