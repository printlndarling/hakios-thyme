#!/var/jb/bin/sh
# hk-appjb.sh: a copy of an app that is already in /Applications, under a bundle id of its own.
#
# For the apps the system image already carries: LaunchServices finds
# /Applications/<app>.app first and launches THAT binary, which lives on a read-only volume and
# cannot be chmod'ed (iSH's stayed 644, so xpcproxy's access(X_OK) said EACCES).  A copy with a
# new CFBundleIdentifier has no twin in /Applications, so uicache registers the copy under
# /var/jb/Applications and launchd runs the real file the copy holds.
#
# usage: hk-appjb.sh SRCAPP DSTNAME NEWID PLIST EXEC...
#   SRCAPP   name in /Applications, e.g. iSH.app
#   DSTNAME  name for the copy in /var/jb/Applications, e.g. iSHjb.app
#   PLIST    the copy's Info.plist, made on the build host with the new id (XML is fine)
#   EXEC     executables inside the bundle to chmod 755
log=/private/var/tmp/inst.log
exec >>"$log" 2>&1

src=/Applications/$1
dst=/var/jb/Applications/$2
newid=$3
plist=$4
shift 4

echo "== hk-appjb $src -> $dst ($newid) $(date)"
rm -rf "$dst"
cp -R "$src" "$dst"
echo "copied the bundle: $?"
cp "$plist" "$dst/Info.plist"
echo "Info.plist replaced: $?"
for f in "$@"; do
	chmod 755 "$dst/$f"
	ls -l "$dst/$f"
done
/var/jb/usr/bin/uicache -a
echo "uicache: $?"
exit 0
