# Migration from the legacy fork

The former third-party fork is not a Git ancestor of this repository. Its source
snapshot is copied to `reference/legacy-device/` so that hardware work is not
lost. Nothing in that directory is built by the official tree.

## Migration order

1. **Official core boot**: build the current minimal configuration, package it
   with the known-good RAM loader, and confirm that XNU reaches launchd and
   returns to Android.
2. **Board and CPU**: port THYME SoC discovery, RPMh command DB, seven cores,
   CPU frequency, and thermal readings.
3. **Display and input**: port the THYME display scan/command-mode path, Adreno
   650 GPU, FTS521 touch, PM8150 keys, display-off/wake.
4. **Power**: port the THYME battery provider, battguard, and the loader's
   current-aware one-shot Qualcomm `TakeAction` handling.
5. **Audio and haptics**: port ADSP, speaker amplifiers, microphone, and AW8697.
6. **Wireless**: finish QCA6390 WLFW/QMI, station creation, the QCA6390 Bluetooth
   UART transport, and HCI reset.
7. **Remaining hardware**: ALS, accelerometer/proximity, camera, torch, UFS,
   cellular, NFC, and fingerprint.

## Source mapping

| Legacy source | Official destination |
|---|---|
| `overlays/xnu/files/bsd/dev/hakios.board.c` | device driver + `drivers/` configuration |
| `overlays/xnu/files/bsd/dev/hakios.display.c` | device display driver |
| `overlays/xnu/files/bsd/dev/umi_touch.c` | FTS521 device driver |
| `overlays/xnu/files/bsd/dev/umi_keys.c` | PM8150 button adaptation |
| `overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | common candidate or device override |
| `overlays/xnu/files/bsd/dev/hakios.adreno.c` | Adreno 650 device adaptation |
| `overlays/xnu/files/bsd/dev/hakios.battery.c` | THYME battery provider |
| `loader/loader.c` | official loader feature parity |
| `uefi/files/` | official `hakios-uefi` device platform |

## Deletion gate for legacy local repositories

The old local checkouts may be removed only after all of the following:

- `tools/hakios-device.py check thyme` passes in this repository.
- The official-main kernel links successfully.
- The official-main kernel RAM-boots the phone through the selected UEFI/loader.
- This repository contains the legacy source snapshot and porting documents.
- Firmware, DTB, signing inputs, and any uncommitted local changes have been
  copied to the new workspace or an external archive and verified by SHA-256.
- The new repository is pushed and `git status` is clean.
