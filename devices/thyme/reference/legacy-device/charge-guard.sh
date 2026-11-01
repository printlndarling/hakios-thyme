#!/bin/bash
# Return a RAM-only HakiOS session to Android after a bounded window, then leave
# the phone in Android to charge. The target must be named explicitly so another
# adb/fastboot device on the host can never be touched.
set -euo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
OUT="$ROOT/out/thyme"
PIDFILE="$OUT/charge-guard.pid"
LOGFILE="$OUT/charge-guard.log"
LOADER_SERIAL="${LOADER_SERIAL:-xnuloader}"
PHONE_SERIAL="${PHONE_SERIAL:-}"
FB="${FB:-$(command -v fastboot.exe || command -v fastboot || echo fastboot)}"
ADB="${ADB:-$(command -v adb.exe || command -v adb || echo adb)}"

if [ "${1:-}" = cancel ]; then
  if [ -s "$PIDFILE" ]; then
    pid="$(cat "$PIDFILE")"
    kill "$pid" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "charge-guard: cancelled pid $pid"
  else
    echo "charge-guard: no active timer"
  fi
  exit 0
fi

minutes="${1:-20}"
case "$minutes" in
  ''|*[!0-9]*) echo "usage: $0 [minutes|cancel]" >&2; exit 2 ;;
esac
[ -n "$PHONE_SERIAL" ] || {
  echo "PHONE_SERIAL must name the target Android serial" >&2
  exit 2
}

if [ -s "$PIDFILE" ]; then
  old="$(cat "$PIDFILE")"
  if kill -0 "$old" 2>/dev/null; then
    echo "charge-guard: already active as pid $old"
    exit 0
  fi
  rm -f "$PIDFILE"
fi

mkdir -p "$OUT"
nohup env LOADER_SERIAL="$LOADER_SERIAL" PHONE_SERIAL="$PHONE_SERIAL" FB="$FB" ADB="$ADB"   bash "$DEVICE_DIR/charge-guard-run.sh" "$minutes" "$PIDFILE" "$LOGFILE"   >>"$LOGFILE" 2>&1 &
pid="$!"
echo "$pid" >"$PIDFILE"
echo "charge-guard: pid $pid will return the phone to Android in $minutes minutes"
