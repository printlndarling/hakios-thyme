<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | haptic | camera | torch | wifi | bluetooth | cellular | network | thermal | usb | sensors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes |  | no | yes | yes |  |  |  |  |  |  |  | yes |  |  |  |
| [POCO X5 Pro 5G](redwood.md) | `sc7280` | yes | yes | yes | yes | yes | yes | yes | partly | yes | yes | partly | yes | no | partly |  | partly |  |  |
| [Xiaomi Mi 10S](thyme.md) | `sm8250` | yes | yes | yes | partly | yes | no | partly | yes | no | no | no | yes | partly | no | yes | partly | partly | no |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `sc7280` | Qualcomm SC7280 family (SM7325 "Snapdragon 778G", QCM6490; "yupik", "kodiak") | carried by redwood (`devices/redwood/soc/`) |
| `sm8250` | Qualcomm SM8250 (Snapdragon 865/870) | carried by thyme (`devices/thyme/soc/`) |
