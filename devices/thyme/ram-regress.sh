#!/bin/sh
# Boot one complete THYME baseline and run the full Wi-Fi + Bluetooth scenario.
#
# This script exists so a focused test cannot silently drop another subsystem from the kernel
# command line. The scenario may change, but every RAM regression run uses devices/thyme/ram.args.
#
#   devices/thyme/ram-regress.sh [scenario.apfs]
#
# The default performs Wi-Fi join/DHCP, Safari, a power cycle, a second join/DHCP, and Bluetooth
# discovery. The local kernel, boot image and scenario are checked against ram-baseline.conf:
# a test of different bytes needs ALLOW_NEW_BASELINE=1, and becomes a regression baseline only
# when that file is updated and committed with the change.
# ALLOW_DIRTY=1 permits a test build from a modified tracked tree; it is recorded in the output.
set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}
SCENARIO=${1:-out/thyme-boot/root-wirelessfull.apfs}
SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
UEFI=${UEFI:-devices/thyme/prebuilt/SILICIUM_UEFI.fd}
BASELINE=$HERE/ram-baseline.conf
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
[ -z "${RAM_EXTRA_ARGS:-}" ] || ARGS="$ARGS $RAM_EXTRA_ARGS"

[ -f "$SCENARIO" ] || { echo "ram-regress: no scenario image: $SCENARIO" >&2; exit 1; }
[ -f "$SYSTEM_IMAGE" ] || { echo "ram-regress: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$XL_BOOT_IMAGE" ] || { echo "ram-regress: no boot image: $XL_BOOT_IMAGE" >&2; exit 1; }
[ -f "$UEFI" ] || { echo "ram-regress: no UEFI image: $UEFI" >&2; exit 1; }
[ -f "$XL_LOADER" ] || { echo "ram-regress: no loader: $XL_LOADER" >&2; exit 1; }
[ -f "$BASELINE" ] || { echo "ram-regress: no baseline manifest: $BASELINE" >&2; exit 1; }
[ -x hakios-kernel ] || { echo "ram-regress: no built kernel; run make kernel" >&2; exit 1; }

DIRTY=$(git status --porcelain --untracked-files=normal)
if [ -n "$DIRTY" ] && [ "${ALLOW_DIRTY:-0}" != 1 ]; then
	echo "ram-regress: tracked or untracked changes make this build non-reproducible:" >&2
	printf '%s\n' "$DIRTY" >&2
	echo "ram-regress: commit them, or set ALLOW_DIRTY=1 for an explicitly labelled experiment" >&2
	exit 1
fi

CANDIDATE=0

baseline_value() {
	sed -n "s/^$1=//p" "$BASELINE"
}

check_hash() { # check_hash <label> <file> <manifest key>
	local actual expected
	actual=$(sha256sum "$2" | cut -d' ' -f1)
	expected=$(baseline_value "$3")
	[ -n "$expected" ] || { echo "ram-regress: baseline has no $3" >&2; exit 1; }
	if [ "$actual" != "$expected" ]; then
		echo "ram-regress: $1 does not match the committed regression baseline:" >&2
		echo "  expected $expected" >&2
		echo "  actual   $actual  ($2)" >&2
		if [ "${ALLOW_NEW_BASELINE:-0}" != 1 ]; then
			echo "ram-regress: rebuild the canonical inputs, or set ALLOW_NEW_BASELINE=1 for a new candidate" >&2
			exit 1
		fi
		CANDIDATE=1
	fi
}

check_hash "kernel" hakios-kernel KERNEL_SHA256
check_hash "boot image" "$XL_BOOT_IMAGE" BOOT_IMAGE_SHA256
check_hash "UEFI" "$UEFI" UEFI_SHA256
check_hash "loader" "$XL_LOADER" LOADER_SHA256
check_hash "system image" "$SYSTEM_IMAGE" SYSTEM_SHA256
check_hash "scenario" "$SCENARIO" SCENARIO_SHA256
check_hash "boot arguments" devices/thyme/ram.args RAM_ARGS_SHA256

actual_args_sha=$(printf '%s' "$ARGS" | sha256sum | cut -d' ' -f1)
expected_args_sha=$(baseline_value BOOT_ARGS_SHA256)
[ -n "$expected_args_sha" ] || { echo "ram-regress: baseline has no BOOT_ARGS_SHA256" >&2; exit 1; }
if [ "$actual_args_sha" != "$expected_args_sha" ]; then
	echo "ram-regress: effective boot arguments do not match the committed baseline:" >&2
	echo "  expected $expected_args_sha" >&2
	echo "  actual   $actual_args_sha" >&2
	echo "  arguments $ARGS" >&2
	if [ "${ALLOW_NEW_BASELINE:-0}" != 1 ]; then
		echo "ram-regress: use the complete ram.args, or set ALLOW_NEW_BASELINE=1 for a new candidate" >&2
		exit 1
	fi
	CANDIDATE=1
fi

HEAD=$(git rev-parse --short=8 HEAD)
DIRTY_MARK=
[ -z "$DIRTY" ] || DIRTY_MARK=-dirty
echo "ram-regress: source=$HEAD$DIRTY_MARK kernel=$(sha256sum hakios-kernel | cut -c1-12) scenario=$(sha256sum "$SCENARIO" | cut -c1-12)"
echo "ram-regress: boot=$(sha256sum "$XL_BOOT_IMAGE" | cut -c1-12) uefi=$(sha256sum "$UEFI" | cut -c1-12) system=$(sha256sum "$SYSTEM_IMAGE" | cut -c1-12)"
echo "ram-regress: args=$ARGS"
echo "ram-regress: args_sha=$actual_args_sha"
if [ "${VERIFY_ONLY:-0}" = 1 ]; then
	if [ "$CANDIDATE" = 1 ]; then
		echo "ram-regress: candidate inputs differ from the committed baseline and were explicitly allowed"
	else
		echo "ram-regress: all inputs match the committed baseline"
	fi
	exit 0
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
(
	sleep "${WIFI_CAPTURE_DELAY:-145}"
	./xl screenshot "${WIFI_SCREENSHOT:-logs/wirelessregress-wifi.png}" || true
	sleep "$(( ${BT_CAPTURE_DELAY:-205} - ${WIFI_CAPTURE_DELAY:-145} ))"
	./xl screenshot "${BT_SCREENSHOT:-logs/wirelessregress-bt.png}" || true
) &
CAPTURE_PID=$!
stop_capture() {
	if [ -n "${CAPTURE_PID:-}" ]; then
		kill "$CAPTURE_PID" 2>/dev/null || true
	fi
}
trap stop_capture EXIT HUP INT TERM

XL_SECS=${XL_SECS:-230} \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENARIO" \
XL_CMDLINE="$ARGS" \
	./xl run
stop_capture
wait "$CAPTURE_PID" || true
CAPTURE_PID=
trap - EXIT HUP INT TERM
