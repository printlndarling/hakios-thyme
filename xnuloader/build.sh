#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
DEVICE=${DEVICE:-redwood}
case "$DEVICE" in
redwood) DEFS=""; DEFAULT_OUT=loader.efi ;;
thyme) DEFS="-DLOADER_THYME"; DEFAULT_OUT=loader-thyme.efi ;;
*) echo "xnuloader/build.sh: no loader platform for $DEVICE" >&2; exit 1 ;;
esac
OUT=${LOADER_OUT:-$DEFAULT_OUT}
# the iPhone's identity for XNU, from the device tree of the builder's own IPSW (not in the repository)
[ -f identity.h ] || ./mkidentity.sh

clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter $DEFS -c loader.c -o "loader-$DEVICE.obj"
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
  /out:"$OUT" "loader-$DEVICE.obj"
ls -la "$OUT"
