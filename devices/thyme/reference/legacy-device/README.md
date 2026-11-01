# Xiaomi Mi 10S (`thyme`)

This is the native THYME device layer for the current HakiOS tree. It started as a
copy of the UMI device package because both phones use the Kona platform, but the
board-specific values and wrappers are qualified for THYME.

Status: **usable bring-up**. SpringBoard, touch, GPU rendering and battery
reporting are hardware-verified on 2026-11-01. Wi-Fi, Bluetooth, audio,
haptics, camera and storage remain incomplete; see
[docs/porting/thyme.md](../../docs/porting/thyme.md) for evidence and TODO.

## Target observed on hardware

Collected from the target Mi 10S on 2026-10-31:

| Item | Value |
|---|---|
| Model | M2102J2SC |
| Platform | `thyme`, `kona`, SM8250-AC |
| ROM | third-party HyperOS 3 based image, `OS3.0.10.0.WPACNXM` |
| Bootloader | unlocked; properties are partly spoofed by the ROM |
| RAM | 7,696,892 kB total |
| Display | 1080 x 2340, active 90 Hz mode |
| Panel boot selection | 42 02 0a command-mode panel |
| Touch | ST FTS at I2C address `0x49`, IRQ GPIO 39, reset GPIO 38; `ldoc1` is 1.8 V |
| GPU | Adreno 650, chip ID `0x06050002` |
| GPU zap reservation | `0x8c41a000` |
| Wi-Fi | QCA6390 over PCIe/MHI |
| Battery at collection | 97%, charging, 4392811 uV, 280 deci-C |

The real THYME DTB is not committed. Its observed size is 933,590 bytes and its
SHA-256 is:

```text
36809f49b323fe3eb7d526a0a013f7283af575724031a0e5a7229705474455f7
```

Keep the local copy outside Git, for example under
`references/thyme-device/thyme.dtb`, and pass it with `DTB=...`.

The vendor firmware and XBL images are also outside Git. The collected local
manifest is kept with the assets, not in this repository.

## Layout

| Path | What |
|---|---|
| `configs/thyme_defconfig`, `configs/thyme.config` | native device and SoC configuration |
| `soc/` | the SM8250 family layer |
| `drivers/umi/conf` | THYME-qualified wrappers around the shared UMI implementations |
| `overlays/xnu/` | THYME-specific kernel overlays, including the THYME zap address |
| `overlays/userland/` | shared UMI userland packaging |
| `loader/` | THYME loader source; platform constants match the collected FDT |
| `uefi/` | THYME UEFI build hooks and local vendor-module extraction |
| boot.args | normal test-boot arguments; UI GPU and battery enabled |

The internal UEFI package directory remains `umiPkg` because the current UEFI
source is derived from that layout. This is a build detail, not a claim that the
image is for a Mi 10.

## Build

The THYME DTB must be supplied explicitly:

    export DTB=/absolute/path/to/thyme.dtb

    devices/thyme/build-kernel.sh -j8
    devices/thyme/build-loader.sh
    devices/thyme/build-wifi.sh
    DEVICE=umi devices/thyme/build-userland.sh

The normal test-boot configuration is:

    bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 cpufreq=1 gpu=1 battery=1 battery_charge=1 battery_stop=25 battery_stop_poll=30

Do not add -v or fb_scan=1 to normal UI boots. -v re-enables the text console
and makes kernel log lines flash over SpringBoard.

## Stable daily-use boot

xl adds a short loader_return_after watchdog to scripted test runs. When it
expires, the GPU is stopped and control returns to the loader. backboardd then
crashes, respawns with softpipe, and SpringBoard is restarted while text flashes
on the panel. Build and use the stable payload when the phone should remain in
iOS:

    devices/thyme/build-stable-payload.sh

Then boot xnu-usb.img, stage the system image and
out/thyme/run-payload-stable.bin, and run fastboot oem run. The stable payload
sets loader_return_after=0 and contains neither -v nor fb_scan=1.

## Charging safety

`battery_charge=1` now drives Qualcomm's charger `TakeAction` state machine.
On THYME this changed the measured net current from about `-162 mA` to
`+1009 mA`. It also transitions from firmware action 7 to action 3. The loader
calls `TakeAction` only during the first 24 seconds, before the watching core
hands UEFI's interrupt controller to XNU; the PMIC setting persists afterward.
The host charge guard remains mandatory until a complete charge cycle and
thermal run have been verified:

    PHONE_SERIAL=<target-adb-serial> devices/thyme/charge-guard.sh 20

The timer returns the phone to Android after 20 minutes and leaves it there to
charge. Cancel the timer with:

    devices/thyme/charge-guard.sh cancel

The stable payload also sets battery_stop=25 and battery_stop_poll=30 as a
last-resort kernel guard.

## First boot and safe recovery

The target boot image requires a THYME UEFI FD and the THYME DTB:

    export DTB=/absolute/path/to/thyme.dtb
    export UEFI_FD=/absolute/path/to/SILICIUM_UEFI.fd
    devices/thyme/run.sh reload
    devices/thyme/run.sh ramdisk
    XL_SECS=180 devices/thyme/run.sh run

XL_SECS=180 is only a diagnostic run. Use build-stable-payload.sh and an
explicit fastboot oem run when the phone should not return to the loader.

fastboot boot only. Do not flash, erase, change the active slot, or write a
partition. The current ROM is a third-party image and its recovery path has not
been verified.

## Hardware facts not to guess

- The GPU zap reservation is the THYME FDT value `0x8c41a000`, not UMI's
  `0x8681a000`.
- The touch controller matches UMI's FTS transport and pins, but the THYME
  firmware/limit files and panel selection must be preserved.
- The collected panel is `42 02 0a`; do not copy the public notes' `42 02 0b`
  identity.
- The THYME XBL contains its own firmware volume and `uefiplat.cfg`; UEFI must
  be built from that XBL, not from a UMI vendor-driver set.
- Battery percentage and the charger flag are provided by the loader/UEFI
  charging path; do not infer full Android charger/power-path parity from them.
- THYME Bluetooth uses the QCA6390 UART transport and the phone firmware
  htbtfw20.tlv / htnv20.bin; it is not yet a usable iOS HCI controller.

See the full analysis, safety gates and checklist at
[`docs/porting/thyme.md`](../../docs/porting/thyme.md).
