#!/var/jb/bin/sh
# hk-pkgsetup.sh: one-shot boot setup so Sileo, TrollStore, TrollApps, and iSH work
# cleanly out of the box on every boot (including daily wirelessfull) with curated
# mainland-China-accessible Rootless Sileo repos and TrollApps JSON sources.
log=/private/var/tmp/pkgsetup.log
exec >>"$log" 2>&1

say() {
	echo "$*"
	echo "$*" > /dev/console 2>/dev/null || true
}

say "pkgsetup: start $(date)"

# Wait briefly for /var/jb graft to settle
i=0
while [ ! -x /var/jb/usr/bin/uicache ] && [ "$i" -lt 20 ]; do
	sleep 1
	i=$((i + 1))
done

# 0. Bootstrap virtual firmware/iOS packages & user db (as /var/jb/prep.sh does)
if [ -x /var/jb/usr/libexec/firmware ]; then
	/var/jb/usr/libexec/firmware || true
fi
if [ -x /var/jb/usr/sbin/pwd_mkdb ] && [ -f /var/jb/etc/master.passwd ]; then
	/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd 2>/dev/null || true
fi

# 1. Sileo: seed curated CN + core Rootless repos & default Chinese/no-Canister prefs
if [ -d /var/jb/etc/apt/sources.list.d ]; then
	if [ -f /redwood/share/sileo/sileo.sources ] && [ ! -f /var/jb/etc/apt/sources.list.d/sileo.sources ]; then
		/var/jb/usr/bin/cp /redwood/share/sileo/sileo.sources /var/jb/etc/apt/sources.list.d/sileo.sources
		/var/jb/usr/bin/chmod 644 /var/jb/etc/apt/sources.list.d/sileo.sources
		say "pkgsetup: seeded /var/jb/etc/apt/sources.list.d/sileo.sources"
	fi
fi
if [ -x /redwood/bin/sileo-seed.sh ]; then
	/var/jb/bin/sh /redwood/bin/sileo-seed.sh || true
fi

mkdir -p /var/mobile/Library/Preferences
if [ ! -f /var/mobile/Library/Preferences/org.coolstar.SileoStore.plist ]; then
	cat > /var/mobile/Library/Preferences/org.coolstar.SileoStore.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>ShowProvisional</key>
	<false/>
	<key>UseSystemLanguage</key>
	<true/>
	<key>CanisterIngest</key>
	<false/>
</dict>
</plist>
PLIST
	/var/jb/usr/bin/chown 501:501 /var/mobile/Library/Preferences/org.coolstar.SileoStore.plist
	/var/jb/usr/bin/chmod 644 /var/mobile/Library/Preferences/org.coolstar.SileoStore.plist
	say "pkgsetup: seeded org.coolstar.SileoStore.plist"
fi

# 2. TrollStore: real 0755 executables, ldid + ldid.version in BOTH /var/jb & MCM container
LDID_VER="v2.1.5-procursus7-new"
if [ -d /Applications/TrollStore.app ]; then
	NO_UICACHE=1 /var/jb/bin/sh /redwood/bin/hk-appinstall.sh TrollStore.app com.opa334.TrollStore
	if [ -f /redwood/share/tsldid/ldid ] && [ -d /var/jb/Applications/TrollStore.app ]; then
		/var/jb/usr/bin/cp /redwood/share/tsldid/ldid /var/jb/Applications/TrollStore.app/ldid
		/var/jb/usr/bin/chmod 755 /var/jb/Applications/TrollStore.app/ldid
		printf '%s' "$LDID_VER" > /var/jb/Applications/TrollStore.app/ldid.version
		/var/jb/usr/bin/chmod 644 /var/jb/Applications/TrollStore.app/ldid.version
	fi
	# Wait for containermanagerd so MCM container + ldid are created reliably
	k=0
	mcm_ok=0
	while [ "$k" -lt 45 ]; do
		if /redwood/bin/mcm_test com.opa334.TrollStore /var/jb/Applications/TrollStore.app /redwood/share/tsldid/ldid; then
			mcm_ok=1
			break
		fi
		sleep 2
		k=$((k + 1))
	done
	for app in /var/containers/Bundle/Application/*/TrollStore.app; do
		[ -d "$app" ] || continue
		/var/jb/usr/bin/cp /redwood/share/tsldid/ldid "$app/ldid"
		/var/jb/usr/bin/chmod 755 "$app/ldid" "$app/TrollStore" "$app/PersistenceHelper" "$app/trollstorehelper" 2>/dev/null || true
		printf '%s' "$LDID_VER" > "$app/ldid.version"
		/var/jb/usr/bin/chmod 644 "$app/ldid.version"
		say "pkgsetup: TrollStore container ldid ready at $app (attempt $k)"
	done
	if [ "$mcm_ok" -eq 0 ]; then
		say "pkgsetup: WARNING mcm_test timed out after $k attempts"
	fi
	if [ -f /redwood/share/trollstore/com.opa334.TrollStore.plist ] && [ ! -f /var/mobile/Library/Preferences/com.opa334.TrollStore.plist ]; then
		/var/jb/usr/bin/cp /redwood/share/trollstore/com.opa334.TrollStore.plist /var/mobile/Library/Preferences/com.opa334.TrollStore.plist
		/var/jb/usr/bin/chown 501:501 /var/mobile/Library/Preferences/com.opa334.TrollStore.plist
		/var/jb/usr/bin/chmod 644 /var/mobile/Library/Preferences/com.opa334.TrollStore.plist
		say "pkgsetup: seeded com.opa334.TrollStore.plist"
	fi
fi

# 3. TrollApps (Chinese UI + CN-accessible JSON sources) & iSH executable fixup
if [ -d /Applications/TrollApps.app ]; then
	NO_UICACHE=1 /var/jb/bin/sh /redwood/bin/hk-appinstall.sh TrollApps.app org.resonance.store TrollApps
	say "pkgsetup: TrollApps.app ready"
fi
if [ -d /Applications/iSH.app ]; then
	NO_UICACHE=1 /var/jb/bin/sh /redwood/bin/hk-appinstall.sh iSH.app app.ish.iSH
fi
if [ -f /redwood/share/trollstore/org.resonance.store.cn.plist ] && [ ! -f /var/mobile/Library/Preferences/org.resonance.store.plist ]; then
	/var/jb/usr/bin/cp /redwood/share/trollstore/org.resonance.store.cn.plist /var/mobile/Library/Preferences/org.resonance.store.plist
	/var/jb/usr/bin/chown 501:501 /var/mobile/Library/Preferences/org.resonance.store.plist
	/var/jb/usr/bin/chmod 644 /var/mobile/Library/Preferences/org.resonance.store.plist
	say "pkgsetup: seeded org.resonance.store.plist (CN repos)"
fi

# 4. Single uicache registration for Sileo, TrollStore, TrollApps, and iSH
/var/jb/usr/bin/uicache -a
rc=$?
say "pkgsetup: done uicache rc=$rc $(date)"
: > /private/var/tmp/pkgsetup.done
exit 0
