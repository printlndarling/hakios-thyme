# Official-main CPU frequency and thermal probe

Date: 2026-11-01

## Purpose

Bring the THYME SM8250 EPSS CPU frequency domains and TSENS thermal governor to
the official common kernel, and add an orderly high-temperature hand-back to the
RAM loader instead of relying on a panic or the hardware's last-resort reset.

## Integration

`CONFIG_HAKIOS_THYME_POWER` builds:

```text
hakios.thyme_board.c
hakios.thyme_cpufreq.c
hakios.thyme_thermal.c
```

The configuration enables:

```text
CONFIG_CPUFREQ=y
CONFIG_THERMAL=y
CONFIG_THERMAL_STOP=95
```

The three EPSS domains and both TSENS register windows match THYME's device
tree:

```text
EPSS      0x18591000, 0x18592000, 0x18593000
TSENS TM0 0x0c263000
TSENS TM1 0x0c265000
```

`thermal_stop=<C>` is now read by the THYME governor. When the hottest valid
sensor stays at or above that threshold for two seconds, it saves the data
volume if available, stops the GPU, Wi-Fi and audio DSP in order, and returns to
the loader. The existing 115 C emergency panic remains as the final fallback.

## Build

```sh
tools/hakios-device.py check thyme
make thyme_defconfig
make config
make kernel -j8
```

The source compiled and linked. As on the rest of this phone configuration, the
XNU kext symbol-set checks exit non-zero after the kernel has already been
produced; `tools/make-kernel.sh` classifies those checks as expected.

## Hardware run

The RAM-only test used this command line:

```text
debug=0x8 loader_return_after=240 epoch=<run> sb_delay=8 bootcpu=7 cpus=7
umi_touch=1 umi_keys=1 gpu=1 cpufreq=1 battery=1 battery_charge=1
battery_stop=25 battery_stop_poll=30
```

The legacy loader rejects `thermal=1`, so the governor's default-on mode was
used with `cpufreq=1`. No image was flashed.

## Frequency result

All three frequency domains were raised from the firmware state:

```text
cpufreq: group 0 (3 of our cores): row 13 -> 16, 1804 MHz
cpufreq: group 1 (3 of our cores): row 0 -> 17, 2419 MHz
cpufreq: group 2 (1 of our cores): row 0 -> 20, 3187 MHz
```

This maps to the three THYME clusters: 3 small cores, 3 big cores and the prime
core.

## Thermal result

The governor mapped both TSENS blocks and started all 27 sensors. The prime core
read 31.9 C at startup. SpringBoard and the Adreno 650 path continued running.

During the 240-second run the hottest CPU sensor rose through moderate pressure
and reached the configured performance-core trip:

```text
thermal: moderate, hottest cpu-1-2 at 66.6 C; cores at rows 16/16 17/17 20/20
thermal: a minute: moderate, hottest cpu-1-1 at 67.7 C, slowed 0 times so far
thermal: heavy, hottest cpu-1-3 at 75.9 C; cores at rows 16/16 16/17 20/20
thermal: moderate, hottest cpu-1-3 at 65.0 C; cores at rows 16/16 17/17 20/20
thermal: a minute: nominal, hottest cpu-1-3 at 42.0 C, slowed 1 times so far
```

The big-core domain was reduced from row 17 (2419 MHz) to row 16, then recovered
after cooling. This is the first physical confirmation that the official-main
thermal path controls real hardware rather than only reading sensors.

## Charging observation

The battery service reported a positive signed current while iOS was running:

```text
battery: SOC=99%, V=3881 mV, I=1239 mA, T=27 C,
         battery=1 charger=1 charging=1 action=3 (13 reads, 0 failed)
battery: SOC=99%, V=4226 mV, I=106 mA, T=28 C,
         battery=1 charger=1 charging=1 action=3 (25 reads, 0 failed)
```

This run therefore did not reproduce the earlier observation that iOS could not
charge. A longer charging and thermal soak is still required.

## thermal_stop verification

To exercise the new hand-back without heating the phone, a second run used the
same kernel with `thermal_stop=30` and a 90-second watchdog. It triggered after
the sensor had remained above the artificial threshold for 2000 ms:

```text
thermal: 27 sensors; the prime core reads 34.2 C; caps on; stop at 30 C
--- thermal: cpu-1-3 reads 51.4 C, at or above thermal_stop=30 for 2000 ms:
    back to the loader ---
adreno: 0: the 3D core before it is stopped: ... ring read up to 15 of 15
```

`fastboot -s xnuloader getvar result` then reported `returned`, and the loader
log ended without a panic. The normal THYME configuration uses the safer
`thermal_stop=95`.

## Safety note

`thermal_stop` returns XNU to `xnuloader`; it cannot directly boot Android from
inside the kernel. Any unattended launcher must notice `getvar result` changing
from `running` to `returned` and issue:

```sh
fastboot -s xnuloader reboot
```

Otherwise the phone remains in the custom RAM loader. This host-side action is
required for the current RAM-only bring-up.

## Remaining checks

- Run a 30-minute session under sustained CPU/GPU load and verify repeated
  throttle and recovery cycles.
- Compare all 27 sensor readings with Android's thermal-zone readings.
- Test a natural `thermal_stop=95` event, then automatically reboot to Android.
- Verify display-off, charging and thermal behavior together.
