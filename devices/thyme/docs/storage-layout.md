# THYME storage layout

Date: 2026-11-03

This is a read-only description from `ufs=1` logs. It is not an accepted write
plan.

## UFS logical units

| Unit | Size | Contents |
|---|---:|---|
| 0 | 119688 MB | Android GPT: boot, super, userdata, firmware |
| 1 | 16 MB | small GPT |
| 2 | 16 MB | small GPT |
| 3 | 32 MB | small GPT |
| 4 | 2176 MB | dynamic metadata / secondary GPT |
| 5 | 64 MB | small GPT |

The main Android GPT has valid CRCs. `super` is LBA 516096..2744319 (8.50 GB).
`userdata` is LBA 2785280..30640122 (about 111.4 GB) and ends within 24 KB of
the end of LUN 0.

## Android dynamic partitions

`tools/ufs-super.py` validates both metadata slots and reports:

- `product_a`: 3.43 GB;
- `system_a`: 896 MB;
- `system_ext_a`: 812 MB;
- `vendor_a`: 1.15 GB;
- `odm_a` and `mi_ext_a`: small;
- `qti_dynamic_partitions_a`: 6.25 GB used, 2.24 GB uncovered;
- slot B logical partitions are empty.

All liblp geometry, header and table SHA-256 checks passed.

## Persistent-storage gates

There is no free GPT partition. There is 2.24 GB of unused space inside
`super`, but the current 2.5 GB RAM system image does not fit there unchanged.
Possible future designs, in order of risk:

1. Build a smaller HakiOS system image and allocate it entirely inside the
   unused `super` extents after an accepted liblp metadata design.
2. Use a file-backed data volume inside Android `userdata`; this needs an F2FS
   read/write path or an Android-side provisioning/boot protocol.
3. Shrink or repartition `userdata` to create a dedicated `darwin` partition.
   This is destructive and requires explicit user authorization plus a tested
   backup/recovery path.

None of these writes is enabled by the current port. The Android partitions and
all existing data remain untouched.
