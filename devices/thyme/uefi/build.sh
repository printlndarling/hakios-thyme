#!/bin/bash
# Build the staged THYME Mu-Silicium tree. Does not pack or boot an image.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MU=${MU:-$ROOT/Mu-Silicium}
PYTHON=${UEFI_PYTHON:-$ROOT/venv-uefi/bin/python}

[ -d "$MU" ] || { echo "thyme-uefi: run apply.sh first" >&2; exit 1; }
if [ ! -x "$PYTHON" ]; then
	python3 -m venv "$ROOT/venv-uefi"
fi
"$PYTHON" -m pip install -q --disable-pip-version-check \
	-r "$MU/pip-requirements.txt" uefi_firmware==1.16

DEP=$MU/Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p "$DEP/Linux-x86-64"
printf 'version: 20016.1.1\n' > "$DEP/extdep_state.yaml"
ln -sfn "$(command -v nasm)" "$DEP/Linux-x86-64/nasm"
ln -sfn "$(command -v ndisasm)" "$DEP/Linux-x86-64/ndisasm"

"$PYTHON" "$MU/Platforms/Xiaomi/umiPkg/DeviceBuild.py" --update -t RELEASE
"$PYTHON" "$MU/Platforms/Xiaomi/umiPkg/DeviceBuild.py" TARGET=RELEASE \
	ENABLE_SECUREBOOT=0 FD_BASE=0x9fc00000 FD_SIZE=0x300000 FD_BLOCKS=0x300 DEVICE_MODEL=0
sha256sum "$MU/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
