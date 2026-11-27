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
| Camera & Torch | **PARTIAL** | Rear S5KHMX and front S5K3T2 produce real frames, with AK7377 AF and PM8150L flash. Native preview pacing, switching latency, and front preview quality remain [under test](devices/thyme/CHECKLIST.md#camera-photos-and-display). |
| Cellular & VoLTE | **PASS** | SDX55M (`pcie2_mhi=2` native `AppleBaseband`/`CommCenter`), 4G LTE dual-stack + VoLTE (`Mav17`) |
| Location & GNSS | **PARTIAL** | Settings toggle, Wi-Fi fixes (`BaiduMap`) & SDX55M QMI LOC SV active; outdoor 3D fix pending |
| Wi-Fi (QCA6390) | **PASS** | 2.4/5 GHz WPA2, DHCP/TLS, locale (`CN`), 65 s lock/wake, AP deauth & power auto-rejoin |
| Bluetooth (QCA6390) | **PASS** | 3 Mbaud H4, discovery, pairing with other phones, headphone connection, A2DP audio & BlueTool deepsleep |
| Battery & Thermal | **PASS** | Fuel gauge, `bq25970` & `fsa4480` 10 s I2C telemetry, USB charging, thermal cap & `battery_stop` guard |
| Sensors (SLPI) | **PASS** | LSM6DSO, `mag_cal` 50 Hz AK0991x compass, front `TCS3701` + rear `TSL2540` ALS, `BMP285` baro, pedometer, orient |
| Hexagon CDSP / IR / RTC / WDT | **PASS** | `CDSP` TrustZone boot/shutdown, SMP2P ready, GLINK negotiation, a FastRPC capability query, guest-session attach/release, and stock compute-process create/release, an in-process ASID query and reverse-listener registration, `/dev/hakios-ir` (38 kHz NEC), SPMI RTC Unix sync, TRNG, active APSS WDT |
| NFC (`nq-nci`) & VPU (`venus`) | **PARTIAL** | `/dev/hakios-nfc` NCI 2.0 handshake ready; Venus firmware preparation and reversible MVS0C core and AXI0 gate enable verified; hardware decode pending |
| Localization & UI | **PASS** | Boots directly in Simplified Chinese with PingFang fonts and Pinyin keyboard |
| Baidu Maps | **PARTIAL** | Roads and labels verified on thyme with the [app-only Metal profile](devices/thyme/docs/ram-boot.md#baidu-maps-app-metal-comparison). Use the launcher’s `--metal-apps` option with a compatible bundle to enable this profile. |
| Sileo & TrollStore | **PASS** | Sileo (11 CN/rootless repos, no Canister nag), TrollStore (`ldid` ready), TrollApps (6 CN sources) |
| Persistent Storage | **TODO** | RAM-boot only by default (`modem_a` read-only VFAT OK); guarded writable UFS storage pending |

## Quick Start

Start HakiOS over USB from Windows, Linux, or macOS. **HakiOS runs in RAM. Restart the phone to return to Android.**

You need an unlocked Xiaomi Mi 10S (`thyme`) running Android with USB debugging enabled and authorized, a data cable, at least 40% charge, and a battery temperature below 45 °C. Install Python 3.8 or later and [download and extract the repository ZIP](https://github.com/printlndarling/hakios-thyme/archive/refs/heads/main.zip) or clone the repository.

Put the four images from the same build in a directory named `thyme-bundle` inside the repository. The required files are `xnu-usb.img`, `hakios-kernel`, `root.apfs`, and `system.apfs`. Build the loader with the current `devices/thyme/build-loader.sh` so diagnostics stay in RAM. The launcher downloads [Google Android Platform-Tools](https://developer.android.com/tools/releases/platform-tools) to a local cache if they are missing.

### Windows

Double-click `devices\thyme\ram-boot.bat` or run it from PowerShell. Install [Python for Windows](https://www.python.org/downloads/windows/) if needed.

```powershell
.\devices\thyme\ram-boot.bat
```

If Android is visible but bootloader fastboot disappears, check the Windows USB driver and cable.

### Linux

```sh
sh devices/thyme/ram-boot.sh
```

If USB access is denied, install your distribution's Android/Fastboot udev rules.

### macOS

```sh
sh devices/thyme/ram-boot.sh
```

The launcher uses native macOS Platform-Tools. Keep the cable connected during upload.

### Download the images on first use

Add `--bundle-url https://your-host/thyme-ram-boot.json` to the launch command to download the images from a trusted manifest. The launcher verifies every download against its SHA-256 and reuses verified cached files. See the [manifest format](devices/thyme/docs/ram-boot.md).

A public full-image bundle is currently unavailable. Build the images from your own IPSW and device firmware, or use a private HTTPS bundle you are authorized to access.

### Prepare images on Linux

Set up the build dependencies and collect the local firmware and UEFI inputs in the [device documentation](devices/thyme/README.md). Native Linux, a Linux VM, or WSL2 can build the bundle. The launchers use the resulting images on any of the three hosts.

```sh
make doctor
make thyme_defconfig
make unpack IPSW=/path/to/iPhone10,3_16.5_Restore.ipsw
make kernel JOBS=8
SYSTEM=1 SCENE=daily devices/thyme/build-root.sh
devices/thyme/build-loader.sh
```

Add `--check` to validate the loader and payload size before connecting the phone. The development [soak script](devices/thyme/docs/ram-boot.md#timed-daily-scene-on-a-development-host) records timed return and Android recovery. For build output paths, manifest details, and recovery from an interrupted boot, see the [cross-platform launcher guide](devices/thyme/docs/ram-boot.md).

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
