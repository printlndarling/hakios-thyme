#!/bin/bash
# Read the target Android phone's wlan0 address and keep it in the ignored
# prebuilt directory for a RAM-only Wi-Fi boot. The serial must be explicit.
set -euo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PHONE_SERIAL=${PHONE_SERIAL:?set PHONE_SERIAL to the target Android serial}
ADB=${ADB:-$(command -v adb.exe || command -v adb)}

address=$(
	"$ADB" -s "$PHONE_SERIAL" shell \
		"su -c 'cat /sys/class/net/wlan0/address'" |
	tr -d '\r\n'
)

if ! printf '%s\n' "$address" |
	grep -Eq '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'; then
	echo "wifi-boot-args: Android did not return a valid wlan0 MAC" >&2
	exit 1
fi

mkdir -p "$DEVICE_DIR/prebuilt"
printf 'wifi_mac=%s\n' "$address" >"$DEVICE_DIR/prebuilt/wifi-boot.args"
echo "wifi-boot-args: wrote devices/thyme/prebuilt/wifi-boot.args"
