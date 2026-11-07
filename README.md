# HakiOS on the Xiaomi Mi 10S (thyme)

English · [中文](README.zh.md) · [Українська](README.uk.md)

**This is the THYME device fork of [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel).**
It runs Apple's open-source Darwin kernel (XNU xnu-8796.121.2) on the Xiaomi
Mi 10S (SM8250 / Snapdragon 870), from RAM first, and leaves the Android
installation and its data untouched. The fork keeps the THYME drivers and the
device fixes verified on the physical phone, and merges upstream common work
selectively.

The authoritative, evidence-backed status is
[`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md); this page is the
summary. A `PASS` there means observed on the physical Mi 10S in the named
build, not merely compiled.

## Status at a glance

| Area | Status | What is verified |
|---|---|---|
| Boot to SpringBoard | **PASS** | launchd, backboardd, SpringBoard and the settings/UI stack reach the home screen from a cold RAM boot |
| Display and UI pacing | **PARTIAL** | 1080 x 2340 panel timing accepted and CoreAnimation paces to 90 fps when asked (452, 451, 451, 451 frames in four five-second windows, 11.1 ms median), but the display controller the firmware leaves behind refreshes the panel about 60 times a second: the DSI command-DMA status count is 118 transitions a second with `fb_hz=60` or `90` and with either panel timing, so 90 fps into it judders. The daily default is now a self-consistent 60 Hz, verified by a 32-minute soak (`logs/035725-run.log`); true 90 Hz needs the controller refresh reprogrammed |
| CPU / DVFS | **PASS** | 7 CPUs start; all three EPSS domains reach 1804 / 2419 / 3187 MHz |
| GPU (OpenGL via Mesa/FD650) | **PASS** | hardware `hakios-gl` frames, no `softpipe` during use; 110 frames in the latest wireless run |
| Metal | **PARTIAL / blocked** | `hakios-metal` probes and builds; the first render pass still hangs, so Metal/CoreAnimation stays off. Upstream's Metal-on-Vulkan/Turnip path is merged but opt-in and untested on THYME |
| Touch, keys, gestures | **PASS** | FTS521 touch, buttons, tap/drag/gesture handling; `errors=0` |
| Brightness | **PARTIAL** | DSI DCS brightness is acknowledged; automatic brightness waits for the ambient-light sensor path |
| Wi-Fi (QCA6390) | **PASS** | scan, 2.4/5 GHz WPA2 join, four-way handshake, DHCP, DNS/TLS/Safari; power off/on recovery; the link stays up across display sleep (200 fetched before and after a 25 s screen-off, same address) |
| Bluetooth (QCA6390, 3 Mbaud) | **PASS** for discovery, pairing, connection and A2DP; open for HFP/SCO, AVRCP, microphone and automatic reconnect | H4 transport, 560 LE reports, no transport faults; the QCA responder starts before `bluetoothd` |
| Audio | **PASS** speaker and microphone; haptics **PARTIAL** | speaker/mic paths run with an audible/recorded 1 kHz check; the vibration coil and BEMF are verified, iOS haptics feedback is not |
| Battery, charging, thermal | **PASS** | percentage/current/charging state; 30-minute charging endurance; thermal caps and orderly return |
| Camera | **PARTIAL** | both cameras stream into `/dev/hakios-camera` with `cam=5 cam_real=1` (in the daily boot args, so the Camera icon is always there). The S5KHMX runs the module's mode 3 whole — 3008×2256 through CSIPHY 3, CSID480 and IFE-lite — at ten to twenty-five frames a second: the mode's own 92 ms frame, or the shorter one the capture stack's AE asks for when the light allows (39 ms at the shortest; two DMA banks, one frame per read: no frozen first frame and no torn one), with the capture stack's auto exposure and the phone's own colour tuning (3 colour matrices' lights, 3 lens shadings); the shutter renders 12 MP stills from eight averaged raw frames. A frame's header names the exposure its pixels were made with, so the AE settles from 8× to the room's gain in about five frames instead of ringing bright and dark; a stream asks the kernel for its own sensor before its first frame, so a rear↔front handover powers one sensor, not two (1.7–2.4 s measured, about a third of it the kernel's power-up and the rest the capture graph's reconfiguration). The wide lens's motor is an **AKM AK7377** at I2C 0x18 (the module file's `umi_ak7377_s5khmx`; its position is register 0x00, twelve bits); at stream start the capture stack's contrast autofocus climbs about the module's position and, when that neighbourhood is flat, measures the whole travel — the fridge's QA label is legible in the photograph, beside the Android crop (`logs/label-compare-af3.jpg`). The front S5K3T2 runs the module's 2592×1940 mode through CSIPHY 4 and CSID-lite 1 (the CSID's PHY select is a 3-bit field: CSIPHY 4 is input 4) with its own tuning, verified in the cam=7 path; in the app its exposure applies through SMIA's group hold mid-stream (1560 lines read back `0x618`, and the frames' brightness follows the gain — `logs/215326-run.log`; `cam_front_hold=0` keeps the module's stop and start), and a 150 s session shows a real, if noisy, scene instead of a black frame (`logs/front-lit-210.png`). The grey-world/neutral-cell blend, a 1.5% red calibration and a 1.18 saturation (its tuning has no colour matrix) bring the front's colours to the Android camera's on the same walls. A full-resolution (108 MP) still path remains |
| Sensors (accelerometer, ALS, proximity) | **IN PROGRESS** | the SLPI sensor core now loads its signed firmware, comes out of reset and exposes its QRTR services; the SNS client and a HID sensor service are being brought up. No sensor reaches iOS yet |
| Cellular (modem, SIM, calls/data) | **FAIL** | the MPSS/telephony stack is not implemented |
| Persistent storage | **TODO** | the root and user data are RAM-backed; a guarded persistent-storage design is not accepted yet |
| Rootless package manager (Sileo) | **PARTIAL** | offline dpkg install/remove and Sileo rendering pass; online repository install is not verified |
| Chinese UI | **PASS** | Simplified/Traditional Chinese resources and PingFang render; the Settings language gate switches the whole page |

## What is being worked on now

1. **Sensors** — the SLPI firmware (`slpi.mdt`, `slpi.b00..b20`) is pulled from
   the phone's Android, authenticated by TrustZone and started; the GLINK
   `IPCRTR` channel and the QRTR service list are up. The next steps are the
   SNS client protocol (the accelerometer first), the sensor registry served
   over FastRPC, and an IOHID sensor service for `backboardd`.
2. **Real camera capture** — the S5KHMX's frames reach Camera.app through
   `/dev/hakios-camera` (`cam=5 cam_real=1`) at the frame length its AE asks
   for — ten to twenty-five frames a second — the whole 3008×2256 sensor mode,
   with exposure/gain from the capture stack and
   the phone's own colour matrices and lens shading; the S5K3T2 front camera
   streams 2592×1940 through CSIPHY 4 and CSID-lite 1, and the app switches
   between them with one sensor power-up per handover; the front's exposure
   applies through SMIA's group hold mid-stream. A frame's header names the exposure it
   was made with, so the AE settles instead of ringing, and the shutter renders
   stills from four averaged raw frames. The wide lens's AK7377 autofocus
   climbs about the module's position and sweeps the whole travel when the
   scene is outside it. The next pieces are the front's noise at high gain and
   a full-resolution (108 MP) still path.
3. **Cellular** — the largest remaining block: MPSS, QMI and the telephony
   stack.
4. **Persistent storage and a standalone installer** — designed around a
   guarded write path and RAM-boot-from-flash, without touching Android until
   an explicit authorization and recovery gate exists.

## Verified RAM baseline (7 November 2026)

| Artifact | SHA-256 |
|---|---|
| `hakios-kernel` | `ab427cc33a55985186406aba1325e9a6093f56e1e0b45ce72c13106ffe01c120` |
| `out/thyme-boot/root-wirelessfull.apfs` | `8f4271d57985795e3e2b1b2783b01daa4fd44bbede203de4b2b3d0a46cb14acb` |
| `devices/thyme/prebuilt/system.apfs` | `4f0f3f8b1ba017ed96016883127761ea65c7d070be791fb49789c2fdace5ab48` |
| `devices/thyme/prebuilt/xnu-usb.img` | `2d662c991a5abacc9f229d3b9d556665959fbda16ec2887f357b7dea56400e10` |

The canonical cold-boot gate is `logs/230840-run.log`: two Wi-Fi joins with
DHCP `192.168.0.109`, Safari TLS, 560 LE reports (126 named), 110 `hakios-gl`
frames (the scene's screens are mostly still), zero H4 faults and no panic.
The 90 fps pacing check is
`logs/115200-run.log`; the 60 Hz soak is `logs/035725-run.log`.
`devices/thyme/ram-baseline.conf` is the machine-checked
copy of these hashes; `devices/thyme/ram-regress.sh` refuses other bytes unless
a new candidate is explicit.

## Building and running

The development environment is WSL2 Ubuntu; the source, Git, build caches and
logs live on ext4, and Windows is used only for USB/fastboot where needed.
Nothing is flashed by the normal test path: the kernel and user space are
loaded into RAM and the phone returns to Android afterwards.

```sh
make thyme_defconfig
make kernel JOBS=16
devices/thyme/build-root.sh
devices/thyme/ram-regress.sh
```

The vendor firmware, the Darwin user space and the big system image are local
and Git-ignored; see [`devices/thyme/README.md`](devices/thyme/README.md) for
what has to be pulled from the phone and an IPSW, and
[`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md) for the hardware
evidence, test recipes and open items.

