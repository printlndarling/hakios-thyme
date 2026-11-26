# HakiOS on Xiaomi Mi 10S (`thyme`)

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Device fork of [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) for the Xiaomi Mi 10S (`thyme`, Snapdragon 870 / SM8250-AC).**
Boots Apple's open-source Darwin/XNU kernel (`xnu-8796.121.2`) directly from RAM without modifying Android or user partitions, keeping verified `thyme` hardware drivers alongside selectively merged upstream improvements.

> Every `PASS` below is verified on a physical Xiaomi Mi 10S. Full test logs, build hashes, and technical notes live in [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md).

## Target Hardware

| Component | Specification |
|---|---|
| SoC / GPU | Qualcomm Snapdragon 870 (`SM8250-AC`) · Adreno 650 |
| Modem / GNSS | External Qualcomm Snapdragon X55 (`SDX55M`) over PCIe2 / MHI |
| Display / Touch | 6.67" 1080 × 2340 90 Hz AMOLED · FocalTech FTS521 |
| Wireless | Qualcomm QCA6390 (Wi-Fi 6 + Bluetooth 5.1) |
| Audio / Haptics | Dual Cirrus Logic CS35L41 stereo amps · AW8697 LRA |
| Cameras | Rear 108 MP Samsung S5KHMX + AK7377 VCM AF · Front 20 MP S5K3T2 |

## Hardware & Feature Status

| Subsystem | Status | Summary |
|---|---|---|
| Boot & SpringBoard | **PASS** | Cold RAM boot to `launchd`, `backboardd`, SpringBoard, and Settings |
| CPU / DVFS | **PASS** | 7 cores active; EPSS scaling up to 1.80 / 2.42 / 3.19 GHz |
| Display & Refresh | **PASS** | 1080 × 2340 OLED, global 90 Hz (`fb_hz=90`), DCS brightness & lock blanking |
| GPU (OpenGL) | **PASS** | Hardware Adreno 650 rendering via Mesa/Freedreno (`hakios-gl`) |
| Metal | **PARTIAL** | `hakios-metal` builds; system `MTLDevice` / VectorKit / ANGLE apps pending |
| Touch & Buttons | **PASS** | FTS521 multi-touch, gestures, power & volume keys |
| Audio & Haptics | **PASS** | Dual CS35L41 stereo L/R speakers, dual mics, AW8697 CoreHaptics |
| Camera & Torch | **PASS** | Rear 27 MP S5KHMX + AK7377 AF, front S5K3T2, lens switching, PM8150L flash |
| Cellular & VoLTE | **PASS** | SDX55M SIM, Settings UI, 4G LTE dual-stack data, two-way VoLTE voice (SMS deferred) |
| Location & GNSS | **PARTIAL** | Settings UI/toggle, Wi-Fi fixes & SDX55M QMI LOC 1 Hz SV active; outdoor 3D fix pending |
| Wi-Fi (QCA6390) | **PARTIAL** | 2.4/5 GHz WPA2, DHCP/TLS & power recovery; `SystemSleep` auto-wake open |
| Bluetooth (QCA6390) | **PARTIAL** | 3 Mbaud H4, discovery, pairing & A2DP playback; HFP/AVRCP/reconnect open |
| Battery & Thermal | **PASS** | Fuel gauge, USB charging telemetry, thermal throttling & low-battery guard |
| Sensors (SLPI) | **PARTIAL** | SLPI firmware & QRTR services up; accelerometer/ALS/proximity HID delivery open |
| Localization & UI | **PASS** | Boots directly in Simplified Chinese with PingFang fonts & Pinyin keyboard |
| Package Manager | **PARTIAL** | Rootless Procursus `apt`/`dpkg` online installs work; Sileo UI queue open |
| Persistent Storage | **TODO** | RAM-boot only by default; guarded flash storage & installer not enabled |

## Current Priorities

- **Sensors (SLPI / HID)**: Deliver accelerometer, ambient-light (auto-brightness), and proximity events to iOS.
- **Outdoor GNSS & Cellular Polish**: Verify open-sky 3D satellite fixes via `umi_gnssd` and SMS when needed.
- **Wireless Power & Audio**: Wi-Fi auto-reconnect after `SystemSleep`; Bluetooth HFP/AVRCP/reconnect.
- **Graphics & Persistence**: System `MTLDevice` support for third-party apps; safe, opt-in persistent storage.

## Building & Running (RAM Boot)

Development runs in **WSL2 Ubuntu** on native ext4 (Windows is used only for USB `adb`/`fastboot`). Normal testing never flashes or modifies phone partitions and automatically returns the phone to Android.

```sh
make thyme_defconfig
make kernel JOBS=16
SCENE=wirelessfull devices/thyme/build-root.sh
devices/thyme/ram-regress.sh
```

- **Firmware & IPSW setup**: Vendor firmware and Darwin userland are local and Git-ignored — see [`devices/thyme/README.md`](devices/thyme/README.md).
- **Cellular / GNSS boots**: Use `XL_BOOT_IMAGE=devices/thyme/prebuilt/xnu-usb.img` (1023-char cmdline with `/baseband` DT node).
- **Detailed evidence & baselines**: See [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md) and [`devices/thyme/ram-baseline.conf`](devices/thyme/ram-baseline.conf).

## Upstream & Related Repositories

This fork tracks [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) for shared kernel, loader, userland, and tooling changes while isolating Mi 10S work under `devices/thyme/`.

| Repository | Description |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | Upstream common kernel port, drivers, loader, userland, and tools |
| [`hakios-metal/`](hakios-metal) | Metal command layer on Adreno via the kernel GPU driver |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G (SC7280) device port |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | Mu-Silicium UEFI and U-Boot bootloaders for HakiOS |
| [hackios-web](https://github.com/oosexgang/hackios-web) | Project website |

## Legal & Credits

- **No Apple or vendor blobs** are distributed in this repository; users supply their own IPSW and extract firmware from their own device. Apple services (Activation, Apple ID, iCloud, App Store / FairPlay DRM) are not supported.
- **Licenses**: XNU patches and derivatives are licensed under [APSL 2.0](LICENSE.APSL); original code is [MIT](LICENSE). See [NOTICE](NOTICE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md), and [LEGAL.md](LEGAL.md).
- **Disclaimer**: Independent research project, **not affiliated with or endorsed by Apple**.
- **Thanks**: Upstream HakiOS maintainers and the open-source Qualcomm/SLPI/QMI reverse-engineering community.
