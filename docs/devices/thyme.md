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
| display | works | framebuffer, Adreno and DSI DCS 0x51 backlight paths work; physical brightness comparison pending |
| touch | works | ST FTS521 at I2C 0x49, IRQ GPIO39, reset GPIO38 |
| buttons | works | PM8150 PON + GPIO6; power and both volume keys verified |
| cpu | works | 7 CPUs; three EPSS domains raised to 1804/2419/3187 MHz on official main |
| gpu | works | Adreno 650 / zap 0x8c41a000, hardware-rendered SpringBoard |
| battery | works | percentage, signed current and temperature from the loader cache |
| storage | no | read-only UFS survey not run in the official-main port |
| audio | partly | DSP, MI2S and both CS35L41 amps work; audible output pending |
| haptic | no | AW8697 UVLO path still fails |
| camera | no | sensor tables collected; no working capture yet |
| torch | no | not yet tested on THYME |
| wifi | works | QCA6390 scan, WPA2 association, DHCP and TCP through en0 |
| bluetooth | partly | firmware, HCI reset and iOS H4 work; pairing test pending |
| cellular | no | modem and telephony path not implemented |
| network | works | en0 DHCP and TCP traffic verified |
| thermal | works | 27 TSENS inputs, live throttling, and orderly thermal_stop hand-back verified; soak pending |
| usb | partly | xnuloader fastboot endpoint works; one endpoint loss observed |
| sensors | no | ALS, accelerometer and proximity are not usable |

The state is what has been seen on this device, written down in `devices/thyme/device.conf`; a part the table does not name has not been looked at.
