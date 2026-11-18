#!/var/jb/bin/sh
# hk-lpver.sh: the version of every LocProbe.app under the MCM containers, with its path, so
# an update run's log says which version the store left installed.  plistcat prints the
# bundle's Info.plist as XML, which the console log keeps.
for f in /var/containers/Bundle/Application/*/LocProbe.app/Info.plist; do
	[ -f "$f" ] || continue
	echo "== $f"
	/redwood/bin/plistcat "$f" | grep -A1 CFBundleShortVersionString
done
