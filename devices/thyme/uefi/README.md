# THYME Mu-Silicium platform

This package restores the former UMI/THYME platform on top of the common
`hakios-uefi` tree and adds the phone's internal storage stack.

The platform is named `umi` inside Mu-Silicium because it follows the Kona
platform package layout. It uses the THYME device tree supplied at build time.

## Build

Keep the vendor firmware and device tree outside Git.

```sh
export THYME_DTB=/home/darling/thyme-assets/thyme.dtb
export THYME_XBL=/home/darling/thyme-assets/staged/boot/xbl_a.img
devices/thyme/uefi/apply.sh
devices/thyme/uefi/build.sh
```

`apply.sh` clones the pinned HakiOS UEFI common tree and Mu-Silicium revision,
initializes only the build dependencies, applies HakiOS's common changes,
installs this platform, and extracts matching vendor modules from the phone's
own XBL.

`build.sh` installs the EDK2 build dependencies in `venv-uefi`, updates external
tools, and writes:

```text
Mu-Silicium/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

## Storage additions

The original UMI platform intentionally omitted storage drivers. The current
DXE list adds the phone's XBL modules:

- `UFSDxe`;
- `DiskIoDxe`;
- `PartitionDxe`.

`tools/uefi-modules.py` can inspect or extract those modules read-only. Their
names, GUIDs and hashes are recorded in `../docs/uefi-ufs-modules.md`.

Secure boot is disabled for RAM bring-up. Vendor binaries remain in the
gitignored Mu-Silicium build tree and are never committed.
