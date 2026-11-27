#!/var/jb/bin/sh
# hk-tsldid.sh: put ldid inside the TrollStore.app that TrollStore itself lives in.
#
# Why: the helper's isLdidInstalled() asks trollStoreAppPath() for it, and that path is the MCM
# app container of com.opa334.TrollStore (trollStorePath() -> MCMAppContainer), not the copy a
# scene drops into /var/jb/Applications.  After `trollstorehelper install-trollstore` has made
# that container, this finds the bundle there and copies ldid into it (and into the /var/jb copy
# for good measure, where an older helper would look).
log=/private/var/tmp/tstore.txt
exec >>"$log" 2>&1
echo "== hk-tsldid $(date)"

for app in /var/containers/Bundle/Application/*/TrollStore.app; do
	[ -d "$app" ] || continue
	cp /redwood/share/tsldid/ldid "$app/ldid"
	chmod 755 "$app/ldid"
	ls -l "$app/ldid"
	echo "container bundle: $app"
done

cp /redwood/share/tsldid/ldid /var/jb/Applications/TrollStore.app/ldid
chmod 755 /var/jb/Applications/TrollStore.app/ldid
ls -l /var/jb/Applications/TrollStore.app/ldid
exit 0
