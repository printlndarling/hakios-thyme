# THYME clean-main RAM baseline

This file pins the inputs for hardware comparisons. A result only describes this
exact source revision and artifact set; do not mix a clean-main kernel with an
experimental kernel or with an older resident system image.

## Source

| Item | Value |
|---|---|
| Branch | `main` |
| Verified source | `b12838cc` (`Port THYME camera identification path`) |
| Worktree | clean (`git status --porcelain` empty) |

## RAM artifacts

| Artifact | SHA-256 |
|---|---|
| `devices/thyme/prebuilt/xnu-usb.img` | `2d662c991a5abacc9f229d3b9d556665959fbda16ec2887f357b7dea56400e10` |
| `devices/thyme/prebuilt/SILICIUM_UEFI.fd` | `3837561780bf4edd37a7818ba737cd0b91c9a1804d7442eeeff6a2304f8af55d` |
| `xnuloader/loader-thyme.efi` | `7bc139e4b1e4d8fc185a909ff516a5c6a5b737558ece862b30b88bf5891d46cb` |
| `devices/thyme/prebuilt/system.apfs` | `fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4` |
| `out/thyme-boot/root-wirelessfull.apfs` | `373a75aeac33f568fe610f1cf5298163e2c9c92be43b2976b932537e835ecbd1` |
| clean-main `hakios-kernel` | `b110d08ffb5414f261d3c0d068d196dafa6946585aadc574abaeb37dd67b9be1` |

The kernel binary hash changes when the build metadata changes. The source
commit, clean worktree, build log, and artifact hashes above must all be
recorded for a hardware run.

`devices/thyme/ram-baseline.conf` is the machine-checked copy of these local
artifact hashes. `ram-regress.sh` refuses to boot different bytes unless
`ALLOW_NEW_BASELINE=1` is explicit, and a verified new candidate must update
that manifest in the same commit. The manifest also hashes the complete
effective boot-argument string, including `RAM_EXTRA_ARGS`, so a focused test
cannot silently disable `wifi`, `bt` or another subsystem.

## Boot arguments

```text
touchscreen=1 buttons=1 umi_touch=1 umi_keys=1 bootcpu=7 cpus=7
cpufreq=1 thermal=1 thermal_stop=95 battery=1 adsp=1 sound=4 mic=4
haptic=5 umi_haptic=5 wifi=1 bt=1 touch_lat=1
```

`system.apfs` is loaded once with `./xl ramdisk`; the scenario image is passed
as `XL_RAMDISK`. The only canonical RAM regression entry point is:

```sh
devices/thyme/ram-regress.sh
```

The script selects the THYME ADB serial, boots the pinned UEFI-capable image,
uploads the system image, checks every input against `ram-baseline.conf`,
uses the complete `ram.args`, and runs `root-wirelessfull.apfs`.

## Current verified run

The clean `b12838cc` boot completed all of these in one 230-second RAM run:

- 5 GHz WPA2 association and four-way handshake;
- DHCP address `192.168.0.103`;
- Wi-Fi off removed the interface;
- Wi-Fi on restarted the radio;
- the second scan found the configured 5 GHz and 2.4 GHz networks;
- the second association installed both keys and made en0 usable;
- Safari opened `https://example.com`;
- Bluetooth Settings opened and the HCI scan path remained healthy.

| Evidence | SHA-256 |
|---|---|
| `logs/101932-run.log` | `3b49b31b493bc27c189653749b581e39772ebb4993debefd99237ab5e158e8fd` |
| `logs/101901-wifi.png` | `4280a171467d9a7827dd02773f921107a3fb2a9247aa47e5d63c1cd852a1e2af` |
| `logs/101901-bt.png` | `b51f23fe88c556f3a5a35b4a94db416c05d4385f49effd10f71c256244745969` |

The screenshots show the Wi-Fi switch on with real nearby networks after the
power cycle, then stock Bluetooth Settings discovering `Accessory` while the
Wi-Fi status icon is still active. The run's later radio teardown happened after
`loader_return_after` expired, during the intentional return to the loader; it
is not a Wi-Fi startup failure.

A separate full-system `press_power=90` run blanked the display on the first
short press and woke it on the second. Evidence: `logs/075530-run.log`,
SHA-256 `0d91ebdba4e4615eb7314180d0b288f22515392592eaf73ba9a93d951c61b85d`.

### PM8150L flashlight pulse

A separate bounded `torch=2 torch_secs=2` RAM run identified the PM8150L flash
module on SPMI channel 97, set channel 0 to 125 mA, observed
`INT_RT_STS=0x90` (the LED ramped up), and switched it off after two seconds.
The final readback was `enable=0000 channels=0000 target=0000 (dark)`.

Evidence: `logs/081934-run.log`, SHA-256
`6b6d07900ad0abf4fefc6a8af4bd17b3534f70d9250d5f941d3d475ffed30301`.
This is a hardware-level PASS for the bounded torch path.

The `torch=3` userspace path was then exercised with `torch_test`:

