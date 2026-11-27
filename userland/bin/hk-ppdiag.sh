#!/var/jb/bin/sh
# hk-ppdiag.sh: why PPSSPP aborts at launch.  Expects PPSSPP already installed and started
# once by uiopen.  Dumps any crash report ReportCrash left, then runs the app binary directly
# with its stderr merged, which is where dyld or a graphics/entitlement refusal says why.
app=$(ls -d /var/containers/Bundle/Application/*/PPSSPP.app 2>/dev/null | head -1)
echo "== app: ${app:-none}"
for d in /var/mobile/Library/Logs/CrashReporter /var/logs/CrashReporter /Library/Logs/CrashReporter /private/var/mobile/Library/Logs/CrashReporter; do
	echo "== dir $d"
	ls -la "$d" 2>/dev/null | tail -8
done
for f in /var/mobile/Library/Logs/CrashReporter/*PPSSPP* /private/var/mobile/Library/Logs/CrashReporter/*PPSSPP*; do
	[ -f "$f" ] || continue
	echo "== report $f"
	cat "$f"
done
[ -n "$app" ] || exit 0
echo "== direct run with stderr, 5 s:"
"$app/PPSSPP" 2>&1 &
pid=$!
sleep 5
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "== direct run done"
