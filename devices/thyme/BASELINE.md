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

## Repeated wireless regression on the pinned inputs

Two separate cold RAM boots used the same kernel (`cb57723fc501`), wireless
scene image (`ba246c0e5b3d`) and THYME loader (`60a0fa9c11c9`). Both passed the
Wi-Fi discovery, Wi-Fi power-cycle and Bluetooth LE gate:

| Run | Source | Log SHA-256 |
|---|---|---|
| `logs/225307-run.log` | `7f475447` | `e7f5763b307a4f8a3cf26534053ec87cfd9f064912671c6ed4d95a3220a26087` |
| `logs/230506-run.log` | `5aaf2e22` | `445a32afd1a1235118c60d5a16f138fbfd16e6002a5c91be75dd7aaff8d04bbb` |

The second run is the current UI reference:

| Evidence | SHA-256 |
|---|---|
| `logs/235513-run.log` | `cd437436d2168acb916ba08f7f343ec24b2601ea70634c27095af13ab0a9e88f` |
| `logs/wirelessregress-wifi.png` | `3be43e133b623d312f15c31f1c249f1c75fd6a6c76d503ec2c7c964ce432f8f7` |
| `logs/wirelessregress-bt.png` | `927a712e9e7f76697d87a752d08909c8ed3ad59e38dec7a7377469cf04e3d12e` |

The Wi-Fi page shows the switch on and real 2.4 GHz and 5 GHz networks. The
Bluetooth page shows the switch on and the discovered `Accessory`. In the
second run, `net_test` returned three APs in 0.6 s, Wi-Fi off removed the
interface, Wi-Fi on restarted the radio, and the following scan again returned
networks. The H4 transport ended with 528 LE reports, `queue=0`,
`queue-err=0`, and `enqueue-errors=0`.

### Bluetooth connection result

The Bluetooth-only pairing scene was then run with the same full boot arguments.
Its log is `logs/234132-run.log`, SHA-256
`7de27ab44c9c97c974da427bee19c6ab094b445151267a09be3d8a47298de3ff`,
and the failure screenshot is `logs/wirelessregress-bt.png`.

The HCI trace corrects an earlier interpretation that no connection-complete
event arrived:

```text
iOS opcode=0x2011                 Add Device To White List
iOS opcode=0x200d                 LE Create Connection, filter policy 1
command status 0x200d status=0
LE Enhanced Connection Complete status=0 handle=1 role=0
iOS opcode=0x41d                  LE Read Remote Features
disconnection complete reason=0x3e
```

The link-layer connection is established, then the peer or its connection
setup times out before remote feature exchange. Reason `0x3e` is
`Connection Failed to be Established`. This is not an H4 queue, event-mask or
Connection Complete delivery failure.

The follow-up run `logs/002232-run.log`, SHA-256
`fcc8887b272125a70ad24879933ed1af9397c240841dae97461c1fb4656e2606`,
identifies the peer's advertising report before the attempt:

```text
LE advertising report type=0 (connectable undirected)
RSSI=-99..-105 dBm
LE Enhanced Connection Complete status=0
disconnection complete reason=0x3e
```

The advertiser is connectable, but it is at the edge of usable range. The
controller establishes the link and then loses it before remote feature
exchange. Pairing must next be repeated with a known-good BLE peripheral close
to the phone; this anonymous, approximately -100 dBm peer is not suitable
evidence that the HakiOS host stack cannot pair.

| Pairing evidence | SHA-256 |
|---|---|
| `logs/btpair-le-progress.png` | `40ee458fb5207a954c447489ffa5cfb28f1907840df75d8e338bdb602f432fd2` |
| `logs/btpair-le-result.png` | `0e263981b3ae2db81e0dfd381ac47d894c3bb9c289f1e0a59388d6052d7dd990` |

The delayed retry in `logs/004734-run.log` (SHA-256
`8c5e4b11bd81c2c2c7554d34bd9742b1160168b0ec581cf669f7e09b9ebcc280`)
made two real connection attempts to the same peer and reproduced
`status=0` followed by reason `0x3e` both times. Parsing the advertisements
found stronger connectable advertisers, but the strongest unnamed one was
-84 dBm and the named candidates were still -97/-98 dBm. Stock Settings
continued to select the approximately -100 dBm anonymous peer. A known,
close-range BLE peer is therefore still required before this gate can pass.
