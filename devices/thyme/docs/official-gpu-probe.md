# Official-main GPU, touch and keys probe

Date: 2026-11-01

## Build

```text
kernel.ver=2.1.13-hakios-darwin22.5-sm8250-g713d2af3-dirty
drivers: thyme_adreno thyme_input battery battguard ...
payload sha256=713c98aa8b17dca138758ec3b0c28bed41f197c2e23de93265d049ff5855a7fb
```

The device-owned Adreno 650 implementation from the legacy THYME bring-up now
builds as `CONFIG_HAKIOS_THYME_ADRENO`. The common Redwood Adreno driver remains
disabled. `MiniIOSurface` gained a device-neutral `gpu_written` invalidation
flag used by the THYME path.

## Result

The official kernel reached SpringBoard and Mesa's freedreno path through
`HakiosGPU`.

```text
hakios-gl: frame 1: 135 GL calls, 4 draws, 8 triangles ...
hakios-gl: frame 100: 140 GL calls, 23 draws, 164 triangles ...
hakios-gl: frame 1000: ...
hakios-gl: frame 2140: 136 GL calls, 23 draws, 164 triangles ...
```

There was no "no accelerated renderer" fallback and no softpipe path. Typical
steady-state frame driver times were about 12-16 ms, with larger bursts while
SpringBoard launched widgets and apps.

## Concurrent input

The same run verified the physical input path:

```text
UMI FTS: poll=23125 events=415 errors=0 irq=1 active=0
UMI keys: volume-down pressed
UMI keys: volume-down released
UMI keys: power pressed
UMI keys: power released
```

The transient PM8150 `PON=0 GPIO6=1` read failure recovered and did not stop key
events.

## Battery

The loader's saved charger action remained active during part of the run:

```text
battery: SOC=99%, V=4169 mV, I=164 mA, T=25 C, battery=1 charger=1 charging=1 action=3
battery: SOC=99%, V=4271 mV, I=12 mA, T=25 C, battery=1 charger=1 charging=1 action=3
```

The run ended through the 240-second `loader_return_after` watchdog and returned
to Android normally.
