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
to Android after the watchdog. The THYME-specific ST FTS521 touch path is also
running on official main: a physical interaction raised the event count from 7
to 472 with `errors=0`, and backboardd reported contact presence touching/none.
The PM8150 key driver now reports power and volume press/release events. The
THYME Adreno 650 driver is also running on official main: a four-minute run
produced more than 2,100 hardware-rendered `hakios-gl` frames without softpipe
fallback. The THYME audio stack now brings up LPASS/ADSP, APR, MI2S and both
CS35L41 amplifiers with protection firmware. The official loader also boots on
the phone, and the microphone probe now enumerates the WCD938x on SoundWire,
reads its chip/SCP IDs and identifies the Bolero 1.2 profile. Microphone
capture now produces nonzero dual-channel PCM through DEC1/ADC1 and
DEC0/ADC4, and Darwin's AVAudioRecorder completed a 4.9-second recording
through `/dev/hakios-mic` with no skipped frames. See
`docs/official-mic-path.md`. The AW8697 motor driver now follows Xiaomi's
stock activation
sequence, loads and verifies Android's waveforms, and plays waveform 1 back to
standby in 38 ms without a fatal fault; physical feel remains to be confirmed.
Bluetooth is now live on official main: QCA6390 firmware
and NVM download, HCI reset, the persistent H4 endpoint, MAC publication and
iOS Bluetooth initialization all work. The device-owned QCA6390 Wi-Fi stack now
scans, associates to a WPA2 access point, completes DHCP and carries TCP traffic
through en0.
The SM8250 EPSS CPU frequency domains and both TSENS blocks are also live on
official main. All three clusters reached 1804/2419/3187 MHz, the governor
physically throttled the big-core domain at 75.9 C, and an artificially low
`thermal_stop=30` run returned to the loader without a panic.
The command-mode 42 02 0a panel now has a DSI `0x51` backlight path and
CoreBrightness hooks: an iOS request for 500 permille reached the panel as level
2056 and completed the command DMA. A human-visible 20/50/100% comparison is
still pending.

The first official-main run reported:

```text
Darwin 22.5.0 iPhone10,3
memory 6144 MB, page size 16384, 7 cpu
battery: SOC=92%, V=4282 mV, I=-97 mA, T=24 C, battery=1 charger=1 charging=0
battguard: charge 92% (battery present, charger attached, charging no)
```

See `docs/official-core-probe.md`.
See also `docs/official-touch-probe.md`.
See also `docs/official-gpu-probe.md`.
See also `docs/official-brightness-probe.md`.
See also `docs/official-cpu-thermal-probe.md`.
See also `docs/official-audio-probe.md`.
See also `docs/official-haptic-probe.md`.
See also `docs/official-bt-probe.md`.
See also `docs/official-wifi-probe.md`.

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

Build the THYME-specific loader and RAM-boot image:

```sh
DEVICE=thyme make loader       # xnuloader/loader-thyme.efi
devices/thyme/build-loader.sh  # devices/thyme/prebuilt/xnu-usb.img
```

The THYME loader uses Project Silicium addresses `FD_BASE=0x9fc00000`,
`FD_SIZE=0x300000` and `PAYLOAD_BASE=0xf0000000`. See
`docs/official-loader-thyme.md`.

The official loader now builds and boots on THYME. The RAM-boot image used by
the verified microphone runs has SHA-256
`f46783dc8df5973bc260f1853770a26466062a05c65253355b090584519afcf1`.
Keep the legacy image as a recovery reference, but it is no longer the only
hardware-proven loader.

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
