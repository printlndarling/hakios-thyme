# THYME daily-driver checklist

Status meanings: `PASS` is observed on the physical Mi 10S in the named build;
`PARTIAL` is implemented but incomplete; `FAIL` is a reproduced failure;
`TODO` has not been tested.

## Current-build regression (2026-11-02)

The official-loader RAM run `logs/143146-run-cpoem.log` reached SpringBoard
but reproduced a device-wide regression:

```text
UMI FTS: board mismatch
display: brightness board mismatch
UMI keys: board mismatch; driver not started
```

The loader omitted `/chosen/hakios-board`, so every THYME driver that rejects
another board (touch, keys, brightness/thermal and the QCA6390 Wi-Fi path)
remained disabled. The run also had no charging bolt and no usable Wi-Fi or
Bluetooth pairing. Bluetooth firmware and HCI reset were still visible in the
kernel log, so that observation does not invalidate the earlier low-level
Bluetooth evidence.

The fix in commit `66500d7` publishes the selected platform as
`/chosen/hakios-board`. A SpringBoard battery-state-refresh change is in the
same commit. Neither fix is a hardware `PASS` until a new RAM run proves the
drivers start and the user-visible charging state updates.

Tested artifacts:

| Artifact | SHA-256 |
|---|---|
| `devices/thyme/prebuilt/xnu-usb.img` | `a559fa38ab4e684459dd9f4d49066486da5ce734038ae723b17c9f632d2d93be` |
| `hakios-kernel` | `10f6b1c18a5fe2e45b5239326b1b681ca41ac9406f17e45193bcc736ad689331` |
| `devices/thyme/prebuilt/system.apfs` | `fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4` |

Post-fix RAM test image:

| Artifact | SHA-256 |
|---|---|
| `out/thyme-boot/root-battery-ui-2.apfs` | `11a23b1b9db35d58a3d732e941b7b5f3a69162624511a23ccb30623cf061df58` |

## Canonical regression after UFS enablement (2026-11-03)

The tested source revision `8a55dec6` on `main` was cold-booted from RAM with the pinned
UEFI-capable image and the complete wireless scenario. The worktree was clean,
and the kernel (`c594cafc9ac1`), boot image (`2d662c991a5a`), loader
(`7bc139e4b1e4`), system image (`fae315dd2ec8`) and scenario
(`373a75aeac33`) all matched `ram-baseline.conf`.

One boot completed:

- 5 GHz WPA2 authentication, association, four-way handshake and key install;
- DHCP address `192.168.0.103`;
- Wi-Fi power off, interface removal, power on and radio restart;
- a second scan and second successful association;
- Safari opening `https://example.com`;
- Bluetooth Settings opening while HCI LE scanning continued with zero queue
  or enqueue errors.

Evidence: `logs/062313-run.log` (SHA-256
`d0086465cdc259e8a7d30d0aa5c0d7aa63f8c7086724b3ae8bfe51e6933a3acf`),
`logs/061901-wifi.png` (SHA-256
`674c7efc33dd6479ab15bf53487abfdaa3127cfe5162db2ee5c60836d40feb19`) and
`logs/061901-bt.png` (SHA-256
`492914edf897d92b5242e9978244ac9cef6bfdfc00386b033445826bc3e7b079`).
The later radio teardown in the log is the intentional
`loader_return_after` shutdown, not a Wi-Fi failure.

The regression command now uses `out/thyme-boot/root-wirelessfull.apfs` by
default and refuses any kernel, boot image, UEFI, loader, system image,
scenario or boot-argument hash that differs from
`devices/thyme/ram-baseline.conf`. This prevents a supposedly fixed feature
from being tested with stale local artifacts or a Bluetooth-only scenario.

### Bluetooth pairing target diagnosis

The HCI trace now records the address in the LE white-list add/remove commands.
This closes the ambiguity in the earlier pairing failures:

```text
LE whitelist add address-type=0
LE whitelist add 0 address=<anonymous advertiser>
LE enhanced connection complete status=0 handle=1
LE enhanced connection peer 0 address=<same anonymous advertiser>
disconnection complete status=0 handle=1 reason=0x3e
```

