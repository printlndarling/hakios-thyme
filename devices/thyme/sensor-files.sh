#!/bin/bash
# Pull the sensor DSP's registry inputs from the phone's Android into fw-sensors/.
#
# The SLPI's sensor process reads these through the FastRPC reverse tunnel, which
# devices/thyme/drivers/sensor/hakios.thyme_slpi.c serves from /fw/sensors in the
# root image (userland/build.sh copies fw-sensors/ there). They are vendor data
# and stay out of git like the other fw-* trees.
#
#   devices/thyme/sensor-files.sh                 (the phone must be in Android, adb)
#   ADB=/tmp/hakios-phone-tools/adb devices/thyme/sensor-files.sh
#
# The persist registry itself (/mnt/vendor/persist/sensors) needs root on the
# phone; the parser rebuilds the registry from these files when it is missing.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ADB=${ADB:-adb}
DEST=${DEST:-$ROOT/fw-sensors}

mkdir -p "$DEST/config" "$DEST/soc0"

# sns_reg_config: version, the sysfs files the parser reads, and its output path
"$ADB" shell cat /vendor/etc/sensors/sns_reg_config | tr -d '\r' > "$DEST/sns_reg_config"

# the per-sensor configuration
"$ADB" shell ls /vendor/etc/sensors/config | tr -d '\r' | while read -r name; do
	[ -n "$name" ] || continue
	"$ADB" shell cat "/vendor/etc/sensors/config/$name" | tr -d '\r' > "$DEST/config/$name"
done

# the platform identity the parser substitutes into the config
for name in hw_platform platform_subtype platform_subtype_id platform_version soc_id revision; do
	"$ADB" shell cat "/sys/devices/soc0/$name" | tr -d '\r' > "$DEST/soc0/$name"
done

echo "sensor-files: $(find "$DEST" -type f | wc -l) files in $DEST"
