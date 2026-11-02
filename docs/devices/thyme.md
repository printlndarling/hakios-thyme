<!-- Made by tools/docs-gen.py from devices/thyme/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi Mi 10S (`thyme`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sm8250` |
| Configuration | `make thyme_defconfig` |
| Maintainer | printlndarling |
| Repository | [`devices/thyme/`](https://github.com/printlndarling/hakios-thyme/tree/main/devices/thyme) |

## What works

| Part | State | |
|---|---|---|
| display | partly | framebuffer, Adreno and DSI DCS 0x51 paths work; current board-gating fix awaits a RAM retest and physical brightness comparison is pending |
| touch | partly | ST FTS521 at I2C 0x49, IRQ GPIO39, reset GPIO38; historically verified, current board-gating fix awaits a RAM retest |
| buttons | partly | PM8150 PON + GPIO6; historically verified, current board-gating fix awaits a RAM retest |
| cpu | works | 7 CPUs; three EPSS domains raised to 1804/2419/3187 MHz on official main |
| gpu | works | Adreno 650 / zap 0x8c41a000, hardware-rendered SpringBoard |
| battery | partly | kernel telemetry and charging work; SpringBoard battery/charging refresh is implemented but awaits hardware validation |
| storage | no | read-only UFS survey not run in the official-main port |
| audio | partly | DSP/MI2S/CS35L41 and WCD938x dual-mic capture work; speaker-to-mic acoustic loopback verified, stereo balance pending |
| haptic | partly | 1700 Hz AW8697 playback and repeatable BEMF response verified; F0 advisory unstable, physical feel pending |
| camera | no | sensor tables collected; no working capture yet |
| torch | no | not yet tested on THYME |
| wifi | partly | QCA6390 scan, WPA2 association, DHCP and TCP through en0 verified historically; current RAM regression fix awaits retest |
| bluetooth | partly | firmware, iOS H4 and physical LE scan work; pairing and reconnect pending |
| cellular | no | modem and telephony path not implemented |
| network | partly | en0 DHCP and TCP traffic verified historically; current RAM regression fix awaits retest |
| thermal | works | 27 TSENS inputs, live throttling, and orderly thermal_stop hand-back verified; soak pending |
| usb | partly | xnuloader fastboot endpoint works; one endpoint loss observed |
| sensors | no | ALS, accelerometer and proximity are not usable |

The state is what has been seen on this device, written down in `devices/thyme/device.conf`; a part the table does not name has not been looked at.
