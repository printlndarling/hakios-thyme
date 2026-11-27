# Cross-platform RAM boot

The Windows batch file and the Linux/macOS shell script use the same Python
launcher. The phone starts in Android with USB debugging authorized. The
launcher checks the device and battery, checks the unlocked state in fastboot,
and sends the loader and system images to RAM. Restarting the phone returns
to Android. The daily RAM kernel uses the existing low-battery, thermal, and
watchdog settings from [ram.args](../ram.args).

## Where the images go

The launcher uses `thyme-bundle/` beside the README when it exists. Use
`--bundle-dir /path/to/images` for a different directory. Otherwise, after
building in the current checkout, the launcher uses these output paths.

| Bundle file | Build output |
|---|---|
| `xnu-usb.img` | `devices/thyme/prebuilt/xnu-usb.img` |
| `hakios-kernel` | `hakios-kernel` |
| `root.apfs` | `userland/root.apfs` |
| `system.apfs` | `userland/system.apfs` |

Keep the four files from one build. Rebuild `xnu-usb.img` using the current
`devices/thyme/build-loader.sh`. The launcher checks the embedded loader
before contacting the phone, and accepts the thyme loader that keeps logs in
RAM for USB retrieval. The older thyme loader wrote diagnostics to `logdump`. The loader and APFS images are built
locally with licensed IPSW and firmware inputs. Optional `fw-wifi/mac` is
read from the checkout. Keep it and any private network configuration local.

## First-run downloads

Use `--bundle-url` with an HTTPS link to a version 1 manifest. For a persistent
launcher shortcut, set `HAKIOS_THYME_BUNDLE_URL` to that link. An explicit
`--bundle-dir` takes precedence over the environment variable. Each image
URL can be relative to the manifest or an absolute HTTPS URL. Use a
trusted manifest URL, since it supplies the expected hashes.

```json
{
  "version": 1,
  "device": "thyme",
  "files": {
    "xnu-usb.img": {"url": "xnu-usb.img", "sha256": "<64 hex characters>"},
    "hakios-kernel": {"url": "hakios-kernel", "sha256": "<64 hex characters>"},
    "root.apfs": {"url": "root.apfs", "sha256": "<64 hex characters>"},
    "system.apfs": {"url": "system.apfs", "sha256": "<64 hex characters>"}
  }
}
```

Use `--check` to resolve inputs, verify downloaded hashes, and build the
staged payload without contacting a phone. Downloads and Google Platform-Tools
go under the ignored `out/thyme-ram-boot/` directory. The project currently has
no release that provides the four images.

## If the phone is still in fastboot

Reconnect the data cable if needed. With only the target phone attached,
`fastboot reboot` returns it to Android. A long press of the power button can
restart the phone if the loader is unresponsive. For timed engineering runs
and kernel logs on capable development hosts, use the repository's
[`xl`](../../../xl) workflow.

## Timed daily scene on a development host

Use a single authorized thyme in Android and put `adb` and `fastboot` on
`PATH`. The soak uses the same 40% minimum charge and battery temperature
below 45 °C as the cross-platform launcher. Start from a fresh loader
session with the desired system image. The script validates the embedded
loader and stages a host-only payload check before changing the USB state.

```sh
SYSTEM_IMAGE=userland/system.apfs SCENE=userland/root.apfs \
  SOAK_SECS=1800 SOAK_STEP=300 devices/thyme/soak.sh
```

Build `userland/root.apfs` for the daily scene before running this command.
The script defaults to the locally prepared wireless test scene at
`out/thyme-boot/root-wirelessfull.apfs`. It keeps the battery, thermal and
watchdog arguments from [ram.args](../ram.args). Optional interval
screenshots are requested while the kernel is running, with a one-minute
margin before the requested end.

Each run saves its upload and run transcripts, the final kernel log and any
screenshots under a unique `logs/soak.*` directory. A successful run requires
a normal loader result, the kernel timer-expiry marker, and Android boot
completion. A battery or thermal exit, a panic, or missing evidence fails the
test. A completed timer is one uptime observation. Use the native app and
long-term gates in the [checklist](../CHECKLIST.md) to record wider coverage.

## NFC-A passive polling

Build the dedicated polling scene on WSL2 and run it with the normal
battery, thermal and timer guards.

```sh
SCENE=nfcpoll METAL_PLUGIN=ca devices/thyme/build-root.sh
SCENE=out/thyme-boot/root-nfcpoll.apfs SOAK_UIM_READONLY=1 SOAK_SECS=120 devices/thyme/soak.sh
```

The scene opens `/dev/hakios-nfc`, initializes NCI 2.0, runs one bounded
passive NFC-A poll, then requests idle deactivation. It reports command
results and any discovered technology or protocol. If a known test tag
is available, present it near the rear NFC antenna during the polling
window. The scene leaves tag identifiers and contents out of the log.
The daily scene leaves this test opt-in.

## Dual-SIM inventory

With two active SIMs, `SOAK_UIM_READONLY=1` observes the modem's UIM
inventory during the normal CommCenter boot. It disables the port's
diagnostic slot switch, provisioning and SMS submit. The inventory
counts present and READY SIM/USIM applications and reports the
primary and secondary GW indices. Network registration and native
line switching require their own checks.

## App-only Metal

Use `--metal-apps` with a compatible RAM Boot bundle to expose Metal to
separately installed applications while the compositor keeps its regular
renderer. Use `--metal-apps --check` to validate the bundle without a phone.
For the timed soak, `SOAK_METAL_APPS=1` selects the same profile.

The historical Baidu Maps verification used an operator-provided bundle.
Current build images omit it. To repeat that comparison, install an
authorized copy separately, handle first-launch choices on the phone,
then check map tiles and panning.
