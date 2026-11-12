#!/bin/bash
# One RAM boot that runs the PCIe2 BHI probe and keeps its whole evidence
# trail: the image the PBL is handed, the doorbell's answer, the root
# complex's AER view and what the SMMU recorded.
#
#   devices/thyme/modem-probe.sh
#   BHI=2 EXTRA_ARGS="pcie2_sid=5" devices/thyme/modem-probe.sh
#
# The scene must carry the modem's boot image at /redwood/fw/modem/sbl1.mbn:
# userland/build.sh stages ../fw-modem-sdx55m/sbl1.mbn into every root it
# builds when that directory is present (the firmware is the vendor's and is
# not in the repository). The probe scene is built once with
#
#   SCENE=modemprobe RC_JOBS=rc.jobs.wirelessfull devices/thyme/build-root.sh
#
# PASS means the probe produced its evidence, not that the modem booted: the
# PBL's answer (result 3 / 0xef120700 so far) is the finding, and the script
# prints it beside the AER and SMMU lines that explain it. A probe never
# starts with pcie2_bhi unset, so a normal boot touches none of this.
#
# The console log keeps a 256 KB head and the newest 1 MB of a longer run
# (osfmk/arm64/loader_return.c): the probe's lines are at the head, a few
# seconds in, so XL_SECS=90 is plenty and a much longer run would only risk
# the middle.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-devices/thyme/prebuilt/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-modemprobe.apfs}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
BHI=${BHI:-1}
IMG=${IMG:-1}
EXTRA_ARGS=${EXTRA_ARGS:-}
XL_SECS=${XL_SECS:-90}

[ -f "$SYSTEM_IMAGE" ] || { echo "modem-probe: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "modem-probe: no scenario image: $SCENE" >&2; exit 1; }
[ -f fw-modem-sdx55m/sbl1.mbn ] ||
	echo "modem-probe: note: no fw-modem-sdx55m/sbl1.mbn here; the scene had better carry one"

# A second kernel in one loader session is not reliable. Start from Android.
if [ "$(./xl state)" != android ]; then
	echo "modem-probe: returning the phone to Android for a fresh loader session"
	./xl reboot >/dev/null || true
	for _ in $(seq 1 150); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
	[ "$(./xl state)" = android ] || { echo "modem-probe: Android did not come back" >&2; exit 1; }
fi

./xl enter
./xl ramdisk "$SYSTEM_IMAGE"
echo "modem-probe: pcie2_bhi=$BHI pcie2_img=$IMG ${EXTRA_ARGS:-}"
XL_SECS=$XL_SECS XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} XL_RAMDISK="$SCENE" \
	XL_CMDLINE="$ARGS pcie2_test=1 pcie2_bhi=$BHI pcie2_img=$IMG ${EXTRA_ARGS}" \
	./xl run

LOG=$(ls -t logs/*-run.log | head -1)
[ -n "$LOG" ] || { echo "modem-probe: no run log" >&2; exit 1; }
echo "modem-probe: log $LOG (SHA-256 $(sha256sum "$LOG" | cut -d' ' -f1))"
grep -a 'pcie2: \(BHI: sbl1\|BHI: status\|AER:\|PARF:\|SMMU: SGFSR\)' "$LOG" || true

fail=0
for want in "pcie2: BHI: sbl1.mbn" "pcie2: BHI: status" "pcie2: AER: uncorr"; do
	if grep -a -q "$want" "$LOG"; then
		echo "modem-probe: present: $want"
	else
		echo "modem-probe: FAIL: missing: $want (the probe did not reach it)" >&2
		fail=1
	fi
done
[ "$fail" = 0 ] && echo "modem-probe: PASS (the evidence is complete; the doorbell's answer is above)"
exit "$fail"
