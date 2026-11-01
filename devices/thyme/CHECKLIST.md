# THYME daily-driver checklist

Status meanings: `PASS` is observed on the physical Mi 10S in the named build;
`PARTIAL` is implemented but incomplete; `FAIL` is a reproduced failure;
`TODO` has not been tested.

## User-priority hardware gate

These are the minimum user-facing hardware checks for the current THYME port.
No item moves to `PASS` from source inspection alone.

| Item | Status | Evidence / next action |
|---|---|---|
| Charging while iOS runs | PARTIAL | Positive current observed at 1239 mA and 106-140 mA with `charging=1`; 30-minute charging and reconnect endurance still required |
| Battery percentage display | PASS | Loader and iOS report percentage, voltage, current and temperature |
| Dual speakers | PARTIAL | Both CS35L41 amplifiers and the protection DSP initialize; audible left/right playback is still unverified |
| Vibration | FAIL | AW8697 is identified and firmware loads, but active mode still reports UVLO |
| Bluetooth | PARTIAL | QCA6390 firmware, HCI reset, MAC publication and the iOS H4 endpoint work; scan, pair and reconnect are pending |
| Wi-Fi | PASS | QCA6390 scans, joins WPA2, completes DHCP and carries TCP traffic through en0; long-run reconnect is pending |
| CPU | PASS | 7 CPUs start and all three EPSS domains raise to 1804/2419/3187 MHz |
| GPU | PASS | Adreno 650 hardware rendering drives SpringBoard without softpipe fallback |
| Touch | PASS | FTS521 DOWN/MOVE/UP events reach backboardd and SpringBoard with `errors=0` |
| Brightness adjustment | PARTIAL | DSI `0x51` level 2056 was acknowledged after iOS requested 500 permille; human-visible low/mid/high comparison remains |

## Official-main migration gate

- [x] Register THYME in `devices/manifest.conf`.
- [x] `tools/hakios-device.py check thyme` passes.
- [x] `make thyme_defconfig` and `make config` pass.
- [x] Official-main kernel builds.
- [x] Official-main kernel reaches launchd and SpringBoard on the phone.
- [x] Official-main kernel returns to Android safely.
- [ ] UEFI and loader are rebuilt from official infrastructure.

## Boot and interaction

| Check | Official main | Legacy evidence |
|---|---|---|
| XNU reaches userspace | PASS | PASS |
| 7 CPUs and cpufreq | PASS; three EPSS domains at 1804/2419/3187 MHz | PASS |
| 1080 x 2340 display | PARTIAL: generic framebuffer reaches SpringBoard | PASS |
| Brightness adjustment | PARTIAL: iOS hooks and DCS `0x51` command completion verified | PASS in the legacy display probe |
| Adreno 650 GPU rendering | PASS, more than 2,100 `hakios-gl` frames in four minutes | PASS |
| 30-minute UI stability | TODO | PASS |
| FTS521 touch polling | PASS, 415 events in the GPU run / `errors=0` | PASS |
| Tap, drag, gestures | PASS, backboardd touching/none and gesture handling | PASS |
| Power key | PASS, pressed/released captured | PASS |
| Volume keys | PASS, pressed/released captured | PASS |
| Display off and wake | TODO | PASS |

## Power and charging

| Check | Official main | Legacy evidence |
|---|---|---|
| Battery percentage/voltage/temperature | PASS | PASS |
| Signed current | PASS, `-97 mA` during the probe | PASS |
| Positive current in HakiOS | PASS; observed 1239 mA and 106-140 mA with `charging=1` | PASS |
| One-shot `TakeAction` | TODO | PASS, exactly two calls |
| 30-minute charging endurance | TODO | PASS with endpoint online |
| Net charge under USB load | TODO | PARTIAL: load can exceed a 500 mA host port |
| `battery_stop` safety guard | TODO | PASS |
| Thermal frequency caps | PASS; big-core domain 17 -> 16 at 75.9 C, then recovered | PASS |
| `thermal_stop` hand-back | PASS with forced `thermal_stop=30`; GPU stopped in order and loader returned without panic | TODO |
| Automatic Android return after `thermal_stop` | TODO: host must issue `fastboot -s xnuloader reboot` | TODO |

## Audio, wireless, and sensors

| Check | Status | Next action |
|---|---|---|
| ADSP firmware | PASS on official main; APR services up | port and validate official ADSP path |
| Speaker playback | PARTIAL: MI2S runs, both CS35L41 amps and protection DSP verified; audible output pending | CS35L41 path existed from the legacy tree |
| Microphone capture | PARTIAL: path linked to `/dev/hakios-mic`; physical capture pending | capture and playback test |
| Haptics | FAIL: AW8697 identified and waveforms loaded, SYSST still reports UVLO | resolve AW8697 UVLO / enable path |
| QCA6390 PCIe/MHI | PASS on official main; firmware reaches mission mode | port and retest |
| QMI / WLFW service | PASS; QCA6390 firmware `0x1016046e` ready | diagnose any long-run reconnect failures |
| Wi-Fi station / join / DHCP | PASS; WPA2 association, DHCP and en0 TCP traffic | test 2.4/5 GHz and reconnect |
| Bluetooth UART firmware | PASS on official main | `htbtfw20.tlv` and `htnv20.bin` accepted |
| Bluetooth HCI reset | PASS on official main | HCI 0x0c03 completed; iOS command flow followed |
| Scan / pair / connect | TODO | controller is initialized; physical peer test pending |
| ALS / proximity / accelerometer | FAIL | no usable sensor path |

## Later product work

- [ ] UFS read-only survey, then a separate accepted design for persistent
  storage.
- [ ] Rear camera and torch.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [ ] Performance and thermal soak under daily use.
- [ ] Battery-cycle integrity and no unexplained percentage jumps.
