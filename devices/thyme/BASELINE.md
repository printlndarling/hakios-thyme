# THYME clean-main RAM baseline

This file pins the inputs for hardware comparisons. A result only describes this
exact source revision and artifact set; do not mix a clean-main kernel with an
experimental kernel or with an older resident system image.

## Source

| Item | Value |
|---|---|
| Branch | `main` |
| Commit | `0f28e9c33ed1a1881628869609776eb23c24668e` |
| Worktree | clean (`git status --porcelain` empty) |

## RAM artifacts

| Artifact | SHA-256 |
|---|---|
| `devices/thyme/prebuilt/xnu-usb.img` | `a559fa38ab4e684459dd9f4d49066486da5ce734038ae723b17c9f632d2d93be` |
| `devices/thyme/prebuilt/system.apfs` | `fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4` |
| `out/thyme-boot/root-wirelessui.apfs` | `0e770d4a9a73f1bd661cc948c946699b138990fc029a1b607c9211900ac97b11` |
| clean-main `hakios-kernel` | `86a4f3423e1858ef27e51790ef6da54611cb6714db3ad7ffd2e19f7e74c99cff` |

The kernel binary hash changes when the build metadata changes. The source
commit, clean worktree, build log, and artifact hashes above must all be
recorded for a hardware run.

## Boot arguments

```text
debug=0x8 rd=md0 battery_shared_pa=0x00000000998de000
loader_return_after=170 sb_delay=8 touchscreen=1 buttons=1
umi_touch=1 umi_keys=1 bootcpu=7 cpus=7 cpufreq=1 thermal=1
thermal_stop=95 battery=1 adsp=1 sound=4 mic=4 haptic=5
umi_haptic=5 wifi=1 bt=1 touch_lat=1 wifi_mac=<phone-wifi-mac>
```

`system.apfs` is loaded once with `./xl ramdisk`; the scenario image is passed
as `XL_RAMDISK`. A RAM boot is:

```sh
adb -s 5290780a reboot bootloader
fastboot boot devices/thyme/prebuilt/xnu-usb.img
./xl ramdisk devices/thyme/prebuilt/system.apfs
XL_RAMDISK=out/thyme-boot/root-wirelessui.apfs \
XL_SECS=190 XL_HOME=fastboot XL_CMDLINE='...' ./xl run
```

## Verified run

- Loader log: `logs/201710-run.log`, SHA-256
  `e180cfa3ea5ff13a842780c7f1740f5791caf5b98829bdaa04e134781027b8b8`
- Screenshot: `logs/clean-main-wifi.png`, SHA-256
  `220bf17333495c8a3c6c3347aefb01bf486d0303410d84294ae2bd103a574fe3`
- Evidence: Wi-Fi startup gate opened after 9204 ms; `net_test` returned three
  access points in 0.6 s; the stock Wi-Fi Settings page displayed real networks.
- Apple80211 handled 53 power queries and 21 scan requests without a refusal.

Wi-Fi power-cycle gate:

- Loader log: `logs/203537-run.log`, SHA-256
  `075d5f2d3622e27a10d335a61aae3282076cbe9ddbe0c6516c853822ef6a413a`
- Screenshot after power off/on: `logs/wifi-after-toggle.png`, SHA-256
  `16b0cfee4034ce277d65c8412d69f240dccd8ad660f9fd06599f525ef82f6091`
- Both `set 19 (POWER)` requests were accepted. The radio was removed when
  switched off, restarted in 0.1 s when switched on, and scanned networks again.

This clean-main run proves that the committed Wi-Fi path still works after a
fresh boot. If a later run fails, compare the source commit, worktree state,
artifact hashes, boot arguments, and loader log before changing driver code.

## Why an earlier pass can disappear

A passing subsystem is only comparable when the complete input set is the same.
Two later 2026-11-02 runs showed why:

- `logs/204958-run.log` used the full wireless command line and passed Wi-Fi
  discovery, Wi-Fi off/on recovery, and Bluetooth LE report delivery in one
  boot.
- `logs/212033-run.log` used the Bluetooth-only command line
  `... bt=1 touch_lat=1`: it did not contain `wifi=1`. The Wi-Fi firmware files
  were present in the scene image, but the kernel never enumerated or started
  the QCA6390. This was an incomplete test invocation, not a source
  regression.

Use `devices/thyme/ram-regress.sh` for RAM regressions. It refuses a dirty tree
unless `ALLOW_DIRTY=1` is explicit, always uses the full
`devices/thyme/ram.args`, selects the THYME ADB serial, and requires the system
and scenario images by path. `xl` now records twelve-digit SHA-256 identities
for the kernel, scene image, and loader as `kernel_sha=`, `image_sha=`, and
`loader_sha=` in the boot command line, so a later log identifies the bytes the
phone actually received rather than only the current Git checkout.

When several ADB devices are attached, set `XL_ADB_SERIAL` or use
`ram-regress.sh`; otherwise `xl` correctly fails closed instead of choosing one.
