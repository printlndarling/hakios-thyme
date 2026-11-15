#!/bin/bash
# One HakiOS session with the stock Weather app in front, for the question the
# user raised: the place name shows but no forecast values.  Screenshots tell
# whether the app is waiting on its location consent, on data, or on drawing;
# the run's log carries its XPC lookups and TCP connections.
#
#   devices/thyme/weather.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PATH="$PATH:$HOME/hakios-tools:/usr/local/bin:/usr/bin:/bin"
export XL_ADB_SERIAL=${XL_ADB_SERIAL-5290780a}
export XL_FIRMWARE=${XL_FIRMWARE:-silicium}
export XL_BOOT_IMAGE=${XL_BOOT_IMAGE:-devices/thyme/prebuilt/xnu-usb.img}
export XL_LOADER=${XL_LOADER:-xnuloader/loader-thyme.efi}

SYSTEM_IMAGE=${SYSTEM_IMAGE:-userland/system.apfs}
SCENE=${SCENE:-out/thyme-boot/root-weather.apfs}
SECS=${SECS:-200}
STEP=${STEP:-20}
ARGS=$(tr -d '\n' < devices/thyme/ram.args)
# one word more for the kernel, e.g. personas=1 (the container manager's switch)
EXTRA_ARGS=${EXTRA_ARGS-}
if [ -n "$EXTRA_ARGS" ]; then
	ARGS="$ARGS $EXTRA_ARGS"
fi
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone

[ -f "$SYSTEM_IMAGE" ] || { echo "weather: no system image: $SYSTEM_IMAGE" >&2; exit 1; }
[ -f "$SCENE" ] || { echo "weather: no scenario image: $SCENE" >&2; exit 1; }

if [ "$(./xl state)" = loader ] && [ -f "$PHONE.ran" ]; then
	echo "weather: this loader session has already run a kernel; cold reboot first"
	./xl reboot
	for _ in $(seq 1 90); do
		[ "$(./xl state)" = android ] && break
		sleep 2
	done
fi
./xl enter
./xl ramdisk "$SYSTEM_IMAGE"

(
	elapsed=0
	while [ "$elapsed" -lt $((SECS - 20)) ]; do
		sleep "$STEP"
		elapsed=$((elapsed + STEP))
		./xl screenshot "logs/weather-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/weather-$(date +%H%M%S).log"
XL_FOLLOW= \
XL_SECS="$SECS" \
XL_REBOOT_AFTER=${XL_REBOOT_AFTER:-1} \
XL_RAMDISK="$SCENE" \
XL_CMDLINE="$ARGS" \
	./xl run 2>&1 | tee "$LOG"
kill "$CAPTURE_PID" 2>/dev/null || true
trap - EXIT HUP INT TERM
echo "weather: run's own output in $LOG"

RUNLOG=$(sed -n 's/.*\[\(logs\/[0-9]*-run\.log\)\].*/\1/p' "$LOG" | tail -1)
echo "weather: run log: ${RUNLOG:-none}"
if [ -n "$RUNLOG" ] && [ -f "$RUNLOG" ]; then
	echo "weather: --- Weather's failed XPC lookups"
	grep -a 'requestor = Weather' "$RUNLOG" | sed 's/.*failed lookup: name = //' | sort | uniq -c | sort -rn | head -12 || true
	echo "weather: --- Weather's TCP connections"
	grep -a 'process: Weather' "$RUNLOG" | tail -12 || true
	echo "weather: --- weatherd"
	grep -a -i 'weatherd\|WeatherDaemon' "$RUNLOG" | head -8 || true
fi
