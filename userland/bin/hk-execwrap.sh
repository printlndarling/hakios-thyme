#!/var/jb/bin/sh
# hk-execwrap.sh: install execwrap as an app's executable, keeping the real binary beside it.
# usage: hk-execwrap.sh APPNAME  (the .app is found by that name in the MCM containers)
name=${1:?app name}
app=$(ls -d /var/containers/Bundle/Application/*/"$name".app 2>/dev/null | head -1)
[ -n "$app" ] || { echo "no $name.app"; exit 1; }
info=/redwood/bin/plistcat
exe=$($info "$app/Info.plist" 2>/dev/null | awk -F'"' '/"CFBundleExecutable"/{print $4; exit}')
bid=$($info "$app/Info.plist" 2>/dev/null | awk -F'"' '/"CFBundleIdentifier"/{print $4; exit}')
[ -n "$exe" ] || exe=$name
[ -n "$bid" ] || bid=akylas.alpi.maps
echo "== app: $app exe: $exe id: $bid"
[ -f "$app/$exe.real" ] || mv "$app/$exe" "$app/$exe.real"
cp /redwood/bin/execwrap "$app/$exe"
chmod 755 "$app/$exe"
/redwood/share/tsldid/ldid -e "$app/$exe.real" > /private/var/tmp/"$exe".ent 2>/dev/null
/redwood/share/tsldid/ldid -S/private/var/tmp/"$exe".ent "$app/$exe" 2>/private/var/tmp/"$exe".sign.log
echo "== signed wrapper:"; ls -l "$app/$exe" "$app/$exe.real"
rm -f /private/var/tmp/execwrap.err
/var/jb/usr/bin/uicache -a
/var/jb/usr/bin/uiopen --bundleid "$bid"
sleep 8
echo "== execwrap.err:"
cat /private/var/tmp/execwrap.err 2>/dev/null | tail -80
echo "== end"
