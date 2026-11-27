#!/var/jb/bin/sh
# hk-appinstall.sh: put a third-party app where launchd will take it.
#
# Why a script: a list's ">> FILE" is only understood as the last two words of a line
# (src/init.c), so a line that ends in ">> FILE 2>&1" silently loses both the append and
# its extra words to the program.  dash does the redirection properly.
#
# And why the files are replaced rather than copied over: /var/jb is a graft (src/init.c,
# "the rootless prefix"): every directory is real but every file is a symbolic link into
# the read-only /jb, so "chmod /var/jb/.../iSH" (or cp through it) writes through the link
# and fails EROFS.  That is what kept iSH at mode 644 in /jb and made launchd's
# access(..., X_OK) fail with EACCES.  Removing the link and copying the real file in its
# place makes the executable a real, executable file in memory.
#
# usage: hk-appinstall.sh APP BUNDLEID [FILE...]
#   APP     the bundle's name in /Applications, e.g. iSH.app
#   FILE    a path inside the bundle that must be executable; default is the list below
# The log is /private/var/tmp/inst.log; the caller cats it at the end of the scene.
log=/private/var/tmp/inst.log
exec >>"$log" 2>&1

app=$1
bid=$2
src=/Applications/$app
dst=/var/jb/Applications/$app

echo "== hk-appinstall $app ($bid) $(date)"

shift 2
if [ "$#" -eq 0 ]; then
	case "$app" in
	iSH.app) set -- iSH PlugIns/iSHFileProvider.appex/iSHFileProvider ;;
	TrollStore.app) set -- TrollStore PersistenceHelper trollstorehelper ;;
	esac
fi

if [ ! -d "$dst" ]; then
	mkdir -p /var/jb/Applications
	# -R into a name that does not exist yet: a real copy of the whole bundle, not a graft
	cp -R "$src" "$dst"
	echo "copied the bundle: $?"
fi

for f in "$@"; do
	if [ -L "$dst/$f" ] || [ -e "$dst/$f" ]; then
		rm -f "$dst/$f"
		cp "$src/$f" "$dst/$f"
	else
		mkdir -p "$(dirname "$dst/$f")"
		cp "$src/$f" "$dst/$f"
	fi
	chmod 755 "$dst/$f"
	ls -l "$dst/$f"
done

if [ "${NO_UICACHE:-0}" != "1" ]; then
	/var/jb/usr/bin/uicache -a
	echo "uicache: $?"
fi
exit 0
