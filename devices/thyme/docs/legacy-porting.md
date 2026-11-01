# Xiaomi Mi 10S (thyme) port

Status: usable bring-up, not daily-driver complete.

This page records only results observed on the physical Xiaomi Mi 10S. The port
is RAM-only: it boots with fastboot boot and never writes Android, userdata, GPT,
or either slot.

The complete daily-use verification matrix is in
[thyme-daily-driver-checklist.md](thyme-daily-driver-checklist.md).

## Target and safety

| Item | Value |
|---|---|
| Device | Xiaomi Mi 10S, codename thyme |
| SoC | SM8250-AC / SD870, Kona platform |
| Android | third-party HyperOS 3 image, bootloader unlocked, root available |
| Display | 1080 x 2340, panel variant 42 02 0a |
| Touch | ST FTS at I2C 0x49, IRQ GPIO 39, reset GPIO 38 |
| GPU | Adreno 650, zap reservation 0x8c41a000 |
| Wi-Fi | QCA6390 over PCIe/MHI |

Rules for every hardware run:

- Address the phone explicitly with adb -s and fastboot -s.
- Do not run a command against a differently named phone on the same host.
- Use fastboot boot only. Never flash, erase, change slots, or write userdata.
- Keep Android and the user's data untouched.
- Keep the canonical repository and build outputs on the WSL ext4 filesystem.

## Current hardware status

| Area | Status | Evidence and limits |
|---|---|---|
| Loader / UEFI | PASS | THYME DTB is accepted; RAM-only boot reaches XNU |
| CPU | PASS | 7 CPUs start and cpufreq is active |
| Display | 1080 x 2340, panel variant 42 02 0a |
| Touch | PASS | FTS521 initializes and touches reach backboardd and SpringBoard |
| GPU | PASS | Adreno 650, FD650 Mesa, CoreAnimation uses hardware rendering |
| Battery monitor | PASS | Percentage, signed current, temperature and charger presence are reported |
| Charging under XNU | PARTIAL | Qualcomm `TakeAction` now produces positive current; long-duration safety and thermal testing remain |
| ADSP firmware | PARTIAL | Firmware starts and links; some services remain unavailable |
| Sound | PARTIAL | Driver path starts; audible output still needs a physical check |
| Haptics | PARTIAL | AW8697 is detected and its waveform loads; vibration is unconfirmed |
| Wi-Fi | PARTIAL | PCIe, MHI, SMMU and firmware start; QMI/WLFW requests time out |
| Bluetooth | NOT STARTED | QCA6390 UART firmware is collected; iOS HCI is not working yet |
| Camera | NOT STARTED | THYME firmware and sensor tables are collected |
| UFS / storage | NOT TESTED | Read-only survey is the next safe step |
| ALS / auto-brightness | MISSING | No ambient-light sensor path; backboardd reports ALS timeout |
| Cellular | MISSING | Modem and iOS telephony path are not implemented |
| NFC / fingerprint | MISSING | Not ported |

## Why the screen flashed text and touch stopped

Two unrelated symptoms had one trigger in the previous test payload:

    loader_return_after=300

At five minutes XNU handed control back to the loader. The GPU was stopped,
backboardd received invalid display/GPU state, crashed with SIGSEGV, and launchd
respawned it with the software softpipe renderer. SpringBoard restarted while
loader text flashed over the panel. Touch looked dead because the UI and input
services were being restarted, not because the FTS driver failed.

The log sequence is:

    --- loader_return_after expired ---
    adreno: the run is about to end: the GPU is given back
    hakios: signal 11 ... backboardd
    backboardd ... exited due to SIGSEGV
    hakios-gl: ... softpipe

The stable payload therefore uses:

    loader_return_after=0

It also omits -v and fb_scan=1. Those options are useful for diagnostics but
must not be used for normal UI boots.

## Stable boot payload

Build the current kernel and userland, then create the payload:

    cd /home/darling/hackios
    devices/thyme/build-kernel.sh -j8
    DEVICE=umi devices/thyme/build-userland.sh
    devices/thyme/build-stable-payload.sh

The resulting payload is:

    /home/darling/hackios/out/thyme/run-payload-stable.bin

Its command line is:

    loader_return_after=0 epoch=<boot-time> sb_delay=8 bootcpu=7 cpus=7
    umi_touch=1 umi_keys=1 cpufreq=1 gpu=1 battery=1 battery_charge=1
    battery_stop=25 battery_stop_poll=30

The payload contains the kernel and root.apfs. system.apfs is staged separately.

## Charging and battery safety

Charging is not implemented while XNU owns the phone. The Qualcomm UEFI charger
stack is still queried for status, but the loader does not run the charger action
state machine. A cable can therefore be present while the battery is discharging.

