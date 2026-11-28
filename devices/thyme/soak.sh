#!/bin/bash
# Run a timed daily scene from RAM. See docs/ram-boot.md for evidence and options.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-wirelessfull.apfs}
SOAK_SECS=${SOAK_SECS:-1800}
SOAK_STEP=${SOAK_STEP:-300}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)

if ! [[ $SOAK_SECS =~ ^[1-9][0-9]{0,5}$ && $SOAK_STEP =~ ^[1-9][0-9]{0,5}$ ]]; then
	echo "soak: SOAK_SECS and SOAK_STEP must be positive integers below 1000000" >&2
	exit 1
fi
for image in "$SYSTEM_IMAGE" "$SCENE" "$XL_BOOT_IMAGE" "$XL_LOADER"; do
	[ -s "$image" ] || { echo "soak: missing or empty image: $image" >&2; exit 1; }
done

# The thyme loader's run-payload window is 0x03f00000 bytes.
export XL_USB_HANDOFF=${SOAK_USB_HANDOFF:-0}
case "$XL_USB_HANDOFF" in
0|1) ;;
*) echo "soak: SOAK_USB_HANDOFF must be 0 or 1" >&2; exit 1 ;;
esac
export XL_MAX_PAYLOAD=66060288 XL_SECS=$SOAK_SECS XL_REBOOT_AFTER=1 XL_DETACH=0 XL_HOME=android XL_RAMDISK="$SCENE"
export XL_CMDLINE="debug=0x8 rd=md0 $ARGS pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=2 pcie2_nr=0"
case "${SOAK_USB_GADGET:-0}" in
0) ;;
1|2) if [ "$XL_USB_HANDOFF" != 1 ]; then
       echo "soak: SOAK_USB_GADGET requires SOAK_USB_HANDOFF=1" >&2
       exit 1
   fi
   XL_CMDLINE="$XL_CMDLINE usb_gadget=$SOAK_USB_GADGET" ;;
*) echo "soak: SOAK_USB_GADGET must be 0, 1 or 2" >&2; exit 1 ;;
esac
case "${SOAK_METAL_APPS:-0}" in
0) ;;
1) python3 devices/thyme/ram-boot.py --check-metal-apps "$SCENE"
   XL_CMDLINE="$XL_CMDLINE metal=1 metal_apps=2" ;;
*) echo "soak: SOAK_METAL_APPS must be 0 or 1" >&2; exit 1 ;;
esac
case "${SOAK_DSDS_PROBE:-0}" in
0) ;;
1) XL_CMDLINE="$XL_CMDLINE pcie2_dsds_probe=1"
   export XL_FOLLOW="${XL_FOLLOW-5}" XL_FOLLOW_KEEP="${XL_FOLLOW_KEEP-1}" ;;
*) echo "soak: SOAK_DSDS_PROBE must be 0 or 1" >&2; exit 1 ;;
esac
case "${SOAK_UIM_READONLY:-0}" in
0) ;;
1) XL_CMDLINE="$XL_CMDLINE pcie2_uiswitch=0 pcie2_no_sms=1" ;;
*) echo "soak: SOAK_UIM_READONLY must be 0 or 1" >&2; exit 1 ;;
esac
python3 devices/thyme/ram-boot.py --check-loader "$XL_BOOT_IMAGE"
./xl check-payload

serial=$(python3 devices/thyme/ram-boot.py --preflight)
if [ -n "${XL_ADB_SERIAL:-}" ] && [ "$XL_ADB_SERIAL" != "$serial" ]; then
	echo "soak: XL_ADB_SERIAL does not match the verified device" >&2
	exit 1
fi
attached=$(timeout -k 2 10 fastboot devices)
if [ -n "$attached" ]; then
	echo "soak: disconnect other fastboot devices before starting" >&2
	exit 1
fi
export XL_ADB_SERIAL=$serial

mkdir -p logs
evidence=$(mktemp -d logs/soak.XXXXXXXX)
echo "soak: ${SOAK_SECS}s timed scene, framebuffer interval ${SOAK_STEP}s, evidence $evidence"
capture_pid=
recover=1
cleanup() {
	if [ -n "$capture_pid" ]; then
		kill "$capture_pid" 2>/dev/null || true
		wait "$capture_pid" 2>/dev/null || true
	fi
	if [ "$recover" = 1 ] && [ "$(timeout -k 2 15 ./xl state)" = loader ]; then
		timeout -k 2 15 ./xl reboot >&2 || true
	fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# Always start from Android and upload the chosen system image for this run.
./xl ramdisk "$SYSTEM_IMAGE" | tee "$evidence/ramdisk.log"

if [ "$XL_USB_HANDOFF" = 0 ]; then
	(
		for ((elapsed=SOAK_STEP; elapsed <= SOAK_SECS - 60; elapsed+=SOAK_STEP)); do
			sleep "$SOAK_STEP" </dev/null >/dev/null 2>&1
			./xl screenshot-live "$evidence/screen-$elapsed.png" || true
		done
	) &
	capture_pid=$!
fi

run_status=0
./xl run | tee "$evidence/host.log" || run_status=$?

# An authorized adb transport appears before Android has finished booting.
for ((attempt=0; attempt<45; attempt++)); do
	booted=$(timeout -k 2 3 adb -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' || true)
	[ "$booted" = 1 ] && break
	# Fix up a failed loader reboot before waiting out the full deadline.
	if [ "$attempt" = 0 ] && [ "$(timeout -k 2 15 ./xl state)" = loader ]; then
		timeout -k 2 15 ./xl reboot || true
	fi
	sleep 2
done
if [ "$booted" != 1 ]; then
	echo "soak: Android boot completion was not observed" >&2
	exit 1
fi
recover=0
echo "soak: Android boot completed"

if [ "$XL_USB_HANDOFF" = 1 ]; then
	if [ "$run_status" != 0 ]; then
		echo "soak: USB handoff run failed" >&2
		exit 1
	fi
	echo "soak: USB handoff reset observed; use the LAN session to capture live diagnostics"
	exit 0
fi

log=$(tr -d '\r' < "$evidence/host.log" |
	sed -nE 's/^result: returned [1-9][0-9]* 0 +\[(logs\/[0-9]{6}-run\.log)\]$/\1/p')
if [ "$run_status" != 0 ] || [ ! -s "$log" ]; then
	echo "soak: no clean loader return with a kernel log" >&2
	exit 1
fi
cp -- "$log" "$evidence/kernel.log"
sha256sum "$evidence/kernel.log"
if ! grep -axq -- '--- loader_return_after expired ---' "$evidence/kernel.log" ||
	grep -aEq 'panic\(cpu|Debugger message: panic|^--- (battguard|thermal): .*back to the loader ---' "$evidence/kernel.log"; then
	echo "soak: the log does not show a clean timed return" >&2
	exit 1
fi
echo "soak: timed return verified for ${SOAK_SECS}s; evidence $evidence"
