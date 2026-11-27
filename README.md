# HakiOS on Xiaomi Mi 10S (`thyme`)

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Device fork of [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) for the Xiaomi Mi 10S (`thyme`, Snapdragon 870 / SM8250-AC).**
Boots Apple's open-source Darwin/XNU kernel (`xnu-8796.121.2`) directly from RAM without modifying Android or user partitions, keeping verified `thyme` hardware drivers alongside selectively merged upstream improvements.

> Every `PASS` below is verified on a physical Xiaomi Mi 10S. Full checklist lives in [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md).

## Hardware & Feature Status

| Subsystem | Status | Summary |
|---|---|---|
| Boot & SpringBoard | **PASS** | Detached daily RAM boot (`SCENE=daily`) to `launchd`, `backboardd`, SpringBoard, and Settings |
| CPU & Runtime DVFS | **PASS** | 7 cores active; 200 ms EPSS scaling (up to 1.80 / 2.42 / 3.19 GHz) + thermal cap |
| Display & Refresh | **PASS** | 1080 × 2340 OLED, hardware 90 Hz (`INTF_1` TE-synced, 0 missed scans), DCS brightness & lock blanking |
| GPU (OpenGL) & DVFS | **PASS** | Adreno 650 Mesa/Freedreno HW rendering + GMU HFI runtime DVFS (305–587 MHz) |
| Metal | **PARTIAL** | `hakios-metal` builds (`metal_apps=1` compositor fix); system `MTLDevice` stability pending |
| Touch & Buttons | **PASS** | FTS521 multi-touch, gestures, power & volume keys |
| Audio & Haptics | **PASS** | Dual CS35L41 L/R + earpiece, 0-short-read mics (VoiceMemos), 15 s retry recovery, AW8697 CoreHaptics |
| Camera & Torch | **PASS** | 1080p ~51 fps S5KHMX + AK7377 AF, front S5K3T2, PM8150L flash (CCI `0x1c` OIS readback verified) |
| Cellular & VoLTE | **PASS** | SDX55M (`pcie2_mhi=2` native `AppleBaseband`/`CommCenter`), 4G LTE dual-stack + VoLTE (`Mav17`) |
| Location & GNSS | **PARTIAL** | Settings toggle, Wi-Fi fixes (`BaiduMap`) & SDX55M QMI LOC SV active; outdoor 3D fix pending |
| Wi-Fi (QCA6390) | **PASS** | 2.4/5 GHz WPA2, DHCP/TLS, locale (`CN`), 65 s lock/wake, AP deauth & power auto-rejoin |
| Bluetooth (QCA6390) | **PASS** | 3 Mbaud H4, discovery, pairing with other phones, headphone connection, A2DP audio & BlueTool deepsleep |
| Battery & Thermal | **PASS** | Fuel gauge, `bq25970` & `fsa4480` 10 s I2C telemetry, USB charging, thermal cap & `battery_stop` guard |
| Sensors (SLPI) | **PASS** | LSM6DSO, `mag_cal` 50 Hz AK0991x compass, front `TCS3701` + rear `TSL2540` ALS, `BMP285` baro, pedometer, orient |
| Hexagon CDSP / IR / RTC / WDT | **PASS** | `CDSP` TrustZone PIL-TZ boot/shutdown, `/dev/hakios-ir` (38 kHz NEC), SPMI RTC Unix sync, TRNG, active APSS WDT |
| NFC (`nq-nci`) & VPU (`venus`) | **PARTIAL** | `/dev/hakios-nfc` NCI 2.0 handshake ready; UFS `modem_a` VFAT loader + `venus` TZ `INIT_IMAGE`/`MEM_SETUP` verified |
| Localization & UI | **PASS** | Boots directly in Simplified Chinese with PingFang fonts & Pinyin keyboard (`BaiduMap` staged) |
| Sileo & TrollStore | **PASS** | Sileo (11 CN/rootless repos, no Canister nag), TrollStore (`ldid` ready), TrollApps (6 CN sources) |
| Persistent Storage | **TODO** | RAM-boot only by default (`modem_a` read-only VFAT OK); guarded writable UFS storage pending |