- `AVFlashlight` reported `hasFlashlight YES` and was available;
- mediaserverd opened `/dev/hakios-torch`;
- the flashlight's level request reached the kernel at 1000/1000 and the LED
  turned on;
- the kernel's 30-second safety limit switched it off.

The same `torch_test` process then held one `AVFlashlight` instance across the
full sequence: set level 1, wait five seconds, set level 0, and read the kernel
state. The kernel log records:

```text
torch: 159.648 s: ON (iOS asked for 1000 of 1000; the battery 28 C)
torch: 164.647 s: off (iOS asked for 0)
```

This proves the userspace ON and OFF path used by Control Center.
Evidence: `logs/090116-run.log`, SHA-256
`d8ed668fbf15c9b5408a08347777e7cb278b50599c6b256a24540140927a0631`.

The complete `userland/rc.jobs.torch` scene was then run with
`torch=3 torch_max_s=5`. It exercised both the programmatic path and the stock
Control Center: `AVFlashlight` requested 1000 then 250, the Control Center taps
requested 850, and every request reached the kernel and was bounded by the
configured safety timer. The kernel's summary records:

```text
ON (iOS asked for 1000 of 1000)
ON (iOS asked for 250 of 1000)
ON (iOS asked for 850 of 1000)
ON (iOS asked for 1000 of 1000)
```

Each illumination was followed by the five-second limit and returned dark.
Evidence: `logs/091200-run.log`, SHA-256
`1350d74549e927bb7bc751bf455ea666cf09d6c0d6cfbdf2807d4971605276a7`.

### THYME rear camera identification

The THYME camera driver now reuses the UMI sensor/power implementation with
the official camera service. A `cam=2 cam_after=0` RAM run reached the sensor
and reported:

```text
UMI camera: S5KHMX ID 0x1ad0, revision register 0xa000
UMI camera: module sensor-version source register 0x000e=0x1301
UMI camera: release supply 2 error=0
UMI camera: release supply 1 error=0
UMI camera: release supply 0 error=0
UMI camera: sensor probe finished error=0; sensor stream was not started
```

This proves the THYME GPIO, PMIC, MCLK, CCI and sensor identification/power
sequence. It does not yet prove CSI frames or camera capture.
Evidence: `logs/101242-run.log`, SHA-256
`56994b92e8a30dd9b65f5f9ec13c128189b9849ed1a0d5989e40fe99eddf63e9`.

## Historical verified run

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

The historical clean-main run proves that the committed Wi-Fi path still works after a
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

## Current-baseline Wi-Fi network gate

The current THYME Wi-Fi driver does not implement the legacy generic driver's
`wifi_join=1`. `net_test joinfile` now reads a gitignored two-line credential
file and issues the same Apple80211 `ASSOCIATE` request Settings uses. It retries
transient joins. This tests the live driver without exposing credentials in the
boot command line or log.

The successful run used kernel `a443cbbdb18a`, scene image
`69dfabccb130`, and loader `60a0fa9c11c9`:

| Evidence | SHA-256 |
|---|---|
| `logs/022344-run.log` | `fad384f09a7810064df64baff463cfc767ae05595d80c4717d0f42bf0bd77942` |
| `logs/wifisafari-example.png` | `9af0847b3e87c52454650144d3aabcd9b85bdb24b31bb945100cab5e75917178` |
| `logs/wifisafari-success.png` | `6731631914a9ab1879de3cc9c5e1dca4d2454deef82883bfe5eeb567e9724da2` |

Observed on hardware:

- Authentication and association succeeded on 5 GHz;
- all four handshake messages completed and pairwise/group keys were installed;
- DHCP assigned `192.168.0.105`, router `192.168.0.1`, and DNS
  `192.168.1.1`, `192.168.0.1`;
- DNS queries and replies reached both servers;
- MobileSafari/WebKit/trustd established TLS connections through en0;
- Safari rendered the HTTPS Apple success page.

`net_test`'s direct `getaddrinfo` call still reports a 30-second failure while
mDNSResponder and Safari resolve the same names later in the same boot. This is
tracked as a test-harness problem, not a network PASS failure.

### Combined wireless regression

The complete cold-boot gate used `userland/rc.jobs.wirelessfull` with the same
full Wi-Fi/Bluetooth argument set. It first joined the private network, obtained
DHCP settings, and opened Apple's HTTPS success page. It then switched Wi-Fi
power off and on, associated again, and finally opened Bluetooth Settings.

| Evidence | SHA-256 |
|---|---|
| `logs/023242-run.log` | `35d6f1e395ebc0767d9795c8037139abed520f88bdb6e88634b027eec150eb6b` |
| `logs/wirelessfull-safari.png` | `299c9cef2bdd3fd3c772fcb33ef35599ce242bba64eef3e89248378ac5bd2c51` |
| `logs/wirelessfull-bt.png` | `9e1afc0577ca80ee114241b8eb31555715959dff67b5a974b6af1d3195881c6d` |

Observed:

- initial 5 GHz association, four-way handshake and DHCP succeeded;
- Safari rendered the Apple HTTPS success page;
- Wi-Fi power off removed the interface and power on restarted the radio;
- the second join reached `THE NETWORK IS JOINED` with the pairwise/group keys
  installed;
