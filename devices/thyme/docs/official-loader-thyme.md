# Official THYME loader port

Date: 2026-11-01

## Problem

The official loader source currently kept only the redwood and QEMU platform
descriptions. A loader built for THYME therefore ran with the QEMU platform,
tried the wrong display path and did not reach its USB command endpoint.

The old THYME boot image used the legacy loader, which still had compile-time
THYME support.

## Fix

`xnuloader/loader.c` now has a compile-time `LOADER_THYME` platform:

- SM8250 peripheral window, GIC addresses, eight cores and the A55 core mask;
- GOP validation for the THYME 1080 x 2340 panel;
- THYME included in the charger/status and virtual-watchdog conditions;
- THYME serial fields in the generated device tree;
- THYME published as `/chosen/hakios-board`.

The last item is a hardware gate, not an identity detail. Without it the
FTS521 touch driver, PM8150 key driver, display/thermal path and QCA6390 Wi-Fi
path all report a board mismatch and stay disabled. Commit `66500d7` fixes the
official loader after the regression reproduced on hardware.

`xnuloader/build.sh` accepts `DEVICE=thyme` and emits
`xnuloader/loader-thyme.efi`. Redwood remains the default.

`mkxnuimg.py` now accepts a device-specific UEFI FD, DTB, boot shim and memory
addresses. THYME uses:

```text
FD_BASE=0x9fc00000
FD_SIZE=0x300000
PAYLOAD_BASE=0xf0000000
```

`devices/thyme/tools/bootshim.S` is built with those values, and
`devices/thyme/build-loader.sh` creates:

```text
devices/thyme/prebuilt/xnu-usb.img
```

The previously verified image is preserved outside Git at:

```text
/home/darling/thyme-assets/xnu-usb-legacy-known-good.img
sha256 a7061100f88135e5887057bebca24e242c0c7eebd57047888ba81ef743b029cb
```

## Verification status

- Redwood loader build passes.
- THYME loader build passes with `LOADER_THYME`.
- THYME boot image packing passes.
- The loader reaches USB and boots the kernel on hardware.
- The post-`hakios-board` image is built and ready for a RAM test. Its hardware
  result is pending; do not mark touch, keys, brightness or Wi-Fi as validated
  until the new run logs those drivers starting.
