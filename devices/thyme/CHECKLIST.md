# THYME daily-driver checklist

Status meanings: `PASS` is observed on the physical Mi 10S in the named build;
`PARTIAL` is implemented but incomplete; `FAIL` is a reproduced failure;
`TODO` has not been tested.

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
| 7 CPUs and cpufreq | PARTIAL: 7 CPUs start; cpufreq driver still off | PASS |
| 1080 x 2340 display | PARTIAL: generic framebuffer reaches SpringBoard | PASS |
| Adreno 650 GPU rendering | TODO | PASS |
| 30-minute UI stability | TODO | PASS |
| FTS521 touch polling | TODO | PASS |
| Tap, drag, gestures | TODO | PASS |
| Power key | TODO | PASS |
| Volume keys | TODO | PASS |
| Display off and wake | TODO | PASS |

## Power and charging

| Check | Official main | Legacy evidence |
|---|---|---|
| Battery percentage/voltage/temperature | PASS | PASS |
| Signed current | PASS, `-97 mA` during the probe | PASS |
| Positive current in HakiOS | TODO | PASS |
| One-shot `TakeAction` | TODO | PASS, exactly two calls |
| 30-minute charging endurance | TODO | PASS with endpoint online |
| Net charge under USB load | TODO | PARTIAL: load can exceed a 500 mA host port |
| `battery_stop` safety guard | TODO | PASS |
| Thermal stop | TODO | TODO |

## Audio, wireless, and sensors

| Check | Status | Next action |
|---|---|---|
| ADSP firmware | TODO | port and validate official ADSP path |
| Speaker playback | TODO | AW882xx path and physical output |
| Microphone capture | TODO | capture and playback test |
| Haptics | FAIL | resolve AW8697 UVLO / enable path |
| QCA6390 PCIe/MHI | PARTIAL | port and retest |
| WLFW QMI | FAIL | diagnose previous timeout `-60` |
| Wi-Fi station / join / DHCP | TODO | depends on WLFW |
| Bluetooth UART firmware | TODO | load `htbtfw20.tlv` and `htnv20.bin` |
| Bluetooth HCI reset | FAIL | no usable HCI controller yet |
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
