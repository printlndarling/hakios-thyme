#!/var/jb/bin/sh
# The apt query Sileo's recheckTotalOps() makes, answered from both candidate
# working directories, with and without the one-line link at "/".
# Sileo spawns apt as root with a relative -oDir::State::lists=sileolists/ and
# never chdir()s; if the inherited cwd is "/" the relative path is /sileolists,
# not the directory Sileo prepared, and apt answers "Unable to locate package"
# -- which is exactly a thrown error and an emptied queue.
lists=/var/jb/var/lib/apt/sileolists
ap() {
	out=$(/var/jb/usr/bin/apt-get install -s -f -oDir::State::lists=sileolists/ tree 2>&1)
	rc=$?
	echo "$out" | head -5
	echo "sileolists-probe:   rc=$rc"
}
echo "sileolists-probe: --- start"
if [ -d "$lists" ]; then
	echo "sileolists-probe: Sileo's lists directory:"
	ls -la "$lists" 2>&1 | head -6
else
	echo "sileolists-probe: no $lists"
fi
rm -f /sileolists
echo "sileolists-probe: A) from /var/jb/var/lib/apt, no link:"
cd /var/jb/var/lib/apt 2>/dev/null || cd /
ap
echo "sileolists-probe: B) from /, no link (what Sileo's spawn sees):"
cd /
ap
ln -sfn "$lists" /sileolists
echo "sileolists-probe: C) from /, link $(ls -ld /sileolists 2>&1):"
ap
echo "sileolists-probe: --- done"
