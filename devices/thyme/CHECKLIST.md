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

## User-priority hardware gate

These are the minimum user-facing hardware checks for the current THYME port.
No item moves to `PASS` from source inspection alone.

| Item | Status | Evidence / next action |
|---|---|---|
| Charging while iOS runs | PASS | Full 30-minute SpringBoard run stayed connected: `charger=1 charging=1`, +129..151 mA, 349 battery reads with 0 failures |
| Battery percentage display | FAIL, fixed source awaits RAM retest | Kernel telemetry is valid, but the current run had no `IOPMPowerSource`; commit `66500d7` feeds the status-bar aggregator from the kernel's battery sysctls and requests a refresh every five seconds |
| Speaker acoustic output | PASS | Known 1 kHz tone is present in microphone captures only while `UMI SPEAKER TONE NOW` runs; median 1 kHz level rises by 25.78 dB (`docs/official-speaker-mic-loopback.md`) |
| Dual-speaker stereo balance | PARTIAL | Both CS35L41 amplifiers and the protection DSP initialize; independent left/right output and balance are still unverified |
| Vibration | PARTIAL | AW8697 stock RAM playback is clean. The continuous-mode probe repeatedly measured BEMF peaks `+47/-200` and `+48/-200`, matching Android's stock `+45/-202`, so the coil drive/BEMF path is hardware-verified. F0 is advisory because it is unstable on both HakiOS and stock Android. Physical feel and the iOS actuator service remain unverified |
| Bluetooth | PARTIAL; current UI flow failed | Earlier runs prove QCA6390 firmware, HCI reset, MAC publication, iOS H4, a 9-advertiser LE scan and classic inquiry. The current RAM run had no usable pairing UI; board gating fix needs a retest before pairing/reconnect work continues |
| Wi-Fi | PARTIAL; current RAM boot failed | Earlier runs prove QCA6390 scan, WPA2 association, DHCP and en0 TCP traffic. The current run disabled the THYME QCA6390 path through the missing board property, and `wifi=1` fell through to the legacy WPSS request |
| CPU | PASS | 7 CPUs start and all three EPSS domains raise to 1804/2419/3187 MHz |
| GPU | PASS | Adreno 650 hardware rendering drives SpringBoard without softpipe fallback |
| Touch | FAIL, fixed source awaits RAM retest | The current run logged `UMI FTS: board mismatch`; earlier builds produced FTS521 DOWN/MOVE/UP events with `errors=0` |
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
| Wi-Fi station / join / DHCP | PASS; WPA2 association, DHCP and en0 TCP traffic | test 2.4/5 GHz and reconnect |
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
| Wi-Fi Settings UI | PARTIAL | wifid and the Apple80211 surface start, but joining/managing networks from the iOS UI has not been validated end to end |
| Bluetooth Settings/pairing UI | FAIL | firmware/HCI/inquiry work, but no LE peer has been paired and reconnected from the iOS UI |
| Notifications, audio routing and background apps | TODO | requires a sustained interactive session with representative applications |
| Lock/unlock and passcode | PARTIAL | lock-screen navigation works through injected input; passcode, secure storage and reboot persistence are not validated |
| User data persistence | TODO | current root and private data are RAM-backed; the persistent-storage policy and guarded write path are not accepted |

## Display, storage, and installation

| Check | Status | Evidence / next action |
|---|---|---|
| Native 1080 x 2340 display | PASS in historical runs | loader validates THYME GOP size and SpringBoard renders at the panel resolution |
| Brightness control | PARTIAL | iOS hook and DSI DCS `0x51` completion are verified; human comparison and automatic brightness remain missing |
| Display sleep and wake | TODO | exercise the power path under the current board-gating fix |
| APFS root/system images | PASS in RAM | the small `/redwood` image and full 2.5 GB system image mount in RAM |
| UFS read-only survey | TODO | run the official-main read-only survey before considering any write path |
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

- [ ] UFS read-only survey, then a separate accepted design for persistent
  storage.
- [ ] Rear camera and torch.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [ ] Performance and thermal soak under daily use.
- [ ] Battery-cycle integrity and no unexplained percentage jumps.
