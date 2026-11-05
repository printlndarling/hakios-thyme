# THYME 30-minute UI soak on the 60 Hz default

Date: 2026-11-06

## Test build

```text
kernel version:  2.1.13-hakios-darwin22.5-sm8250-g0bba86c7-dirty
boot arguments:  ram.args with fb_hz=60 (panel timing 2f 01 / 26 01 / b5 84)
run length:      loader_return_after=1805
log:             logs/035725-run.log
```

The session is the daily one: `touchscreen=1 buttons=1 umi_touch=1 umi_keys=1
bootcpu=7 cpus=7 cpufreq=1 thermal=1 thermal_stop=95 battery=1 adsp=1 sound=4
mic=4 haptic=5 umi_haptic=5 wifi=1 bt=1 touch_lat=1 gpu=1 fb_hz=60`. The
phone was not interacted with; SpringBoard and its ambient animations ran the
whole time.

## Why this run

The display work on 2026-11-06 showed that the controller the firmware leaves
behind refreshes the panel about 60 times a second whatever `fb_hz` asks, so
the daily default became a self-consistent 60 Hz (`fb_hz=60`, panel timing
`2f 01`/`26 01`). This soak is the first long run on that default.

## End state

The loader returned on its own after 1929 s of kernel time. The ordered
teardown stopped the GPU, the audio DSP and TrustZone's view of it; the last
lines are `THE AUDIO DSP IS SHUT DOWN` and the always-on processor taking
`load_state off`.

```text
[ 1929.141]: adsp: 11: THE AUDIO DSP ACKNOWLEDGES THE STOP after 55.656 ms
[ 1929.153]: adsp: 11: TrustZone returns 0, answer 0
[ 1929.153]: adsp: 11: THE AUDIO DSP IS SHUT DOWN
```

No panic, no watchdog reset, no kernel fault. The two `backboardd Corpse
allowed` / `EXC_CORPSE_NOTIFY` lines at +1919 s happen after the GPU was
given back (`adreno: the run is about to end`) and are the same teardown-only
artifact the previous official soak recorded; a fresh backboardd starts as
the run ends.

## What the run measured

- Framebuffer: a steady 20 frames a second while the ambient UI animated,
  every swap on a beat and none dropped; the driver copy stayed at 890-968 us.
- GPU: 631-635 us of work per submission, 101 submissions in every 5-second
  window, 98 MB of buffers kept and none made during the run.
- Thermal: the hottest core reached 70.5 C; the three frequency rows stayed
  at 16/16, 17/17 and 20/20 throughout (no throttle).
- Memory: no growth in the kept-buffer pool; the memorystatus lines are the
  normal launchd and poster services settling at the end of the run.
