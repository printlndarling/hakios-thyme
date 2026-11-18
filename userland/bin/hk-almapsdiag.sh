#!/var/jb/bin/sh
# hk-almapsdiag.sh: why Alpimaps aborts at launch.  Expects alpimaps already installed and
# started once by uiopen.  Dumps any crash report ReportCrash left, then runs the app binary
# directly with its stderr merged; an uncaught NSException prints its name and reason there.
app=$(ls -d /var/containers/Bundle/Application/*/alpimaps.app 2>/dev/null | head -1)
echo "== app: ${app:-none}"
for d in /var/mobile/Library/Logs/CrashReporter /private/var/mobile/Library/Logs/CrashReporter /var/logs/CrashReporter; do
	echo "== dir $d"
	ls -la "$d" 2>/dev/null | tail -8
done
for f in /var/mobile/Library/Logs/CrashReporter/*alpi* /private/var/mobile/Library/Logs/CrashReporter/*alpi*; do
	[ -f "$f" ] || continue
	echo "== report $f"
	cat "$f"
done
[ -n "$app" ] || exit 0
echo "== Info.plist capabilities:"
/redwood/bin/plistcat "$app/Info.plist" 2>/dev/null | grep -A6 -i 'UIRequiredDeviceCapabilities' | head -12
echo "== direct run with stderr, 6 s:"
"$app/alpimaps" 2>&1 &
pid=$!
sleep 6
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "== direct run done"
