#!/usr/bin/env python3
"""Pack a .app into an IPA: a zip whose Payload/<name>.app holds the bundle, Unix modes kept.

usage: ipa.py APP OUT.ipa

Why not zip(1): the modes are the point.  Python's zipfile writes external_attr only when
it is set, and an extractor that trusts the archive (TrollStore's unarchive) takes the
executable bit from there; a bundle whose main binary arrives 644 is exactly the wall
iSH hit when an IPA was unpacked by hand (CHECKLIST, "Third-party applications").
Directories are stored as entries of their own so empty ones survive.
"""
import os
import stat
import sys
import zipfile

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(1)

app = sys.argv[1].rstrip('/')
out = sys.argv[2]
base = os.path.dirname(app)
stamp = (2020, 1, 1, 0, 0, 0)

with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
    for root, dirs, files in os.walk(app):
        dirs.sort()
        rel = os.path.join('Payload', os.path.relpath(root, base))
        info = zipfile.ZipInfo(rel + '/')
        mode = os.stat(root).st_mode
        info.external_attr = ((mode & 0xFFFF) << 16) | 0x10
        info.date_time = stamp
        archive.writestr(info, b'')
        for name in sorted(files):
            path = os.path.join(root, name)
            arc = os.path.join(rel, name)
            mode = os.lstat(path).st_mode
            if stat.S_ISLNK(mode):
                continue        # bundles here carry none; a link would need its target
            info = zipfile.ZipInfo(arc)
            info.external_attr = (mode & 0xFFFF) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.date_time = stamp
            with open(path, 'rb') as handle:
                archive.writestr(info, handle.read())
print('ipa.py: %s (%d bytes)' % (out, os.path.getsize(out)))