A Windows Intel Bluetooth controller was then used as a known connectable BLE
peripheral at approximately `-40 dBm`. HakiOS still selected the anonymous
advertiser at approximately `-98 dBm`; the Windows peripheral was not placed in
the white list. The failure is therefore in iOS's choice of the single
unnamed `Accessory` row, not in HCI event delivery, queueing or Wi-Fi/Bluetooth
coexistence.

The Windows GATT provider does not publish a local name through this path, so
iOS collapses nearby unnamed connectable advertisers into one `Accessory`
entry. A named, known BLE peripheral remains required for a valid end-to-end
pairing result.

## Wireless startup and coexistence (2026-11-02)

The board-gating fix in `66500d7` is now verified on the physical phone:
touch and charging indicators work. Wi-Fi was also validated with the full
SpringBoard workload after the QCA6390 mission-mode barrier in `513489d`:
`net_test` returned three access points, and the stock iOS Wi-Fi Settings page
displayed real nearby networks (`logs/182354-run.log`,
`logs/180902-ui-65.png`). This proves the radio, scan path and user-space scan
display can all work together; it does not prove stable discovery on every boot.

Bluetooth firmware, HCI, the iOS H4 endpoint and LE reception are verified
independently. The controller reported seven to eight LE advertisers both with
Wi-Fi off and with Wi-Fi in mission mode. The full regression run
`logs/204958-run.log` then displayed an `Accessory` in stock Bluetooth Settings
while Wi-Fi remained usable. The pair attempt in `logs/212033-run.log` was
accepted by the controller as LE Create Connection (`0x200d`, command status
0), but no LE Connection Complete event followed before iOS timed out. The
failure is therefore in LE connection establishment or the peer, not H4 event
delivery, Wi-Fi coexistence, or missing discovered devices.

A clean-main rerun on 2026-11-02 (`0f28e9c`, clean worktree) again brought Wi-Fi
up, returned three APs through `net_test`, and displayed real networks in stock
Settings. The exact build and artifact set is pinned in `BASELINE.md`. A later
user-visible failure is treated as a boot/artifact mismatch until the recorded
source, worktree, image hashes and boot arguments are compared.

| Wireless test artifact | SHA-256 |
|---|---|
| `logs/201710-run.log` (clean main) | `e180cfa3ea5ff13a842780c7f1740f5791caf5b98829bdaa04e134781027b8b8` |
| `logs/clean-main-wifi.png` | `220bf17333495c8a3c6c3347aefb01bf486d0303410d84294ae2bd103a574fe3` |
| `logs/183439-run.log` (Bluetooth only) | `5d9eebe1c34fe8c948764634c19d4b6ce537e4b830ff910b1aee27a4f0206dc9` |
| `logs/194622-run.log` (Settings spinner) | `c8a572b4e6d8a198212c0b94fb83115c71dd19ce35788237d8459508ba5b238d` |
| clean-main `hakios-kernel` | `86a4f3423e1858ef27e51790ef6da54611cb6714db3ad7ffd2e19f7e74c99cff` |
| `logs/203537-run.log` (Wi-Fi power cycle) | `075d5f2d3622e27a10d335a61aae3282076cbe9ddbe0c6516c853822ef6a413a` |
| `logs/wifi-after-toggle.png` | `16b0cfee4034ce277d65c8412d69f240dccd8ad660f9fd06599f525ef82f6091` |

## User-priority hardware gate

These are the minimum user-facing hardware checks for the current THYME port.
No item moves to `PASS` from source inspection alone.

