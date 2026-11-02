#!/bin/bash
# Build an isolated THYME Mu-Silicium source tree without pulling OpenSSL test
# submodules. No phone storage is touched.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MU=${MU:-$ROOT/Mu-Silicium}
COMMON=$ROOT/uefi
DTB=${THYME_DTB:-/home/darling/thyme-assets/thyme.dtb}
XBL=${THYME_XBL:-/home/darling/thyme-assets/staged/boot/xbl_a.img}
PYTHON=${UEFI_PYTHON:-$ROOT/venv-uefi/bin/python}

[ -d "$COMMON/.git" ] || "$ROOT/tools/hakios-device.py" sync hakios-uefi
[ -f "$DTB" ] || { echo "thyme-uefi: no device tree: $DTB" >&2; exit 1; }
[ -f "$XBL" ] || { echo "thyme-uefi: no XBL image: $XBL" >&2; exit 1; }
[ ! -e "$MU" ] || { echo "thyme-uefi: $MU already exists" >&2; exit 1; }

git clone https://github.com/Project-Silicium/Mu-Silicium.git "$MU"
git -C "$MU" checkout -q "$(tr -d '\r\n' < "$COMMON/BASE")"
git -c http.version=HTTP/1.1 -C "$MU" submodule update --init --depth 1 \
	Binaries Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicium-ACPI \
	Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
git -c http.version=HTTP/1.1 -C "$MU/Mu_Basecore" submodule update --init --depth 1 \
	BaseTools/Source/C/BrotliCompress/brotli \
	MdeModulePkg/Library/BrotliCustomDecompressLib/brotli \
	MdeModulePkg/Universal/RegularExpressionDxe/oniguruma \
	MdePkg/Library/BaseFdtLib/libfdt

git -C "$MU" apply --whitespace=nowarn "$COMMON/hakios.patch"
cp -r "$COMMON/files/." "$MU/"
python3 "$COMMON/branding/banner.py" "$MU/Silicon/Silicium/SiliciumPkg/Include/Resources"
cp -r "$HERE/files/." "$MU/"
cp "$DTB" "$MU/Resources/DTBs/umi.dtb"

MU="$MU" "$PYTHON" "$HERE/extract.py" "$XBL"
echo "thyme-uefi: staged at $MU"
