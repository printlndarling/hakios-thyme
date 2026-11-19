#!/var/jb/bin/sh
# hk-mapipa.sh: install the map IPA the host staged on the LAN and start it.
#
# rc.jobs.mapdiag calls this once the network is up.  tools/map-run.sh writes two
# files into the served directory before the image boots:
#   target.ipa   the IPA under test
#   target.txt   name=, bundle=, version=, appdir=, sha256=, source= (one per line)
# The digest is handed to hk_download, which compares it and returns 4 on a
# mismatch, so a truncated or replaced file stops before TrollStore's helper
# sees it.  The install itself is the same helper the store scenes used.
# Everything printed lands in /private/var/tmp/mapipa.txt, which the scene cats.
base=${1:-http://192.168.0.104:8000}
out=/private/var/tmp/mapipa.txt
exec >>"$out" 2>&1
echo "== hk-mapipa $(date)"
echo "== base $base"

/redwood/bin/hk_download "$base/target.txt" /private/var/tmp/target.txt || {
	echo "== failed: no target.txt at $base (did tools/map-run.sh stage an IPA?)"
	exit 1
}
cat /private/var/tmp/target.txt
sha=$(sed -n 's/^sha256=//p' /private/var/tmp/target.txt | head -1)
bid=$(sed -n 's/^bundle=//p' /private/var/tmp/target.txt | head -1)
appdir=$(sed -n 's/^appdir=//p' /private/var/tmp/target.txt | head -1)
echo "== bundle=${bid:-?} appdir=${appdir:-?} sha256=${sha:-?}"

if [ -n "$sha" ]; then
	/redwood/bin/hk_download "$base/target.ipa" /private/var/tmp/target.ipa "$sha"
else
	/redwood/bin/hk_download "$base/target.ipa" /private/var/tmp/target.ipa
fi
rc=$?
echo "== download rc=$rc"
[ "$rc" -eq 0 ] || exit "$rc"

echo "== install"
/var/jb/Applications/TrollStore.app/trollstorehelper install custom /private/var/tmp/target.ipa
echo "== install rc=$?"
/var/jb/usr/bin/uicache -a
echo "== uicache rc=$?"

[ -n "$bid" ] || { echo "== no bundle id in target.txt; not launched"; exit 0; }
/var/jb/usr/bin/uiopen --bundleid "$bid"
echo "== uiopen rc=$?"
sleep 12

echo "== containers (newest three)"
ls -dt /var/containers/Bundle/Application/*/*.app 2>/dev/null | head -3
if [ -n "$appdir" ]; then
	echo "== the staged bundle:"
	ls -d /var/containers/Bundle/Application/*/"$appdir" 2>/dev/null | head -2
fi
for d in /var/mobile/Library/Logs/CrashReporter /private/var/mobile/Library/Logs/CrashReporter; do
	echo "== dir $d"
	ls -la "$d" 2>/dev/null | tail -6
done
echo "== end"
