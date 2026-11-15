#!/usr/bin/env python3
"""Remove the 'get' selector from the Location Services row of Privacy.plist, in the system
image being built.

The Location Services row is the only row of PrivacySettingsUI.framework's Privacy.plist that
carries a "get" -- {'cell': 'PSLinkListCell', 'detail': 'PUILocationServicesListController',
'get': 'locationServicesEnabled:', 'iconCache': 'Location', 'label': 'LOCATION'} -- and it is
the one row the Privacy & Security page leaves out on this port, while every other row of the
same plist is shown (devices/thyme/CHECKLIST.md, 2026-11-15).  The classes the row names all
resolve; what is left is the getter.  With the key gone the row is an ordinary link cell, and
whether it appears says whether the getter is what drops it.  Idempotent: a plist whose row is
already without the key is left exactly as it is.
"""
import os
import plistlib
import sys

if len(sys.argv) != 2:
    sys.exit('usage: patch-privacy.py <Privacy.plist>')

path = sys.argv[1]
with open(path, 'rb') as f:
    plist = plistlib.load(f)

changed = False
for item in plist.get('items', []):
    if isinstance(item, dict) and item.get('label') == 'LOCATION':
        if 'get' in item:
            del item['get']
            changed = True
        break

if changed:
    # write beside the file and rename over it: the system tree is a tree of hard links to the
    # extracted rootfs (build.sh's linkcopy), and truncating the shared inode would edit the
    # extraction itself
    temporary = path + '.patch-privacy'
    with open(temporary, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.replace(temporary, path)
    print('patch-privacy: the LOCATION row no longer has a get')
else:
    print('patch-privacy: the LOCATION row has no get; left as it is')