- Bluetooth Settings listed both `Accessory` and `HONOR X10 Max`;
- the Wi-Fi status icon remained active in the final Bluetooth screenshot.

A later named-peer attempt, `logs/024316-run.log`, kept Wi-Fi connected but the
list contained only `Accessory` when the second row was tapped. No pairing was
attempted. `HONOR X10 Max` is an intermittent nearby advertisement, not yet a
controlled peer; the known-peer Bluetooth gate remains open.

## THYME UFS survey

The official loader survey was run read-only with `ufs=1` on the full THYME boot
argument set. Survey log `logs/025212-run.log`, SHA-256
`d62c7ecc3c3dc4633085561dd13dd248dff8a730999e173704f0edce25749a87`,
contains the loader result:

```text
ufs: no partition information in this UEFI
```

The loader did execute `ufs_survey`; the failure occurs because the current
Silicium UEFI did not publish any UEFI partition-information handle for the
phone's UFS. There is therefore no BlockIo/GPT path for the survey or for the
kernel's loader-backed block service. The run performed no storage writes.

Before an installer or persistent-storage design can be accepted, the THYME
Silicium UEFI must expose the UFS controller/LUNs and GPT partitions, or an
equivalent read-only native UFS path must be brought up and verified.

The phone's own `xbl_a.img` contains the vendor modules the current image
omits. `tools/uefi-modules.py` identified and extracted, without modifying the
image:

- `UFSDxe` `0d35cd8e-97ea-4f9a-96af-0f0d89f76567`;
- `PartitionDxe` `1fa1f39e-feff-4aae-bd7b-38a070a3b609`;
- `SdccDxe` `f10f76db-42c1-533f-34a8-69be24653110`.

Their PE32 hashes and the next UEFI integration step are recorded in
`docs/uefi-ufs-modules.md`. The vendor binaries remain local and untracked.

The blocker was then removed in the isolated THYME UEFI build. The DXE platform
now carries the phone's own `UFSDxe`, `DiskIoDxe` and `PartitionDxe`, and its BDS
hook performs `EfiBootManagerConnectAll()` before starting the resident loader.
The RAM test `logs/041710-run.log`, SHA-256
`e62b25dc98d0e860087bfba74441422a8042a544670d0c928f87ebdc08d523d5`, enumerated
six LUNs, validated every GPT header/table CRC, and listed the complete
partition set including `super`, `userdata`, both boot slots, `xbl_a/b`,
`tz_a/b`, `modem_a/b` and the loader's partition hashes. This is a read-only
PASS; no write path or installer is accepted by this result.

### UFS-capable full wireless baseline

The same UFS-capable UEFI then passed the complete wireless workload in one RAM
boot. The input set was:

```text
source 25a20d7c
kernel a443cbbdb18a
UEFI   3837561780bf4edd37a7818ba737cd0b91c9a1804d7442eeeff6a2304f8af55d
image  2e074fddf19c1b241e59e552a2b104ef81929356bdf379b1e7248b1961f53e47
scene  373a75aeac33
```

The run enumerated all six UFS LUNs and partitions, then completed:

- Wi-Fi 5 GHz join, four-way handshake and DHCP;
- Safari HTTPS loading;
- Wi-Fi power off/on and a second successful join;
- Bluetooth LE discovery with zero queue errors.

| Evidence | SHA-256 |
|---|---|
| `logs/042810-run.log` | `f024a0b15f061feb49649097f708293bf591f117c81d9bde4a68f8514b137328` |
| `logs/ufs-full-safari.png` | `dfb5ca5eab694969a4bfc8db48b72ae2d4afaceef39e88b48ffbfdab8a0cbad4` |
| `logs/ufs-full-bt.png` | `4667ea92b8a0bac68af0a64c12c661c0887734a6e8cb2569a17631bbdbb4a0a6` |

The final packaged `devices/thyme/prebuilt/xnu-usb.img` was then cold-booted
again with `ufs=1`:

```text
source bf973a76
kernel a443cbbdb18a
UEFI   3837561780bf4edd37a7818ba737cd0b91c9a1804d7442eeeff6a2304f8af55d
loader 7bc139e4b1e4
image 9e219ee5b77b
boot   2d662c991a5abacc9f229d3b9d556665959fbda16ec2887f357b7dea56400e10
```

The log `logs/043951-run.log`, SHA-256
`c1b0b98a05e08ac2c460d08d0c57e4de682b6e8fb4d6477ab7f7ab5372ea8cb9`,
repeated the six-LUN/GPT enumeration and completed the bounded RAM return to
Android.

### Storage layout

The same survey was parsed with `tools/ufs-super.py`. Both liblp metadata slots
validated, and slot A uses 6.25 GB of the 8.50 GB `super`, leaving 2.24 GB
uncovered. `userdata` is about 111.4 GB and ends within 24 KB of LUN 0's end.
There is no free GPT partition. The read-only layout and the persistent-storage
options are recorded in `docs/storage-layout.md`.
