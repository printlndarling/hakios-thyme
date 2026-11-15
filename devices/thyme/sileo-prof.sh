#!/bin/bash
# One Sileo dependency-install run with the main thread sampled for the whole scene:
# the queue's slow stretch (36.5 frames a second at 596.9 s, logs/070701-run.log) is the
# application's own commits, and the sampler in libhakios.app.dylib (appprof_secs=N) is how
# its hot sites are named.
#
#   devices/thyme/sileo-prof.sh
#
# The scene is the dependency install (wget and its six dependencies, root-sileodep.apfs);
# sileo.sh does the rest -- the jb system image, the Wi-Fi join, the taps and the checks.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export SCENE=${SCENE:-out/thyme-boot/root-sileodep.apfs}
export SILEO_SECS=${SILEO_SECS:-1000}
export EXTRA_ARGS=${EXTRA_ARGS:-appprof=1 appprof_secs=900 diag_mask=1}
[ -f "$SCENE" ] || {
	echo "sileo-prof: no $SCENE; build it: SCENE=sileodep EXTRA=sileo-seed.sh devices/thyme/build-root.sh" >&2
	exit 1
}
echo "sileo-prof: $SILEO_SECS s of the dependency scene, the main thread sampled for 900 s ($EXTRA_ARGS)"
exec devices/thyme/sileo.sh
