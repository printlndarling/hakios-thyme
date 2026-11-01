# Official-main core probe

Date: 2026-11-01

## Purpose

Verify the official common kernel and the new THYME device configuration on the
physical Xiaomi Mi 10S before migrating any device-specific GPU, touch, audio or
wireless driver.

## Build

```text
kernel.ver=2.1.13-hakios-darwin22.5-sm8250-g4f4a4d3a
drivers: virtio_sound virtio_input virtio_net battery battguard virtio_blk
payload sha256=37ad1ca844983aca68056e687d9d3d83f84548216da7804d0b54038203482019
```

The known-good THYME RAM loader was used only to isolate the kernel migration.
The UEFI loader itself is still from the legacy tree and must be rebuilt later.

## Command line

```text
-v loader_return_after=120 sb_delay=8 bootcpu=7 cpus=7 battery=1
battery_stop=25 battery_stop_poll=30
```

## Result

The kernel reached userspace and SpringBoard, then returned to the loader when
`loader_return_after` expired.

```text
Darwin 22.5.0 iPhone10,3
Darwin Kernel Version 22.5.0: Sun Nov  1 10:46:58 +08 2026
memory 6144 MB, page size 16384, 7 cpu
```

Launchd started `backboardd` and `SpringBoard`; MiniIOSurface and
MiniIOMobileFramebuffer produced SpringBoard surfaces through the generic
framebuffer path. Without the device GPU driver the frame rate was low, as
expected (about 8-10 frames a second).

Battery and battguard read the loader's shared page correctly:

```text
flags=f SOC=92% V=4332 mV I=-162 mA T=24 C action=7
battery: SOC=92%, V=4282 mV, I=-97 mA, T=24 C, battery=1 charger=1 charging=0
battguard: charge 92% (battery present, charger attached, charging no)
```

The charger is attached but current is negative because this probe does not yet
carry the THYME loader's current-aware one-shot `TakeAction` logic.

## Next work

1. Port THYME board/discovery and the FTS521 touch driver.
2. Port the PM8150 button driver.
3. Port the Adreno 650 GPU path and firmware mapping.
4. Port the current-aware charger action and test positive current.
5. Port audio, haptics, Wi-Fi and Bluetooth one at a time.
