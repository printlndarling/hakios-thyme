# From RAM to the phone's own storage, without a brick: the order, the gates, and what has been done

[install.md](install.md) is the design and its rules. This is the order of work that follows from them, with a gate between
every two steps, and an honest account of how far it has got. The first two stages use read-only tools; the rest is a plan for a person at the desk.

## Why this order

A phone is bricked by writing the wrong thing to the chain it starts from, or by leaving it with no slot that starts. So:

1. nothing is written until what cannot be made again has been copied off and proven;
2. the system moves to the phone's own storage **before** the boot chain does: the loader is still started from RAM
   (`fastboot boot`), so a wrong system image costs a reboot into Android, not a phone;
3. the boot chain moves last, into the slot Android does **not** run from, with the other slot left exactly as it was;
4. the bootloader chain itself (`xbl`, `abl`, `tz`, `hyp` ...), the modem's and the radio's calibration, and the active slot are
   never written by this project. On Xiaomi phones a damaged bootloader chain can need an authorised service tool to revive;
   that is the one failure no step here may risk.

## The stages

| # | Stage | What happens to the phone | State |
|---|---|---|---|
| 0 | **Preflight.** Two slots? Which one is Android on? Is the bootloader really unlocked? Battery over 60% and the cable good? `darwin` already there? | nothing; reads | `make doctor` does most of it. The charge itself is still a person's check; a run that nobody watches ends at `battery_stop` (verified on hardware 2026-11-11, `logs/134145-run.log`; see "The phone that went quiet") |
| 1 | **Backup.** The boot-related partitions of both slots, `vbmeta*`, `persist`, `modemst1/2`, `fsg`, the partition tables, and with `--with-bootchain` the chain. Each copy is proven by three SHA-256s (the phone's before, the host's, the phone's after) | nothing; reads, as root | **written and tested** against a stand-in adb: `tools/backup-partitions.py` (`tools/backup-partitions-test.py`). A local thyme backup dated 2026-11-03 was rechecked offline on 2026-11-27: 40 partition files and 12 GPT records for six UFS logical units matched the manifest. All 12 GPT ranges also matched a read-only comparison against the connected thyme |
| 2 | **Carve `darwin`** off the end of Android's userdata | An authorized resize moves F2FS data and changes both GPT copies. An independently recoverable Android user-data backup is required. | Host planner tested. The non-wipe path requires a matching F2FS readback before generating either GPT file. The attached thyme currently has no `darwin`. |
| 3 | **Provision the root and data regions in `darwin`** | Writes would be limited to the approved `darwin` layout and read back. | The host packer produced the root and cryptex prefix. HFSX image generation from paired GPT snapshots and two QEMU persistence boots passed. Physical provisioning remains open. The Android data writer accepts an explicitly supplied pair of GPT snapshots and checks the live partition against them. |
| 4 | **Boot from the phone's storage, still from RAM.** `fastboot boot` the loader, and the kernel mounts its root from `darwin` (`root_apfs=`) instead of from an image uploaded to RAM | `fastboot boot` writes nothing; `ufs_write` stays 0 | QEMU mounted the root and both cryptexes from a read-only virtio disk and started SpringBoard. Thyme UFS and the physical boot arguments still need the stage 3 readback and three consecutive boots before stage 5. |
| 5 | **The boot image into the inactive slot**, `fastboot set_active` to it, once | writes the inactive slot's `boot` (and what its recipe needs); the active slot is not touched | **not done, not tried**; whether Project Silicium's boot manager behaves from a boot slot, and what `vbmeta` says to a modified boot image, are unknown |
| 6 | **A way back**, tested before it is needed: `fastboot set_active a` (or b) from the bootloader, and Android's own slot switch | writes the slot flag | the standard fastboot command; not tried here |

Stage 4 is the one that changes the project: it makes "the system lives in RAM for the length of a session" false, and it does so
without any step that can brick. A phone that boots from `darwin` by `fastboot boot` is already "not RAM"; stage 5 only removes the cable.

## Recheck a local backup

```sh
tools/backup-partitions.py /path/to/backup --verify
```

This rereads the named files, verifies their sizes and SHA-256 hashes, and
requires a head and tail record for each disk listed in the manifest. It also
reports missing core rollback partitions. Before any authorized write, compare
the backup coverage and GPT geometry with the current phone and verify that
Android's user-data backup is recoverable. On 2026-11-27 the six GPT
head/tail pairs matched the connected thyme's backup. Fastboot reported
two slots, current slot `a`, and `unlocked: yes`. Android reported
`ro.boot.flash.locked=1`. Use the bootloader's report for this preflight.
Android completed boot again after the query. The partition tool does not
back up `userdata`.

Local transcripts are `logs/thyme-install-readonly-20261127.log`
(SHA-256 `5d636fedbc6c266db6c8f0550a89a4a0aa74f1501a91e72ca772cae53f00eae5`)
and `logs/thyme-fastboot-readonly-20261127.log`
(SHA-256 `e48a9961a2361bc1de038258412467ee04e96553244c0b4ab12e43fc43cf2a3c`).

## Current thyme layout

On 2026-11-27 a read-only Android query found `userdata` on
`/dev/block/sda35` and no `/dev/block/by-name/darwin`. The host planner
was exercised on copies of the verified LUN 0 GPT backup. A hypothetical
20 GiB layout placed `darwin` on `sda36` and changed only the `userdata`
and new `darwin` entries. The two generated tables matched and all four
CRCs passed. The exercise used the planner's wipe mode on host files to
validate GPT geometry. A separate read-only F2FS snapshot put the non-wipe
candidate on `sda36` as well and found no valid blocks beyond the proposed
segment limit at the time of the snapshot. The planner emitted the resize
target and no GPT files. A device F2FS shrink and Android recovery remain
unverified. Partition provisioning requires the owner's explicit authorization.

## Prepare the APFS partition prefix on the host

Use the root and both matching cryptexes from the locally supplied 16.5
system image. The packer checks the APFS superblock and declared size of
each input against the file and writes the three complete containers in
the kernel's mount order. It produces a host file for the system prefix
of `darwin`.

```sh
python3 tools/apfs/dmg.py extract sysimage/rootfs.dmg darwin-root.apfs --apfs
python3 tools/mkflashroot.py darwin-root.apfs sysimage/cryptex-os.dmg \
    sysimage/cryptex-app.dmg darwin-system.img
```

The tested inputs start at blocks 0, 1374208 and 2039808, respectively.
The last container ends immediately before block 2042368, where HFSX
begins. The prefix file is 8,365,539,328 bytes. A prefix that would cross
the data boundary is refused.

For a host boot test, build in a separate workspace with the matching
extracted userland and locally supplied firmware. The verified recipe
used `FLASHBOOT=1 CRYPTEXES=flash SHARE=1 REST=1 GL=1 BLUETOOTH=1 ADSP=1
CHECK=1 RAM_ROOT_SLACK=128 RC_JOBS=rc.jobs.unlock`. It keeps one
compressed DSP firmware copy in Boot. Footprint for the kernel and both
containers was 56,038,408 bytes, within the loader's 63 MiB limit. The
optional package manager was omitted from this test. QEMU verified
image loading and the `/fw` mount. Phone audio, GPU and radio operation
need their own hardware tests.

```sh
python3 xnuloader/mkpayload.py -o qemu/payload.bin kernel=hakios-kernel \
    ramdisk=userland/root.apfs resident=userland/flashboot.apfs \
    --cmdline "debug=0x8 rd=md0 bootcpu=0 cpus=2 root_apfs=rvblk0 virtio_blk=1 epoch=$(date +%s)"
QEMU_SMP=2 QEMU_MEM=5G qemu/run.sh 120 -global virtio-mmio.force-legacy=false \
    -drive "file=$PWD/darwin-system.img,format=raw,if=none,id=disk,readonly=on" \
    -device virtio-blk-device,drive=disk -device ramfb
```

Check for the read-only Darwin mount, root switch, both mounts from
`/dev/rvblk0,c=1` and `/dev/rvblk0,c=2`, and the SpringBoard spawn.
Verify the prefix's SHA-256 before and after the run. Set `QEMU_ESP=1`
on the same command to serve the payload from the ESP in
[UEFI test mode](uefi-boot.md).
The phone's UFS and boot chain still require physical validation.

## Prepare the HFSX data image on the host

Pass the matched pair of planned GPT files from the carve planner or the
matched pair read back from LUN 0. The image uses the size of the APFS
partition named `darwin` and puts the journaled HFSX volume from block
2042368 through the end of that partition.

```sh
python3 tools/mkdatavol.py --gpt PLAN/gpt-head.new PLAN/gpt-tail.new data.simg
```

The host replay checked both volume headers and all sparse ranges. A
read-only HFS check passed, and the kernel kept a boot counter across two
QEMU boots on the replayed volume. The hypothetical clear-data GPT has
5,243,387 blocks in `darwin`, and the conservative F2FS candidate has
5,242,875. Generate the image for the exact GPT selected for provisioning.
An actual deployment still needs the stage 2 readback and the stage 3
provisioning gate.

## Check the planned data range in Android

After an authorized GPT change and a successful return to Android, use
the planned GPT files to verify the current device. The command reads
LUN 0 and checks the partition name, node, start and size. It checks the
SHA-256 of both eight-block GPT snapshots against the plan.

```sh
python3 tools/darwin-write.py --gpt PLAN/gpt-head.new PLAN/gpt-tail.new check
```

The current 20 GiB host plan resolves to `sda36` with 5,243,387
4 KiB blocks. Android currently lacks that partition, so the live
check refuses the planned target. In paired GPT mode, `plan`, `write`
and `zero` accept ranges from block 2042368 through the partition's
last block. The Darwin root and both cryptexes are protected. The
data writer takes raw blocks. The Android sparse HFSX image above
is a whole-partition layout and cannot be passed directly as raw data
to this writer.

## The gates

A stage is started only when the one before it has a recorded result and a person has read it:

- before stage 2: `manifest.json` of stage 1 exists and every partition in it is proven; a copy of the phone's data exists;
- before stage 3: the new GPT was read back from the phone and equals what the carve tool made; Android boots and its data is there;
- before stage 5: stage 4 has booted the system from `darwin` at least three times in a row, and the log says so;
- before every write: `tools/provenance.allow` lists the writer (CI fails if a writer appears that is not listed).

## The phone that went quiet

Writing this, the phone vanished from USB: not adb, not fastboot, not Qualcomm's emergency mode. The last run before that ended
normally and left the phone in the loader. The likeliest cause is the **battery**: nothing charges the phone while it sits in the
loader (the kernel's charging path is not written; the UEFI shell does not charge), the screen was on for hours, and a long session
is a flat battery. If so, plugging it into a charger for a few minutes brings it back to Android by itself. That has not been
confirmed. It is why stage 0 has a battery gate, and why an unattended run is not a safe thing to leave overnight until the kernel
reads the battery and stops the run. That guard now exists and is verified: the loader's charging bit follows the cell's voltage
(a cable attached is not charging), and with `battery_stop=25 battery_stop_poll=30`, which `devices/thyme/ram.args` now carries,
an unattended run hands back to the loader before the cell is empty (forced at 95% in `logs/134145-run.log`). It hands back to the
**loader**, not to Android: only the host's `XL_REBOOT_AFTER=1` sends the phone where it charges, so a run is still not something
to leave with nobody there to take it back.

## What the user decides

1. Whether to give up some of the phone's storage (stage 2): the size of `darwin`. The system image alone is 2.5 GB; room to live in
   is more.
2. Whether Android's data may be disturbed at all. If not, stages 2 to 5 wait for a second phone or a spare partition.
3. Who is at the desk for stage 5.

## What was not done tonight, on purpose

No partition was written, no slot was switched, `darwin` was not made, and the installer was not written: an installer that
has never run on a phone, written while its owner sleeps, would be a guess with a progress bar. The tools above are the part
that could be built and tested without a phone, and they are.
