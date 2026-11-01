# Xiaomi Mi 10S (`thyme`)

THYME device package for the official HakiOS common tree. This repository is now
a fork of `oosexgang/hakios-xnu-kernel` at the revision recorded in
`UPSTREAM.md`; the former third-party fork is kept only as a read-only reference
under `reference/legacy-device/`.

## Target

| Item | Value |
|---|---|
| Device | Xiaomi Mi 10S |
| Codename | `thyme` |
| SoC | Qualcomm SM8250-AC / Snapdragon 870 |
| CPU | 8 cores; RAM boot currently starts on core 7 |
| GPU | Adreno 650 |
| Display | 1080 x 2340, panel selection `42 02 0a`, 90 Hz mode |
| Touch | ST FTS521, I2C `0x49`, IRQ GPIO 39, reset GPIO 38 |
| Buttons | PM8150 PON and PM8150 GPIO6 |
| Wi-Fi | QCA6390, PCIe 0 / MHI |
| Bluetooth | QCA6390 UART, QUP6 at `0x00998000`, GPIO 16-19 |
| Haptics | AW8697, I2C `0x5a`, reset GPIO 130, IRQ GPIO 129 |
| Android | third-party HyperOS 3 image, unlocked bootloader, rooted |

The device tree is intentionally not committed. The locally collected copy is
kept outside Git at `/home/darling/thyme-assets/thyme.dtb`.

## Safety rules

- Use `fastboot boot` only. Never flash, erase, change slots, or write Android,
  userdata, or GPT from this project.
- Address ADB and fastboot explicitly. The target Android serial is
  `5290780a`; the RAM loader serial is `xnuloader`.
- Do not send fastboot commands to an unidentified `????????????` endpoint.
- Keep a charge timer active for every unattended RAM session.
- Keep source, builds, and logs in WSL ext4. Copy only ready boot artifacts to
  Windows when a Windows fastboot client is required.

## Current state

The official-main common kernel now boots this phone through the legacy RAM
loader, reaches `/redwood/sbin/init`, starts launchd and SpringBoard, and returns
to Android after the watchdog. The first official-main probe used the generic
framebuffer display path and the battery/battguard drivers; device-specific GPU,
touch, keys, audio, Wi-Fi and Bluetooth still have to be migrated.

The first official-main run reported:

```text
Darwin 22.5.0 iPhone10,3
memory 6144 MB, page size 16384, 7 cpu
battery: SOC=92%, V=4282 mV, I=-97 mA, T=24 C, battery=1 charger=1 charging=0
battguard: charge 92% (battery present, charger attached, charging no)
```

See `docs/official-core-probe.md`.

The legacy tree previously proved the following on the physical phone:

- SpringBoard and the desktop rendered through the Adreno 650.
- FTS touch produced DOWN/MOVE/UP events, taps, drags, and system gestures.
- Power and both volume buttons produced HID events.
- Power-button display off and wake completed without a SpringBoard restart.
- Stable UI sessions exceeded 30 minutes without loader return, softpipe
  fallback, or a backboardd SIGSEGV.
- Qualcomm `TakeAction` produced positive signed battery current. One-shot
  charging used exactly two firmware actions: action 7 before the kernel and
  action 3 after it.
- The external charge guard returned the phone to Android after a bounded run.

Those are evidence for the port, not claims that every part already works on the
official-main build. See `docs/legacy-validation.md` and
`docs/legacy-checklist.md`.

## Build

```sh
make thyme_defconfig
make config
make kernel -j8
```

The device is registered in `devices/manifest.conf`. The official tools must
accept it before any build:

```sh
tools/hakios-device.py check thyme
make config
```

The UEFI/loader path is not yet rebuilt against the official repository. Until
that passes, use the legacy tree only as a hardware reference and fallback.

## Repository layout

| Path | Purpose |
|---|---|
| `device.conf` | device identity and user-visible hardware status |
| `configs/thyme_defconfig` | common + SM8250 + THYME configuration layering |
| `configs/thyme.config` | drivers enabled for this device |
| `soc.args` | SM8250 addresses read from the THYME device tree |
| `soc/` | SM8250 family facts |
| `docs/` | porting notes, validation evidence, and migration checklist |
| `reference/legacy-device/` | read-only source snapshot from the former fork |

## Firmware and private inputs

Firmware pulled from the user's phone is stored in the ignored `fw-*` folders at
the repository root. These folders and the DTB must never be committed.

## Remotes

```text
origin    https://github.com/printlndarling/hakios-thyme.git
upstream  https://github.com/oosexgang/hakios-xnu-kernel.git
```

See `UPSTREAM.md` for update rules and `MIGRATION.md` for what remains before
the old local repositories can be deleted.