## Upstream

This fork tracks [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)
for the common kernel, loader, user space and tools, and keeps THYME-only work
in `devices/thyme/`. Upstream main `63fc2b4` is merged; the Metal-on-Vulkan/
Turnip path is opt-in (`TURNIP=1 M2V=1`, `metal_m2v=1`) and is not part of the
daily-driver image. The device-specific work is never pushed upstream.

## Repositories and credits

| Repository | Contents |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [`hakios-metal/`](hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (Qualcomm SC7280 family) |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20 (MediaTek Helio G85): an experimental port with its own loader. Not in this repository's manifest |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader: Mu-Silicium with HakiOS's boot path, banner and name, and HakiOS's U-Boot |
| [hackios-web](https://github.com/oosexgang/hackios-web) | the website |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | old: a pointer kept for its links. User space is `userland/` here |

Nothing of Apple's and no vendor firmware is in this repository. The Darwin
user space, if used, is laid out from an IPSW the user provides; firmware
comes off the user's own phone. Apple's services (activation, Apple ID,
iCloud, the App Store) are not supported and not a goal.

The XNU patch and the files derived from it are under the Apple Public Source
License 2.0 ([LICENSE.APSL](LICENSE.APSL)); the rest is MIT
([LICENSE](LICENSE)), as the file headers say. Third parties: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
[COMPLIANCE.md](COMPLIANCE.md). What is and is not distributed:
[LEGAL.md](LEGAL.md). None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or
endorsed by Apple**; "Darwin", "iPhone", "XNU" and "Apple" are named only to
describe what the code works with.

Thanks to the upstream HakiOS team and to the people who reverse-engineered
the SLPI/SNS and FastRPC protocols in the open (see
[`tools/`](tools) and the sensor notes in `devices/thyme/CHECKLIST.md`).
