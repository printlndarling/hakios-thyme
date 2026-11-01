#!/bin/bash
# Build the THYME loader and RAM-boot image. Nothing is written to the phone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/devices/thyme/prebuilt/xnu-usb.img"
PAYLOAD=$(mktemp "$ROOT/devices/thyme/prebuilt/.usb-payload.XXXXXX")
trap 'rm -f "$PAYLOAD"' EXIT

cd "$ROOT"
DEVICE=thyme xnuloader/build.sh
python3 xnuloader/mkpayload.py -o "$PAYLOAD" \
	loader.efi="$ROOT/xnuloader/loader-thyme.efi" --usb
python3 mkxnuimg.py -d thyme \
	--fd-base 0x9fc00000 --fd-size 0x300000 --payload-base 0xf0000000 \
	-p "$PAYLOAD" -o "$OUT"
sha256sum "$OUT"
