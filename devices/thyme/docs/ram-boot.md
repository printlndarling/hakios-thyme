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

Keep the four files from one build. The loader and APFS images are built
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
