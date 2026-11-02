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
