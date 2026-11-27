# Installing HakiOS on a phone: the design, and what is missing

This design covers an installation alongside Android. See
[the installation verification record](install-safe.md) for device-specific
results. On the attached thyme, RAM boot and Android return are verified.
`darwin` provisioning and independent installation remain open.

## Today

HakiOS is booted from RAM using `fastboot boot`, and the timed test
workflow returns to Android. The rebuilt thyme loader retains diagnostic
logs in RAM. The opt-in `ufs_write=1` path is limited to a compatible
`darwin` partition. The complete list of writers is recorded in
`tools/provenance.allow`.

## What "installed" would mean

Android stays, HakiOS sits beside it and is chosen at boot, and going back is a slot switch. Two things have to live
on the phone:

1. **The boot chain.** The boot image `./xl enter` builds (`mkxnuimg.py`: Project Silicium's UEFI, the shim and a
   payload) already starts with no host: the shim puts the payload in memory and the loader runs it, in one-shot mode
   when the payload carries no `usb` entry. Installed, that same kind of image would sit in the boot partition of the
   **inactive** slot.
2. **The system.** The partition `darwin`, cut off the end of Android's userdata once
   (`tools/userdata-carve.py` validates both GPT copies and checks the F2FS resize readback).
   Provisioning the root image from the flash recipe (`FLASHBOOT=1 userland/build.sh`)
   still needs a guarded path for the actual thyme layout. `tools/darwin-write.py` is
   limited to the data range of the legacy geometry.

`make doctor` already looks at what this stands on, read only: whether the phone reports two slots, which one Android
runs from, and whether `darwin` exists.

## Rules an install must keep

Each one is how the project already treats writes (`darwin-write.py`, the loader's refusals), carried over:

1. **Look before anything is written, and keep what was there.** Slots, partition table and free space are read first;
   before a write, the partitions that will be touched, the GPT head and tail and the boot and `vbmeta` partitions
   are copied off the phone and hashed twice.
2. **Only the inactive slot.** Never the slot Android runs from. No A/B, no install.
3. **Never the partitions that cannot be made again**: the bootloader chain, the modem's and radio's calibration, and
   the phone's own identity data.
4. **Every write is read back and compared**, and a mismatch stops everything.
5. **A way back that does not need this host**: choosing Android's slot again, with the standard `fastboot set_active`.
6. **Dry run by default.** Each step is printed with the checks it made, one at a time; a person types the
   confirmation for each; nothing is done on a script's own say-so.
7. **Everything is logged**, refusals too.

## What exists and what does not

| Needed | State |
|---|---|
| A boot image that starts with no host (shim + payload) | exists (`mkxnuimg.py`); the loader started from it by `fastboot boot` on the attached phone |
| Preparing and provisioning `darwin` | The HFSX data image can be built from paired GPTs and has passed host replay and QEMU tests. The Android data-range writer supports the legacy layout. Root and cryptex provisioning on thyme remain open. |
| A plan for carving `darwin` | Host-only planner and tests. The resize path requires a matching F2FS readback before emitting GPT files. |
| Read-only preflight | exists (`make doctor`): slots, `darwin`, firmware, root, bootloader as reported |
| **Writing the boot image to a slot and switching to it** | **not done, not tried** |
| **Booting the flash recipe's root from `darwin`** | **not verified** (the arguments it needs are not confirmed) |
| **Project Silicium's Boot Manager behaving the same from a boot slot** | **unknown** |
| **Verified boot (`vbmeta`) for a modified boot image** | **unknown** |
| An installer that chains the steps under the rules above | not written |

## Why there is no installer yet

A wrong write can leave a phone that does not boot, and an unlocked bootloader only makes the mistake possible, not
recoverable. An installer that has never been run on a phone, written without one on the desk, would be a guess
with a progress bar. What can be built and checked first is built and checked: the read-only preflight, the guarded
writer, the plans. The slot step comes when a phone is attached, a spare slot is confirmed, backups exist and a
person is watching.
