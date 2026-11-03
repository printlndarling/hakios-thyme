#!/usr/bin/env python3
# Fail the prefix build if dpkg knows a package without a file list. Missing
# lists make dpkg warn during install and make removal/upgrade unreliable.
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: jb-check-prefix.py PREFIX")
prefix = Path(sys.argv[1])
jb = prefix / "var/jb"
status = jb / "Library/dpkg/status"
info = jb / "Library/dpkg/info"
packages = re.findall(r"(?m)^Package: (.+)$", status.read_text(encoding="utf-8", errors="replace"))
duplicates = sorted({name for name in packages if packages.count(name) > 1})
missing = [name for name in packages if not (info / (name + ".list")).is_file()]
empty = [name for name in packages if (info / (name + ".list")).is_file() and (info / (name + ".list")).stat().st_size == 0]
if duplicates or missing or empty:
    if duplicates:
        print("duplicate packages: " + ", ".join(duplicates), file=sys.stderr)
    if missing:
        print("missing file lists: " + ", ".join(missing), file=sys.stderr)
    if empty:
        print("empty file lists: " + ", ".join(empty), file=sys.stderr)
    raise SystemExit(1)
print("jb-check-prefix: %d packages, all with file lists" % len(packages))