The first THYME measurement exposed a false-positive charging flag:

    battery: flags=1f SOC=99% V=4412 mV I=-162 mA action=7

The loader used to set charging whenever a cable was present and SOC was below
100. That is wrong. It now sets charging only when the signed current is greater
than zero. The battery guard also treats "charger attached but not charging" as
a battery-only run, so a low battery can still return to the loader.

The next stable run on 2026-11-01 confirmed the loader correction:

    battery: flags=f SOC=98% V=4344 mV I=-162 mA T=29 C action=7

`flags=f` is valid, battery present, charger present and voltage valid, with the
charging bit clear. The current was still negative because the read-only path
did not run the charger action state machine.

The following run used `battery_charge=1`, which gates Qualcomm
`QcomCharger->TakeAction(action, status)` behind an explicit switch. The stock
charger application and `QcomChargerDxeLA` confirm that this call is the normal
charging path, and that action 7 is handled as a charging action. On hardware:

    battery: TakeAction(7) accepted
    battery: flags=f SOC=97% V=4334 mV I=-162 mA ... action=7
    battery: TakeAction(3) accepted
    battery-now: flags=1f SOC=97% V=3880 mV I=1009 mA ... action=3
    ... two minutes later:
    battery-now: flags=1f SOC=97% V=4181 mV I=93 mA ... action=3

The charger therefore moved from a net discharge to positive current, and the
loader's charging bit became set. `TakeAction` completed more than 100 calls
without a firmware error during the first charging run. A later stable run lost
the loader's USB endpoint after about five minutes while continuing to use the
periodic action loop. That is not yet root-caused. The loader now stops issuing
`TakeAction` as soon as one action is accepted, then leaves the programmed PMIC
state alone. The next run may accept action 7 before the kernel starts and one
action 3 once the kernel is running. This one-shot behavior still needs another
long hardware run. The stable payload carries `battery_charge=1`, and
`oem battery` reports the live loader-owned state for diagnostics.

The follow-up stable run also verified the kernel battery guard. It had been
using `kernel_sysctlbyname` before the battery sysctls were usable and therefore
logged `charge -1%`. THYME now exposes the battery driver's in-kernel state
directly to the guard:

    battguard: charge 97% (battery present, charger attached, charging yes); stop at or below 25%

This keeps the host timer as the primary protection and makes the 25 percent
in-kernel fallback effective.

Stable payloads carry:

    battery_stop=25 battery_stop_poll=30

This is a last-resort kernel guard. The primary safe workflow is a host timer
that returns the phone to Android and leaves it there to charge:

    PHONE_SERIAL=<target-adb-serial> devices/thyme/charge-guard.sh 20

Cancel it with:

    devices/thyme/charge-guard.sh cancel

After the timer, verify Android reports a positive charge current before the
next RAM boot. Do not repeatedly start iOS below 40 percent until the charger
state machine is implemented.

## Safe RAM boot

From Android:

    adb -s <target-serial> reboot bootloader
    fastboot -s <target-serial> boot xnu-usb.img

Wait until fastboot reports the loader as xnuloader, then use only that loader:

    fastboot -s xnuloader oem ramdisk
    fastboot -s xnuloader stage system.apfs
    fastboot -s xnuloader stage run-payload-stable.bin
    fastboot -s xnuloader oem run

To stop a run and return to Android:

    fastboot -s xnuloader reboot

The Windows fastboot client cannot open a file inside the WSL ext4 VHD directly.
When a Windows fastboot build is required, mirror only the ready-to-boot
artifacts to a temporary Windows directory. Keep source edits and builds in WSL.

## Touch bring-up notes

The THYME device tree exposes a six-byte board identity, including the trailing
NUL. The correct check is:

    size == 6 && memcmp(board, "thyme", 5) == 0

Checking size == 5 fails, and a mismatch makes every THYME driver return before
touching hardware.

A healthy touch boot contains:

    UMI FTS: ST FTS521 3639 initialized scan=0x1
    UMI FTS: poll=... events=... errors=0
    backboardd ... [TouchEvents] contact 1 presence: touching

If these lines are absent:

1. Confirm the stable payload command line contains umi_touch=1.
2. Confirm the board identity check accepts size 6.
3. Confirm touch boost and ldoc1 power votes succeed.
4. Check for write-address NACK, controller-ready timeout, or bad chip ID.
5. Check that the stable payload has not returned to the loader.

## GPU and display notes

Working evidence:

    adreno: THE 3D CORE HAS POWER AND ANSWERS
    hakios-gl: OpenGL ES 3.2 Mesa 25.0.7; FD650
    HakiosGPU: ... submissions ... buffers named

