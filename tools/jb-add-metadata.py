#!/usr/bin/env python3
# Install the dpkg metadata for a .deb whose files are already in the prefix.
# The metadata stays under var/jb/Library/dpkg; package maintainer scripts are
# copied but never run here.
from pathlib import Path, PurePosixPath
import io
import lzma
import os
import posixpath
import re
import subprocess
import sys
import tarfile

SCRIPT_NAMES = {"preinst", "postinst", "prerm", "postrm", "config"}


def ar_members(deb):
    run = subprocess.run(["ar", "t", str(deb)], capture_output=True, text=True)
    if run.returncode:
        raise SystemExit(run.stderr.strip() or "ar t failed")
    return [line.strip() for line in run.stdout.splitlines() if line.strip()]


def ar_data(deb, member):
    run = subprocess.run(["ar", "p", str(deb), member], capture_output=True)
    if run.returncode or not run.stdout:
        raise SystemExit("cannot read %s from %s" % (member, deb))
    data = run.stdout
    if member.endswith(".zst"):
        run = subprocess.run(["zstd", "-dc"], input=data, capture_output=True)
        if run.returncode:
            raise SystemExit(run.stderr.decode(errors="replace"))
        return run.stdout
    if member.endswith(".lzma"):
        return lzma.decompress(data, format=lzma.FORMAT_ALONE)
    return data


def tar_of(deb, member):
    return tarfile.open(fileobj=io.BytesIO(ar_data(deb, member)), mode="r:*")


def field(control, name):
    match = re.search(r"(?m)^%s: (.*)$" % re.escape(name), control)
    return match.group(1).strip() if match else None


def stanzas(text):
    return [block for block in text.split("\n\n") if block.strip()]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: jb-add-metadata.py PREFIX PACKAGE.deb")
    prefix = Path(sys.argv[1])
    deb = Path(sys.argv[2])
    jb = prefix / "var/jb"
    info = jb / "Library/dpkg/info"
    info.mkdir(parents=True, exist_ok=True)

    members = ar_members(deb)
    control_member = next((m for m in members if m.startswith("control.tar")), None)
    data_member = next((m for m in members if m.startswith("data.tar")), None)
    if control_member is None or data_member is None:
        raise SystemExit("%s is not a .deb with control and data archives" % deb)

    with tar_of(deb, control_member) as control_tar:
        control_file = next((m for m in control_tar.getmembers() if PurePosixPath(m.name).name == "control"), None)
        if control_file is None:
            raise SystemExit("%s has no control file" % deb)
        control = control_tar.extractfile(control_file).read().decode("utf-8", "replace")
        package = field(control, "Package")
        version = field(control, "Version")
        if not package or not version:
            raise SystemExit("%s has an incomplete control file" % deb)
        for member in control_tar.getmembers():
            name = PurePosixPath(member.name).name
            if name in ("", ".", "control") or not member.isfile():
                continue
            data = control_tar.extractfile(member).read()
            target = info / (package + "." + name)
            target.write_bytes(data)
            target.chmod(0o755 if name in SCRIPT_NAMES else 0o644)

    with tar_of(deb, data_member) as data_tar:
        paths = {"/.", "/var", "/var/jb"}
        for member in data_tar.getmembers():
            parts = [p for p in PurePosixPath(member.name).parts if p not in ("", ".", "/")]
            if "var" not in parts or "jb" not in parts:
                continue
            at = parts.index("jb")
            if at == 0 or parts[at - 1] != "var":
                continue
            path = "/var/jb/" + "/".join(parts[at + 1:])
            path = path.rstrip("/")
            if path == "/var/jb/":
                path = "/var/jb"
            paths.add(path)
            parent = posixpath.dirname(path)
            while parent.startswith("/var/jb/"):
                paths.add(parent)
                parent = posixpath.dirname(parent)

    list_path = info / (package + ".list")
    ordered = ["/.", "/var", "/var/jb"] + sorted(paths - {"/.", "/var", "/var/jb"})
    list_path.write_text("\n".join(ordered) + "\n", encoding="utf-8")

    status_path = jb / "Library/dpkg/status"
    status = status_path.read_text(encoding="utf-8", errors="replace") if status_path.exists() else ""
    entry = control.strip() + "\nStatus: install ok installed"
    blocks = stanzas(status)
    out = [entry if field(block, "Package") == package else block for block in blocks]
    if not any(field(block, "Package") == package for block in blocks):
        out.append(entry)
    status_path.write_text("\n\n".join(out) + "\n", encoding="utf-8")
    print("jb-add-metadata: %s %s, %d paths" % (package, version, len(ordered)))


if __name__ == "__main__":
    main()
