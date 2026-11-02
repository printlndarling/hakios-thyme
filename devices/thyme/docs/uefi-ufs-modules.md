# THYME UEFI UFS modules

Date: 2026-11-03

The THYME XBL contains the storage stack that the current Silicium image does
not expose. It was inspected read-only with `tools/uefi-modules.py` from the
phone's local `xbl_a.img`.

| Module | GUID | PE32 SHA-256 |
|---|---|---|
| `UFSDxe` | `0d35cd8e-97ea-4f9a-96af-0f0d89f76567` | `ffeaed467f8251b7e30a45075da84a1c563e2bac7dc4be0f79f0ed49a88b8dc9` |
| `PartitionDxe` | `1fa1f39e-feff-4aae-bd7b-38a070a3b609` | `9cc8760dbe3dfca656e26cacea5f6e6b8cf09c02d5ddb35f11f1e229c9b67c06` |
| `SdccDxe` | `f10f76db-42c1-533f-34a8-69be24653110` | `b62eca095e3a5776735d8ceb8dafbf3d35a5f9d786b0d4d2f98813ac01f7b344` |

The XBL also contains `UsbMassStorageDxe`, but that is not a substitute for the
internal UFS controller.

The modules are vendor firmware and must stay local. The extraction command is:

```sh
venv-uefi/bin/python tools/uefi-modules.py xbl_a.img \
  --match UFSDxe --extract work/thyme-firmware/modules
```

Repeat with `PartitionDxe` and `SdccDxe`. The next build step is to place these
PE32 images in an active THYME Mu-Silicium platform package and include their
INF descriptions in the DXE/FDF, then verify BlockIo/GPT enumeration through
`ufs=1` RAM boot. No storage write is allowed before that read-only gate passes.

## First integrated build

The active THYME platform now includes the phone's `UFSDxe`, `DiskIoDxe` and
`PartitionDxe`. A Release Mu-Silicium build succeeded:

| Artifact | SHA-256 |
|---|---|
| `SILICIUM_UEFI.fd` (3 MiB) | `7203aeaafcd16ad4332d7f007fc3fda1dde0ece9c745bffab3f9537182aae468` |
| `out/thyme-boot/xnu-ufs.img` | `f6797dfae5d889e4992a878f8c777c82a691482d2ee6005a286e5f27de9d7604` |

`tools/uefi-modules.py` confirms that the generated FD contains `UFSDxe`,
`DiskIoDxe` and `PartitionDxe`. Hardware boot and GPT enumeration were not yet
verified at that build point.
