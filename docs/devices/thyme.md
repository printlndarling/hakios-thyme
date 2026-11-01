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
| display | partly | legacy RAM tree reaches SpringBoard; official-main port pending |
| touch | partly | ST FTS521 at I2C 0x49, IRQ GPIO39, reset GPIO38; legacy tree verified |
| buttons | partly | PM8150 PON + GPIO6; power and both volume keys verified in legacy tree |
| cpu | partly | 7 CPUs and cpufreq verified in legacy tree; official-main port pending |
| gpu | partly | Adreno 650 / zap 0x8c41a000 verified in legacy tree; official-main port pending |
| battery | partly | percentage, current and temperature verified in legacy tree |
| storage | no | read-only UFS survey not run in the official-main port |
| audio | no | ADSP/speaker/microphone not yet ported to official main |
| haptic | no | AW8697 UVLO path still fails |
| camera | no | sensor tables collected; no working capture yet |
| torch | no | not yet tested on THYME |
| wifi | partly | QCA6390 PCIe/MHI starts; WLFW QMI times out |
| bluetooth | no | UART/pins identified; no HCI controller yet |
| cellular | no | modem and telephony path not implemented |
| network | no | no usable station/interface yet |
| thermal | partly | thermal driver exists in legacy tree; soak test pending |
| usb | partly | xnuloader fastboot endpoint works; one endpoint loss observed |
| sensors | no | ALS, accelerometer and proximity are not usable |

The state is what has been seen on this device, written down in `devices/thyme/device.conf`; a part the table does not name has not been looked at.