| Item | Status | Evidence / next action |
|---|---|---|
| Charging while iOS runs | PASS | Full 30-minute run stayed connected; the charging indicator was also confirmed by the user on the current RAM boot |
| Battery percentage display | PASS | The current status bar shows a stable percentage and charging state; kernel telemetry remains valid |
| Speaker acoustic output | PASS | Known 1 kHz tone is present in microphone captures only while `UMI SPEAKER TONE NOW` runs; median 1 kHz level rises by 25.78 dB (`docs/official-speaker-mic-loopback.md`) |
| Dual-speaker stereo balance | PARTIAL | Both CS35L41 amplifiers and the protection DSP initialize; independent left/right output and balance are still unverified |
| Vibration | PARTIAL | AW8697 stock RAM playback is clean. The continuous-mode probe repeatedly measured BEMF peaks `+47/-200` and `+48/-200`, matching Android's stock `+45/-202`, so the coil drive/BEMF path is hardware-verified. F0 is advisory because it is unstable on both HakiOS and stock Android. Physical feel and the iOS actuator service remain unverified |
| Bluetooth | PARTIAL; peer-pairing gate pending | Firmware, HCI reset, H4 transport, LE reception and Settings discovery PASS (`logs/204958-run.log`, `logs/230506-run.log`). The `Accessory` is a connectable-undirected advertiser at -99..-105 dBm; it reached `LE Enhanced Connection Complete status=0`, then disconnected with reason `0x3e` before remote feature exchange (`logs/002232-run.log`). A known-good peer close to the phone is required before calling host pairing broken |
| Wi-Fi discovery/startup | PASS | The current canonical run opened Wi-Fi, returned 2.4 GHz and 5 GHz networks, and displayed them in Settings (`logs/052011-run.log`, `logs/wirelessregress-wifi.png`) |
| Wi-Fi power off/on recovery | PASS | The current canonical run switched Wi-Fi off (`set 19`, interface removed), then on again (`set 19`, radio restarted), scanned, associated and retained real network traffic (`logs/052011-run.log`) |
| Wi-Fi association/DHCP | PASS | The current 5 GHz AP completed authentication, association, the four-way handshake and key installation. DHCP assigned `192.168.0.105`, router `192.168.0.1`, and DNS servers `192.168.1.1`/`192.168.0.1` (`logs/022344-run.log`) |
| Wi-Fi TCP/TLS and Safari | PASS | Safari loaded `https://www.apple.com/library/test/success.html` through en0 and displayed `Success`; MobileSafari/WebKit/trustd established multiple TLS connections and transferred about 40 MB (`logs/wifisafari-success.png`) |
| Combined wireless regression | PASS | The current canonical cold boot completed Wi-Fi join/DHCP, Safari, Wi-Fi power off/on, a second association, and Bluetooth Settings discovery while HCI scanning stayed healthy (`logs/052011-run.log`) |
| CPU | PASS | 7 CPUs start and all three EPSS domains raise to 1804/2419/3187 MHz |
| GPU | PASS | Adreno 650 hardware rendering drives SpringBoard without softpipe fallback |
| Touch | PASS | User confirmed touchscreen input on the current RAM boot after `/chosen/hakios-board` was restored; FTS521 polling also reports `errors=0` |
| Brightness adjustment | PARTIAL | DSI `0x51` level 2056 was acknowledged after iOS requested 500 permille; human-visible low/mid/high comparison remains |

## Official-main migration gate

- [x] Register THYME in `devices/manifest.conf`.
- [x] `tools/hakios-device.py check thyme` passes.
- [x] `make thyme_defconfig` and `make config` pass.
- [x] Official-main kernel builds.
- [x] Official-main kernel reaches launchd and SpringBoard on the phone.
- [x] Official-main kernel returns to Android safely.
- [x] Official loader and `xnu-usb.img` boot on the phone.

## Boot and interaction

| Check | Official main | Legacy evidence |
|---|---|---|
| XNU reaches userspace | PASS | PASS |
| 7 CPUs and cpufreq | PASS; three EPSS domains at 1804/2419/3187 MHz | PASS |
| 1080 x 2340 display | PARTIAL: generic framebuffer reaches SpringBoard | PASS |
| Brightness adjustment | PARTIAL: iOS hooks and DCS `0x51` command completion verified | PASS in the legacy display probe |
| Adreno 650 GPU rendering | PASS, more than 2,100 `hakios-gl` frames in four minutes | PASS |
| 30-minute UI stability | PASS; continuous SpringBoard rendering, stable memory and orderly return (`docs/official-ui-soak.md`) | PASS |
| FTS521 touch polling | PASS, 415 events in the GPU run / `errors=0` | PASS |
| Tap, drag, gestures | PASS, backboardd touching/none and gesture handling | PASS |
| Power key | PASS, pressed/released captured | PASS |
| Volume keys | PASS, pressed/released captured | PASS |
| Display off and wake | TODO | PASS |

