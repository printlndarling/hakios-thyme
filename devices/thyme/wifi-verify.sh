#!/bin/bash
# The two Wi-Fi recovery tests, one after the other, and what each log says about them:
#
#   devices/thyme/wifi-verify.sh
#
# 1. the power cycle (rc.jobs.wifipower): join, the address and a name, the radio's power off
#    and on, then the address and a name again with no joinfile between them -- the station
#    layer's own re-join of the standing network (kernel dd3f6368 or later);
# 2. the drop (rc.jobs.wifidrop): join, net_test leave, and the keepalive's link event and join.
#
# Both scenes start with sleep_hold, because a sleeping system suspends every scene process and
# a scene cannot wake it (CHECKLIST, 2026-11-15). The phone must be in Android, and charged:
# the kernel is told battery_stop=10 here; the runs themselves need about 15 % between them.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH=${PATH}:/tmp/hakios-phone-tools:/usr/local/bin:/usr/bin:/bin
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

POWER_SCENE=${POWER_SCENE:-out/thyme-boot/root-wifipower.apfs}
DROP_SCENE=${DROP_SCENE:-out/thyme-boot/root-wifidrop.apfs}
for scene in "$POWER_SCENE" "$DROP_SCENE"; do
	[ -f "$scene" ] || {
		echo "wifi-verify: no $scene; build it: SCENE=wifipower devices/thyme/build-root.sh (and wifidrop)" >&2
		exit 1
	}
done

STATE=$(./xl state)
if [ "$STATE" != android ]; then
	echo "wifi-verify: the phone is $STATE, not in Android; connect it and run this again" >&2
	exit 1
fi

ARGS=$(tr -d '\n' < devices/thyme/ram.args | sed 's/battery_stop=25/battery_stop=10/')
case "$ARGS" in
*"battery_stop=10"*) ;;
*) echo "wifi-verify: warning: battery_stop=25 is not in ram.args; the guard is whatever it names" >&2 ;;
esac

run() { # run <scene> <seconds> <what it answers>
	echo "wifi-verify: $3"
	BASE_ARGS="$ARGS" SCENE=$1 XL_SECS=$2 devices/thyme/slpi-run.sh --reboot | tail -3
	LOG=$(ls -t logs/*-run.log | head -1)
	echo "wifi-verify: what $LOG says:"
	grep -n 'keepalive\|net_test: power\|net_test: leave\|joinfile: final\|waitaddr en0:\|resolve apt\|net: en0' "$LOG" | tail -40 || true
	# the next run needs the phone back in Android; xl's own waits cover the loader, not this
	for _ in $(seq 1 60); do
		[ "$(./xl state)" = android ] && return 0
		sleep 2
	done
	echo "wifi-verify: the phone did not come back to Android in two minutes" >&2
	return 1
}

run "$POWER_SCENE" 300 "the power cycle: the station layer re-joins the standing network"
run "$DROP_SCENE" 300 "the drop: the keepalive reads the link event and joins again"
echo "wifi-verify: both runs are in logs/; the scene screenshots also carry them (logs/sileo-*)"
