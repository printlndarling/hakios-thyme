#!/var/jb/bin/sh
# hk-almapsrun.sh: run Alpimaps' binary as uid 501, outside SpringBoard, with its stderr kept.
# The scene already installed it.  If the binary dies here too, the environment that breaks it
# is the user's (uid/groups); if it runs, what breaks it is SpringBoard's launch itself.
app=$(ls -d /var/containers/Bundle/Application/*/alpimaps.app 2>/dev/null | head -1)
echo "== app: ${app:-none}"
[ -n "$app" ] || exit 1
rm -f /private/var/tmp/alpi.err
/redwood/bin/runas 501 /private/var/tmp/alpi.err "$app/alpimaps" &
pid=$!
sleep 8
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "== stderr:"
tail -60 /private/var/tmp/alpi.err 2>/dev/null
echo "== end"