The THYME zap shader reservation is 0x8c41a000. Do not reuse UMI's value.

Do not enable -v for UI work. The kernel text console draws over SpringBoard and
looks like the screen is randomly running code. fb_scan=1 is also unnecessary
for the current stable configuration.

## Reproducible artifact hashes

At the time of this bring-up:

    root.apfs
    385bc22574c3bf606e8ce1206fdf7a97295048f85c6c0e63e146d81879dfa1fe

    system.apfs
    fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4

    run-payload-stable-validated.bin
    3e376baed1f2c1b070f6a39c04e66ef800270f110fa043385b05aa0440e8bd96

    run-payload-charge.bin
    7ea04cf42389cb9359c9d1b3affbd6c1e01b0c972a75de137b02eecce7a64d23

    loader.efi
    a870ca008ca171b80d152173a8e8d76cae2a4566c428e6a2bf16c307da8c6963

    xnu-usb.img
    a7061100f88135e5887057bebca24e242c0c7eebd57047888ba81ef743b029cb

    mach.development.bcm2837
    89a066a0b61bbbe8cae08f0728a5b565a3515148a1abec8d472a6efad296178

Firmware files, iOS images, build outputs and keys stay outside Git.

## Priority TODO and checklist

### P0: keep the UI stable

- [x] Identify the five-minute loader return as the trigger for the crash/flash.
- [x] Build a payload with loader_return_after=0.
- [x] Remove -v and fb_scan from the stable command line.
- [x] Confirm FD650 hardware rendering after more than five minutes.
- [x] Confirm a stable boot longer than eight minutes without backboardd respawn.
- [x] Distinguish charger presence from actual battery charging current.
- [x] Make battery_stop work when a cable is attached but the phone is discharging.
- [x] Add a host-side timed charge guard.
- [x] Allow battery_stop and battery_stop_poll through the THYME payload gate.
- [x] Allow the microphone and thermal-stop arguments required by the daily-use profile.
- [x] Verify the corrected charging flag at loader initialization on hardware.
- [x] Add `oem battery` diagnostics and gate Qualcomm TakeAction behind `battery_charge=1`.
- [x] Drive Qualcomm TakeAction and obtain a positive signed battery current in XNU.
- [x] Keep TakeAction gated until it has hardware evidence, then enable it in the stable payload.
- [x] Make battery_stop read the battery driver state directly instead of an early sysctl lookup.
- [x] Verify battery_stop reports a real SOC and effective charging state on hardware.
- [x] Stop TakeAction after the first accepted action after a later USB endpoint wedge.
- [ ] Re-test the one-shot TakeAction build for at least 30 minutes without losing xnuloader USB.
- [ ] Run a 30-minute charge while watching current, temperature and charger failures.
- [ ] Confirm the SpringBoard charging indicator follows the corrected charger state.
- [ ] Leave the stable boot running for 30 minutes and confirm backboardd does not respawn.
- [ ] Physically confirm a tap, drag, and edge gesture in SpringBoard.
- [ ] Confirm the stable payload does not return to the loader after 30 minutes.

### P1: complete basic daily-use I/O

- [x] Wi-Fi: accept THYME firmware version 0x1016046e in source.
- [x] Wi-Fi: map qca6390/bd_j2s.elf instead of bd_j1gl.elf.
- [ ] Wi-Fi: verify WLFW mode request and station readiness.
- [ ] Charging: monitor the TakeAction loop for a full charge cycle and implement failure recovery.
- [ ] Bluetooth: identify the HCI transport in the THYME device tree.
- [ ] Bluetooth: load htbtfw20.tlv and htnv20.bin, then verify an HCI reset.
- [ ] Audio: physically verify speaker output.
- [ ] Haptics: fix AW8697 UVLO and physically verify vibration.

### P2: remaining hardware

- [ ] Camera: probe the THYME sensor module read-only.
- [ ] UFS: run a read-only survey before enabling any write path.
- [ ] Display: measure sustained frame cadence and thermals during scrolling.
- [ ] ALS: add a sensor or explicitly disable automatic brightness.
- [ ] Cellular, NFC and fingerprint: define scope before starting.

### P3: product quality

- [ ] Convert repeated bring-up commands into one safe device script.
- [ ] Add automated log assertions for touch, FD650, backboardd and loader return.
- [ ] Record every new hardware result in this file with date and hash.
- [ ] Re-run the maintenance checks before publishing the device repository.

## Known log noise

Bluetoothd currently exits every ten seconds because the device tree has no
usable Bluetooth HCI identity. The repeated service is expected during this
stage and does not imply a new crash. Auto-brightness may also log an ALS
timeout. Neither should be mistaken for the loader-return failure above.
