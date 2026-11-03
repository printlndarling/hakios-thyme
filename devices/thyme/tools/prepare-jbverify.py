#!/usr/bin/env python3
# Fetch the pinned Procursus package used by userland/rc.jobs.jbverify.
# The package is local test data and is never committed.
from pathlib import Path
import hashlib
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "out/jb-offline/tree"
NAME = "tree_2.0.1_iphoneos-arm64.deb"
URL = "https://apt.procurs.us/pool/main/iphoneos-arm64-rootless/1800/tree/" + NAME
SHA256 = "f35ae059741b467636fad837c3dd827253f5287f69fe9eee1b90b0f15c43d05a"

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

OUT.mkdir(parents=True, exist_ok=True)
dest = OUT / NAME
if dest.is_file() and digest(dest) == SHA256:
    print(dest)
    raise SystemExit(0)

part = OUT / (NAME + ".part")
print("fetching", URL)
with urllib.request.urlopen(URL, timeout=60) as response, part.open("wb") as f:
    for chunk in iter(lambda: response.read(1 << 20), b""):
        f.write(chunk)
actual = digest(part)
if actual != SHA256:
    part.unlink(missing_ok=True)
    raise SystemExit(f"SHA-256 mismatch: expected {SHA256}, got {actual}")
part.replace(dest)
print(dest)
