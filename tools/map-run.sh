#!/bin/bash
# tools/map-run.sh: stage one IPA for the phone's map scene (rc.jobs.mapdiag) and boot it.
#
# usage: tools/map-run.sh /path/to/app.ipa [source note]
#
# The IPA must run on this port, which means decrypted: an App Store image is
# FairPlay-encrypted (LC_ENCRYPTION_INFO cryptid 1) and there is no Apple key on a
# Snapdragon to decrypt it, so the script refuses it before anything is built.  An
# unencrypted build (cryptid 0) -- a self-built IPA, an unsigned release, or a
# decryption the operator lawfully made of their own copy -- is what it stages.
#
# What it does:
#   1. reads the bundle's Info.plist and the executable's cryptid from the IPA;
#   2. writes target.ipa and target.txt (name, bundle, version, appdir, sha256,
#      source) into the LAN directory the phone fetches from;
#   3. builds out/thyme-boot/root-mapdiag.apfs and starts one RAM run in the
#      background, logging to logs/mapdiag-run.out.
# MAP_STAGE_ONLY=1 stops after step 2 (staging can be checked without a phone run);
# SKIP_BUILD=1 uses the existing root-mapdiag.apfs when the scene has not changed.
# The phone side (userland/bin/hk-mapipa.sh) verifies the digest, installs through
# TrollStore's helper, launches by bundle id and collects the crash reports.
set -euo pipefail
cd "$(dirname "$0")/.."

ipa=${1:?usage: tools/map-run.sh APP.ipa [source note]}
source_note=${2:-operator-provided}
[ -f "$ipa" ] || { echo "map-run: no such file: $ipa" >&2; exit 1; }
lan=${LAN_DIR:-/home/darling/ipas-lan}
mkdir -p "$lan"

python3 - "$ipa" "$lan" "$source_note" <<'PY'
import hashlib, os, plistlib, struct, sys, zipfile

ipa, lan, source = sys.argv[1], sys.argv[2], sys.argv[3]
sha = hashlib.sha256(open(ipa, 'rb').read()).hexdigest()

with zipfile.ZipFile(ipa) as z:
    infos = [n for n in z.namelist()
             if n.startswith('Payload/') and n.endswith('.app/Info.plist') and n.count('/') == 2]
    if len(infos) != 1:
        sys.exit(f'map-run: expected one Payload/*.app/Info.plist, found {len(infos)}')
    appdir = infos[0].split('/')[1]
    d = plistlib.loads(z.read(infos[0]))
    exe = d.get('CFBundleExecutable', '')
    exe_path = f'Payload/{appdir}/{exe}'
    if exe_path not in z.namelist():
        sys.exit(f'map-run: no {exe_path} in the IPA')
    data = z.read(exe_path)

def cryptid_of(buf, base):
    magic = struct.unpack_from('<I', buf, base)[0]
    if magic not in (0xfeedfacf, 0xfeedface):
        return None
    hdrsize = 32 if magic == 0xfeedfacf else 28
    ncmds = struct.unpack_from('<I', buf, base + 16)[0]
    off = base + hdrsize
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', buf, off)
        if cmd in (0x2C, 0x21):          # LC_ENCRYPTION_INFO_64 / LC_ENCRYPTION_INFO
            return struct.unpack_from('<I', buf, off + 16)[0]
        off += cmdsize
    return 0

cryptid = None
magic = struct.unpack_from('>I', data, 0)[0]
if magic in (0xcafebabe, 0xbebafeca):
    n = struct.unpack_from('>I', data, 4)[0]
    for i in range(n):
        _cpu, _sub, off, size, _align = struct.unpack_from('>IIIII', data, 8 + i * 20)
        c = cryptid_of(data, off)
        if c is not None and struct.unpack_from('<I', data, off)[0] == 0xfeedfacf:
            hdr = struct.unpack_from('<II', data, off + 4)
            if hdr[0] == 0x0100000C:      # CPU_TYPE_ARM64
                cryptid = c
                break
else:
    cryptid = cryptid_of(data, 0)

print(f'map-run: {appdir} {d.get("CFBundleIdentifier","?")} '
      f'{d.get("CFBundleShortVersionString","?")} ({len(data)} bytes, cryptid {cryptid})')
if cryptid != 0:
    sys.exit(f'map-run: cryptid {cryptid}: FairPlay-encrypted; this port cannot decrypt it '
             f'(no Apple key). Stage a decrypted copy of a build you lawfully have.')

with open(os.path.join(lan, 'target.txt'), 'w') as f:
    f.write(f'name={d.get("CFBundleDisplayName") or d.get("CFBundleName") or appdir}\n')
    f.write(f'bundle={d.get("CFBundleIdentifier","")}\n')
    f.write(f'version={d.get("CFBundleShortVersionString","")} build={d.get("CFBundleVersion","")}\n')
    f.write(f'appdir={appdir}\n')
    f.write(f'exe={exe}\n')
    f.write(f'sha256={sha}\n')
    f.write(f'source={source}\n')
print(f'map-run: sha256 {sha}')
PY

cp -f "$ipa" "$lan/target.ipa"
echo "map-run: staged $(basename "$ipa") -> $lan/target.ipa"

# The phone fetches from the Windows LAN address 192.168.0.104:8000; that address
# must be this machine (WSL2's own 172.x address is not reachable from the phone).
if ! curl -sf -m 3 http://192.168.0.104:8000/target.txt >/dev/null; then
	echo "map-run: 192.168.0.104:8000 does not serve $lan -- start the LAN server there" >&2
	echo "map-run:   cd $lan && python -m http.server 8000" >&2
	exit 1
fi
echo "map-run: the LAN server answers with: $(curl -s -m 3 http://192.168.0.104:8000/target.txt | tr '\n' ' ')"
[ -z "${MAP_STAGE_ONLY:-}" ] || exit 0

if [ -z "${SKIP_BUILD:-}" ]; then
	SCENE=mapdiag METAL_PLUGIN=ca devices/thyme/build-root.sh
fi
[ -f out/thyme-boot/root-mapdiag.apfs ] || { echo "map-run: no root-mapdiag.apfs" >&2; exit 1; }
log=logs/mapdiag-run.out
nohup bash /home/darling/run-applog.sh mapdiag >"$log" 2>&1 &
echo "map-run: phone run started (pid $!); follow $log and logs/*-run.log.live"
