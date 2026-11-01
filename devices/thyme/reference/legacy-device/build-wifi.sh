#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
export XNU_SRC="$ROOT/out/thyme/xnu"
python3 "$DEVICE_DIR/tools/stage-wifi.py"
exec "$ROOT/out/thyme/wifi-build/tools/mk-ath11k.sh" "$@"