## Power and charging

| Check | Official main | Legacy evidence |
|---|---|---|
| Battery percentage/voltage/temperature | PASS for telemetry; iOS status bar refresh mismatch remains | PASS |
| Signed current | PASS, `-97 mA` during the probe | PASS |
| Positive current in HakiOS | PASS; observed 1239 mA and 106-140 mA with `charging=1` | PASS |
| One-shot `TakeAction` | TODO | PASS, exactly two calls |
| 30-minute charging endurance | PASS; endpoint online throughout the UI run, 0 failed reads | PASS with endpoint online |
| Net charge under USB load | PARTIAL: SOC stayed 97% over the 30-minute UI run at +129..151 mA; longer net-charge test still required | PARTIAL: load can exceed a 500 mA host port |
| `battery_stop` safety guard | TODO | PASS |
| Thermal frequency caps | PASS; big-core domain 17 -> 16 at 75.9 C, then recovered | PASS |
| `thermal_stop` hand-back | PASS with forced `thermal_stop=30`; GPU stopped in order and loader returned without panic | TODO |
| Automatic Android return after `thermal_stop` | TODO: host must issue `fastboot -s xnuloader reboot` | TODO |

## Audio, wireless, and sensors

| Check | Status | Next action |
|---|---|---|
| ADSP firmware | PASS on official main; APR services up | port and validate official ADSP path |
| Speaker playback | PASS for acoustic output: MI2S, both CS35L41 amps and protection DSP run, and a 1 kHz tone raises the microphone's 1 kHz component by 25.78 dB at the matching time | confirm independent left/right output and stereo balance |
| Microphone capture | PASS: DEC1/ADC1 + DEC0/ADC4 captures dual-channel PCM (`rms 4854-7930`); Darwin AVAudioRecorder through `/dev/hakios-mic` ran 4.9 s with `0 frames skipped` and `0` short reads | keep a tone/intelligibility regression check in future soak runs |
| Haptics | PARTIAL: repeated BEMF peaks match stock, proving continuous drive and back-EMF; F0 is advisory and unstable in both HakiOS and Android; BSTERRI is the vendor-ignored boost latch | confirm physical feel, then test iOS taps and long vibrations from the Actuator HAL |
| QCA6390 PCIe/MHI | PASS on official main; firmware reaches mission mode | port and retest |
| QMI / WLFW service | PASS; QCA6390 firmware `0x1016046e` ready | diagnose any long-run reconnect failures |
| Wi-Fi station / join / DHCP | PASS on the current baseline: WPA2 5 GHz association, four-way handshake, DHCP, DNS, TLS and Safari page rendering (`logs/022344-run.log`) | test 2.4/5 GHz switching, reconnect after link loss, and sleep/wake |
| Bluetooth UART firmware | PASS on official main | `htbtfw20.tlv` and `htnv20.bin` accepted |
| Bluetooth HCI reset | PASS on official main | HCI 0x0c03 completed; iOS command flow followed |
| Scan / pair / connect | PARTIAL: LE scan and classic inquiry PASS, including address/RSSI/name decoding | pair a known LE peer from iOS, reconnect after disabling/enabling Bluetooth, then test data or audio transfer |
| ALS / proximity / accelerometer | FAIL | no usable sensor path |

## User space and daily use

