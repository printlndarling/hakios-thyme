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

# The persist registry's index, sensors_registry, is the one file the scene's
# fw-sensors tree does not have (docs/sensors-status.md, "The one registry file
# the image does not have"): the DSP's registry sensor reads it first, and
# without it the registry never creates its instance -- every registry client
# gets result 5. It is per-device state on the persist partition, readable with
# root and read only; the bytes go in as they are (the parser would read a
# text file, but nothing here may alter it).
if "$ADB" shell su -c true >/dev/null 2>&1; then
	mkdir -p "$DEST/persist/registry/registry"
	for source in /persist/sensors/registry/registry/sensors_registry \
	              /mnt/vendor/persist/sensors/registry/registry/sensors_registry; do
		if "$ADB" shell su -c "test -f $source" >/dev/null 2>&1; then
			"$ADB" shell su -c "cat $source" > "$DEST/persist/registry/registry/sensors_registry"
			echo "sensor-files: the persist registry index, $source: $(wc -c < "$DEST/persist/registry/registry/sensors_registry") bytes"
			break
		fi
	done
else
	echo "sensor-files: no root on the phone; the persist registry index was not pulled"
fi

echo "sensor-files: $(find "$DEST" -type f | wc -l) files in $DEST"
