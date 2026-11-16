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
# BATTERY_STOP=<percent>: this run's own battery_stop, ahead of ram.args' (the
# kernel's PE_parse_boot_argn takes the first occurrence).  The guard stopped
# three runs at ~100 s because the phone sat at 24-26% and ram.args says 25
# ("the charge is 24%, at or below battery_stop=25 three looks in a row",
# logs/155121-run.log.live), just before weatherd is spawned; a watched
# session can run longer on 10.
if [ -n "${BATTERY_STOP:-}" ]; then
	ARGS="battery_stop=$BATTERY_STOP $ARGS"
fi
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
	# Stop well before the kernel's return: the loader's clock starts at its
	# own boot, about 117 s (the boot image upload) before the kernel does, so
	# XL_SECS=380 runs the kernel about 260 s.  A screenshot in flight when
	# the run's final log fetch stages its buffer makes that fetch download
	# the framebuffer instead of the log (logs/145727-run.log,
	# logs/152821-run.log: 9.8-10.1 MB of pixels, no text).
	elapsed=0
	while [ "$elapsed" -lt $((SECS - 150)) ]; do
		sleep "$STEP"
		elapsed=$((elapsed + STEP))
		./xl screenshot "logs/weather-${elapsed}.png" || true
	done
) &
CAPTURE_PID=$!
trap 'kill "$CAPTURE_PID" 2>/dev/null || true' EXIT HUP INT TERM

LOG="logs/weather-$(date +%H%M%S).log"
# Follow the log every 15 s: the loader keeps one staging pointer, and this
# script's own screenshots (xl screenshot stages the framebuffer) can be what
# a get_staged sends when the run's final log fetch races them
# (logs/145727-run.log: 9.8 MB of pixels, not one byte of text). The periodic
# .live copies keep what the kernel said; XL_FOLLOW=5 changes the period,
# XL_FOLLOW= (empty) turns them off.
XL_FOLLOW=${XL_FOLLOW-15} \
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