## Quick Start

Everything runs **strictly from RAM** — Android and user partitions are never flashed or modified. Rebooting returns cleanly to Android.

### Prerequisites

- **Phone**: Xiaomi Mi 10S (`thyme`) with unlocked bootloader, USB debugging enabled, and battery **≥ 40%**.
- **Host**: Linux or **WSL2 Ubuntu** on native ext4 (Windows is only used for `adb`/`fastboot` USB passthrough).
- **Local assets (not committed to Git)**:
  - Your own `iPhone10,3` iOS 16.5 IPSW (for Darwin userland).
  - Vendor firmware (`fw-*`) extracted from your own Mi 10S — see [`devices/thyme/README.md`](devices/thyme/README.md).
  - Optional: Wi-Fi credentials in `fw-wifi/network` (two lines: SSID and passphrase).

### 1. Clone & Configure

```sh
git clone --recursive https://github.com/printlndarling/hakios-thyme.git
cd hakios-thyme
make doctor
make thyme_defconfig
```

### 2. Unpack Userland & Build Latest Stable Images

```sh
# 1) Unpack your iPhone10,3 iOS 16.5 IPSW into sysimage/ (once)
make unpack IPSW=/path/to/iPhone10,3_16.5_Restore.ipsw

# 2) Build the XNU kernel (hakios-kernel)
make kernel JOBS=$(nproc)

# 3) Build the full resident system image (userland/system.apfs) + daily RAM rootfs (userland/root.apfs)
SYSTEM=1 SCENE=daily devices/thyme/build-root.sh
# (Subsequent rootfs-only rebuilds only need: SCENE=daily devices/thyme/build-root.sh)
```

### 3. RAM-Boot on Mi 10S

```sh
export DEVICE=thyme
export XL_BOOT_IMAGE=devices/thyme/prebuilt/xnu-usb.img
export XL_RAMDISK=userland/root.apfs
export XL_CMDLINE="-v debug=0x8 rd=md0 $(tr -d '\n' < devices/thyme/ram.args) pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=2 pcie2_nr=0"

# Enter the RAM UEFI loader (non-destructive fastboot boot)
./xl enter

# Upload userland/system.apfs into RAM once (~2 min; stays resident across kernel runs)
./xl ramdisk userland/system.apfs

# Option A — Daily interactive boot (detaches after launch; you can unplug USB)
XL_SECS=0 XL_DETACH=1 ./xl run

# Option B — Timed verification boot (collects logs/<time>-run.log & reboots back to Android)
# XL_SECS=120 XL_REBOOT_AFTER=1 ./xl run
```

> **Tip**: Check [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md) for current hardware status and [`devices/thyme/ram-baseline.conf`](devices/thyme/ram-baseline.conf) for regression baselines. To return to Android at any time, run `./xl reboot` or force-reboot the phone.

## Current Priorities

- **Outdoor GNSS & Cellular Polish**: Verify open-sky 3D satellite fixes via `umi_gnssd` and SMS when needed.
- **VPU Decode & USB Gadget**: Complete `pil-venus` HFI hardware video decode and SM8250 `DWC3` USB gadget (`usbmux`/`lockdownd`) for PC tools (i4Tools/iTunes).
- **Graphics & Persistence**: System `MTLDevice` support for third-party apps; safe, opt-in persistent storage.

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

- **No Apple or vendor blobs** are distributed in this repository; users supply their own IPSW and extract firmware from their own device. See the [Apple services compatibility review](devices/thyme/UPSTREAM.md#apple-services-compatibility-review) for the published upstream experiments and their verification status on thyme.
- **Licenses**: XNU patches and derivatives are licensed under [APSL 2.0](LICENSE.APSL); original code is [MIT](LICENSE). See [NOTICE](NOTICE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md), and [LEGAL.md](LEGAL.md).
- **Disclaimer**: Independent research project, **not affiliated with or endorsed by Apple**.
- **Thanks**: Upstream HakiOS maintainers and the open-source Qualcomm/SLPI/QMI reverse-engineering community.