| Check | Status | Evidence / next action |
|---|---|---|
| Darwin launchd and core daemons | PASS in historical RAM runs | launchd starts process 1 and brings up cfprefsd, logd, runningboardd, backboardd, powerd, wifid and other selected system daemons |
| SpringBoard home screen | PASS in historical RAM runs; current fixed build awaits retest | SpringBoard reaches the home screen and renders through the Adreno OpenGL ES path |
| SpringBoard settings and status bar | PARTIAL | Reduce Transparency, Reduce Motion and battery/status-bar hooks exist; battery charging state still needs the current-build RAM retest |
| Settings and application launch | PARTIAL | URL openings, Settings panes and application launch navigation work in scripted runs; broad application compatibility is unverified |
| Wi-Fi Settings UI | PARTIAL | The pinned baseline displays real 2.4 GHz and 5 GHz networks and recovers after off/on. The current join was issued through the same Apple80211 ASSOCIATE request that Settings uses; entering credentials through the Settings keyboard remains unvalidated |
| Bluetooth Settings/pairing UI | PARTIAL | Stock Settings discovered and displayed a connectable `Accessory`. Its signal was only -99..-105 dBm, and it disconnected with reason `0x3e` after link establishment; pairing with a known-good peripheral in close range remains the next gate |
| Notifications, audio routing and background apps | PARTIAL | Safari reaches and renders HTTPS content, Weather/Calendar widgets render, Apple services resolve and establish TLS. Notification delivery and sustained background behavior still need an interactive soak |
| Lock/unlock and passcode | PARTIAL | lock-screen navigation works through injected input; passcode, secure storage and reboot persistence are not validated |
| User data persistence | TODO | current root and private data are RAM-backed; the persistent-storage policy and guarded write path are not accepted |

## Display, storage, and installation

| Check | Status | Evidence / next action |
|---|---|---|
| Native 1080 x 2340 display | PASS in historical runs | loader validates THYME GOP size and SpringBoard renders at the panel resolution |
| Brightness control | PARTIAL | iOS hook and DSI DCS `0x51` completion are verified; human comparison and automatic brightness remain missing |
| Display sleep and wake | TODO | exercise the power path under the current board-gating fix |
| APFS root/system images | PASS in RAM | the small `/redwood` image and full 2.5 GB system image mount in RAM |
| UFS read-only survey | PASS | The rebuilt THYME UEFI exposed 6 UFS LUNs with valid GPT CRCs and enumerated `super`, `userdata`, both boot slots and firmware partitions. The final packaged `prebuilt/xnu-usb.img` repeated this successfully (`logs/043951-run.log`); no write was attempted |
| Storage layout survey | PASS, read-only | Six LUNs and both liblp metadata slots validated. `super` uses 6.25/8.50 GB in slot A with 2.24 GB uncovered; `userdata` is about 111.4 GB and ends near LUN0's end. No free GPT partition exists (`docs/storage-layout.md`) |
| Persistent system/user storage | TODO | no accepted design; preserve Android and do not write partitions yet |
| Safe return to Android | PASS | RAM runs return through the loader; Android remains installed and bootable |
| Standalone installer/recovery | TODO | requires carve, guarded-write, RAM-boot-from-flash, slot-switch and recovery gates from `docs/install-safe.md` |

## Camera, cellular, sensors, and security hardware

| Check | Status | Evidence / next action |
|---|---|---|
| Rear camera capture | FAIL | no working official-main capture path; sensor tables are available |
| Front camera capture | TODO | not exercised |
| Torch/flash | TODO | not yet exercised on the official-main port |
| Cellular modem / SIM | FAIL | MPSS/telephony stack is not implemented and no SIM registration has been verified |
| Mobile data / calls / SMS | FAIL | depends on the cellular stack |
| ALS / auto-brightness | FAIL | no usable ambient-light sensor path; brightness is manual |
| Accelerometer / orientation | FAIL | backboardd reports no usable accelerometer HID service |
| Proximity | TODO | no usable sensor path; lock-screen pocket behavior is unverified |
| NFC | TODO | not ported |
| Fingerprint | TODO | not ported and no secure unlock design exists |

## Later product work

- [x] Integrate the phone's extracted UFSDxe/DiskIoDxe/PartitionDxe into the
  THYME UEFI, expose BlockIo/PartitionInfo, and pass the read-only survey.
- [ ] After that survey, make a separate accepted design for persistent storage.
- [ ] Rear camera and torch.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [ ] Performance and thermal soak under daily use.
- [ ] Battery-cycle integrity and no unexplained percentage jumps.
