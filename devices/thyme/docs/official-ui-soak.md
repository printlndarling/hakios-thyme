# THYME 30-minute UI and charging soak

Date: 2026-11-02

## Test build

```text
repo HEAD:       44ea0e0
kernel SHA-256:  e6a699afbf44938c5ea5b36dfd496cf9c42833790952386013b782028fafce2b
payload SHA-256: 5e4b738f8d7737e13c38be60890c44f88adcbe8a56f4ef1cea5470abf937b411
system image:    fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4
```

The test used `fastboot boot`, resident `oem ramdisk`, full 2.4 GB
`system.apfs`, the root image and `oem run`. No partition was written.

Session options:

```text
loader_return_after=1805
gpu=1 touchscreen=1 umi_touch=1 umi_keys=1
cpufreq=1 thermal=1 thermal_stop=95
battery=1 adsp=1 sound=4 mic=4 haptic=5
wifi=1 bt=1
```

## End state

The loader returned normally after the 30-minute window. The final framebuffer
captured immediately after return still showed the SpringBoard home screen with
icons, dock, widgets and status bar:

```text
logs/official-ui-soak-30m.png
sha256 62b6e43c690eb95561cd971a415584bc63b96d5e32182ef640188d62cc6cd030
```

The final raw log tail has no panic, watchdog reset or driver fault:

```text
logs/official-ui-soak-30m-tail.log
sha256 f98d25f2696272f4bf28b960e93487338067ec919df27bd90602c5b2bb296673
```

The last minutes continued to render and returned all GPU submissions:

```text
MiniIOMobileFramebuffer: 51 frames in 5000 ms, 0 gave up, 0 late
HakiosGPU: 101 submissions, 0 over before they began, 0 longer than 4 ms
GPU clock: 587 MHz, GPU sensors 39.0/40.9 C
thermal: nominal, hottest CPU 44.0 C, slowed 0 times
memory available: 2192 MB, stable across samples
```

The ordered teardown completed GPU, audio and ADSP shutdown. Two teardown-only
lines (`backboardd Corpse allowed`, `EXC_CORPSE_NOTIFY`) occurred after the
30-minute run had ended and did not precede a runtime restart.

## Charging telemetry

Charging and battery reads remained healthy:

```text
SOC 97%, V 4176-4180 mV, I +129..+151 mA, T 30 C
charger=1 charging=1, 349 reads, 0 failed
```

This PASSES 30-minute charger/endpoint endurance. It does not prove net charge
growth under UI load: SOC stayed at 97% while the phone rendered and ran the
full audio/UI stack.

## Battery display discrepancy

The final kernel telemetry reported 97%, while the retained status bar showed
99%. That is larger than a short charge interval can explain. The user-visible
battery percentage therefore needs a refresh/path audit and is marked PARTIAL,
even though the kernel readings themselves are valid.

## Checklist impact

- 30-minute UI stability: PASS.
- 30-minute charging endpoint: PASS.
- Net charge under UI load: still PARTIAL.
- Battery percentage display: PARTIAL pending the stale-status-bar fix.
