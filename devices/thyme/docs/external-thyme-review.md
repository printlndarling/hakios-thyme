# External THYME repository review

Reviewed on 2026-11-04 without adding either repository as a Git remote.

## Liquidation0619/hakios-thyme-mi10s

Reviewed at `4bb1a4e`.

Useful finding:

- The system image must retain `zh-Hans`/`zh-Hant` localization directories;
  otherwise Settings emits a language-change notification but cannot render
  Chinese.
- Its `scripts/patch-keep-lang.py` is a hard-coded edit of another checkout.
  THYME instead changed its own `sysimage/select-system.py`, retained Chinese
  resources and added PingFang Medium to the existing trimming policy.
- The automated `rc.jobs.language` gate and a physical Settings screenshot now
  prove the fix. See `CHECKLIST.md`.

Not merged:

- Its 90 Hz display work explicitly remains unfinished.
- Its storage/jailbreak/partition workflow is a different boot and persistence
  design and includes operations outside this fork's current safe RAM policy.
- Its Bluetooth and audio notes say not implemented; this fork has the more
  advanced working path.

## azzlantern/hackios-thyme

Reviewed at `be4e3ed`.

This repository documents touch, GPU and initial Wi-Fi bring-up on the older
HackiOS device-package layout. It states that charging, Bluetooth and recovery
remain unresolved, so none of those areas is a usable patch source for this
fork. Its loader and UEFI work was already reviewed during the original THYME
bring-up. No additional source was selected in this review.

## Follow-up source comparison (2026-11-04)

Both repositories were compared again at their current main branches:
Liquidation0619/hakios-thyme-mi10s at 4bb1a4e and azzlantern/hackios-thyme at
be4e3ed.

No device-driver file was copied. The overlapping THYME camera, GPU, Wi-Fi,
audio and touch files in this fork descend from the same source family and
already contain later validation work. Examples present here and absent from
the Liquidation snapshot include camera table bounds checks before following
power/table offsets, camera mode/stride checks, GLINK message length checks and
the later THYME GPU zap-window/profile selection. The Azzlantern snapshot is an
older device-package layout and its own README still lists charging, Bluetooth
and recovery as unresolved.

The one claim worth rechecking independently was Liquidation rootless Sileo
environment. It did not need to be copied: this fork already carries
tools/mk-jb.sh, the prefix graft in userland/src/init.c and the Sileo job
definitions. A fresh candidate was therefore built and tested in RAM rather
than importing their scripts.

Result on the phone:

- /var/jb was grafted from the system image with 439 directories and 6416
  files/links, with users resolvable;
- Procursus dpkg reported version 1.22.6;
- an offline tree_2.0.1_iphoneos-arm64.deb was installed, queried, executed,
  removed and queried again;
- Sileo launched, rendered its Featured page and stayed up through the capture
  window.

The offline package test now emits no missing-file-list warnings. A follow-up
fix generates .list, md5sums, conffiles, maintainer scripts and triggers for
packages merged by mk-jb.sh and jb-preinstall.py; all 70 packages in the generated
database have file lists. The builder now rejects a prefix with a package that
has no file list. See the rootless package-manager section in
CHECKLIST.md, userland/rc.jobs.jbverify and tools/jb-add-metadata.py.

Online repository installation remains PARTIAL. One focused run lost Wi-Fi at
association time (AP authentication status 1), so the offline install/remove
gate is the reproducible result. Longer Sileo repository refresh and dependency
repair also remain open.

The test uses devices/thyme/tools/prepare-jbverify.py to fetch and hash-check
the pinned package. Neither the package nor the generated prefix is committed.

## Rule for future reviews

Treat another repository as a source of isolated, testable findings, not as a
merge target. A claim is adopted only after:

1. locating the exact implementation and checking its assumptions against THYME;
2. reimplementing or cherry-picking only the relevant change;
3. rebuilding the affected artifacts;
4. validating on the physical phone in a RAM session; and
5. recording source, artifacts, log and screenshot hashes.
