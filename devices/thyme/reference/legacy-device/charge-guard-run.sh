#!/bin/bash
set -euo pipefail
minutes="$1"
pidfile="$2"
logfile="$3"
trap 'rm -f "$pidfile"' EXIT
sleep "$((minutes * 60))"

echo "$(date -Is) charge-guard: timer expired after $minutes minutes"
if "$FB" devices | grep -q "^${LOADER_SERIAL}[[:space:]]"; then
  echo "$(date -Is) charge-guard: returning loader session to Android"
  "$FB" -s "$LOADER_SERIAL" reboot
else
  echo "$(date -Is) charge-guard: loader $LOADER_SERIAL is not present; nothing to reboot"
fi

state=""
for i in $(seq 1 120); do
  state="$(timeout 8 "$ADB" -s "$PHONE_SERIAL" get-state 2>/dev/null || true)"
  [ "$state" = device ] && break
  if [ $((i % 6)) -eq 0 ]; then
    echo "$(date -Is) charge-guard: waiting for Android ($((i * 5)) s)"
  fi
  sleep 5
done
if [ "${state:-}" = device ]; then
  echo "$(date -Is) charge-guard: Android is up; battery follows"
  "$ADB" -s "$PHONE_SERIAL" shell dumpsys battery || true
else
  echo "$(date -Is) charge-guard: Android did not appear within ten minutes; adb devices follows" >&2
  timeout 8 "$ADB" devices -l || true
  exit 1
fi
