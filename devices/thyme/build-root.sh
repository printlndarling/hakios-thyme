#!/bin/bash
# Build the canonical THYME RAM root image. Every optional component a
# daily-driver image needs is named here: GL userspace, the audio DSP, the
# QCA Bluetooth launch set, the on-demand Darwin jobs and the scenario.
# check-root.py refuses the result if any of them silently disappeared.
#
#   devices/thyme/build-root.sh
#   SCENE=jiggle devices/thyme/build-root.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCENE=${SCENE:-wirelessfull}
RC_JOBS=${RC_JOBS:-rc.jobs.$SCENE}
cd "$ROOT/userland"

export REST=1
export BLUETOOTH=1
export ADSP=1
export GL=1
export RC_JOBS
/bin/bash ./build.sh

cd "$ROOT"
python3 devices/thyme/check-root.py userland/root "$RC_JOBS"
cp userland/root.apfs "out/thyme-boot/root-$SCENE.apfs"
sha256sum "out/thyme-boot/root-$SCENE.apfs"
