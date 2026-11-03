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

## Rule for future reviews

Treat another repository as a source of isolated, testable findings, not as a
merge target. A claim is adopted only after:

1. locating the exact implementation and checking its assumptions against THYME;
2. reimplementing or cherry-picking only the relevant change;
3. rebuilding the affected artifacts;
4. validating on the physical phone in a RAM session; and
5. recording source, artifacts, log and screenshot hashes.
