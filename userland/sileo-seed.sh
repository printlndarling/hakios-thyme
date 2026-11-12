#!/bin/sh
# Sileo learns a repository's name from the cached Release file
# (RepoManager._checkUpdatesInBackground reads cacheFile("Release") and takes its
# "origin"), and the cache is the same /var/jb/var/lib/apt/lists that apt writes.
# apt stores the repository's signed metadata as <repo>_InRelease; Sileo asks for
# <repo>_Release and, finding none, shows the repository as "Untitled Repo" (and
# the queue path that follows loses its source). Give Sileo the same bytes under
# the name it asks for, once apt-get update has fetched them.
#
# Run it in the scene as: /var/jb/bin/sh /redwood/bin/sileo-seed.sh
lists=/var/jb/var/lib/apt/lists
[ -d "$lists" ] || {
	echo "sileo-seed: no apt lists at $lists"
	exit 0
}
found=0
for f in "$lists"/*_InRelease; do
	[ -f "$f" ] || continue
	base=${f%_InRelease}
	if [ -f "${base}_Release" ]; then
		continue
	fi
	cp "$f" "${base}_Release" && echo "sileo-seed: $(basename "${base}_Release")"
	found=$((found + 1))
done
echo "sileo-seed: $found Release file(s) written for Sileo"
