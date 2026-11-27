#!/usr/bin/env python3
"""Inspect an IPA before it is staged for the phone.

Prints the bundle identity, minimum OS, the architectures with their FairPlay
state (LC_ENCRYPTION_INFO cryptid), the frameworks and plug-ins the bundle
carries and every dylib its main executable links against.  cryptid 1 means an
App Store image: only the Apple device that bought it can decrypt it, so the
port cannot run it; cryptid 0 is an image this port can execute (a developer's
unsigned release, a self-built IPA, or a decryption made from a lawful copy).

usage: tools/ipa-inspect.py APP.ipa [APP.ipa ...]
"""
import hashlib
import os
import plistlib
import struct
import sys
import zipfile

CPU_NAMES = {
    0x0000000C: 'arm',
    0x0100000C: 'arm64',
}


def cryptid_at(buf, base):
    magic = struct.unpack_from('<I', buf, base)[0]
    if magic not in (0xfeedfacf, 0xfeedface):
        return None
    hdrsize = 32 if magic == 0xfeedfacf else 28
    ncmds = struct.unpack_from('<I', buf, base + 16)[0]
    off = base + hdrsize
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', buf, off)
        if cmd in (0x2C, 0x21):
            return struct.unpack_from('<I', buf, off + 16)[0]
        off += cmdsize
    return 0


def load_commands(buf, base):
    """yield (cmd, header_off, cmdsize) of the Mach-O at base"""
    magic = struct.unpack_from('<I', buf, base)[0]
    if magic not in (0xfeedfacf, 0xfeedface):
        return
    hdrsize = 32 if magic == 0xfeedfacf else 28
    ncmds = struct.unpack_from('<I', buf, base + 16)[0]
    off = base + hdrsize
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', buf, off)
        yield cmd, off, cmdsize
        off += cmdsize


def linked_dylibs(buf, base):
    names = []
    for cmd, off, _size in load_commands(buf, base):
        # LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB
        if cmd in (0xC, 0x80000018, 0x8000001F, 0x80000023):
            str_off = struct.unpack_from('<I', buf, off + 8)[0]
            end = buf.index(b'\0', off + str_off)
            names.append(buf[off + str_off:end].decode('utf-8', 'replace'))
    return names


def slices_of(data):
    """[(arch_name, slice_offset, slice_bytes)] for a thin or fat Mach-O"""
    if len(data) < 8:
        return []
    if struct.unpack_from('>I', data, 0)[0] in (0xcafebabe, 0xbebafeca):
        n = struct.unpack_from('>I', data, 4)[0]
        out = []
        for i in range(n):
            cpu, sub, off, size, _align = struct.unpack_from('>IIIII', data, 8 + i * 20)
            out.append((CPU_NAMES.get(cpu, hex(cpu)) + ('e' if cpu == 0x0100000C and sub == 2 else ''),
                        off, data[off:off + size]))
        return out
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic in (0xfeedfacf, 0xfeedface):
        cpu = struct.unpack_from('<I', data, 4)[0]
        sub = struct.unpack_from('<I', data, 8)[0]
        return [(CPU_NAMES.get(cpu, hex(cpu)) + ('e' if cpu == 0x0100000C and sub == 2 else ''), 0, data)]
    return []


def inspect(path):
    data = open(path, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    print(f'== {os.path.basename(path)} ({len(data)} bytes, sha256 {sha})')
    with zipfile.ZipFile(path) as z:
        infos = [n for n in z.namelist()
                 if n.startswith('Payload/') and n.endswith('.app/Info.plist') and n.count('/') == 2]
        if len(infos) != 1:
            print(f'   Payload/*.app/Info.plist: {len(infos)} found; not a single-app IPA')
            return
        appdir = infos[0].split('/')[1]
        d = plistlib.loads(z.read(infos[0]))
        exe = d.get('CFBundleExecutable', '')
        exe_path = f'Payload/{appdir}/{exe}'
        exe_data = z.read(exe_path)
        print(f'   bundle: {d.get("CFBundleIdentifier", "?")}  version: '
              f'{d.get("CFBundleShortVersionString", "?")} ({d.get("CFBundleVersion", "?")})')
        print(f'   name: {d.get("CFBundleDisplayName") or d.get("CFBundleName") or appdir}  '
              f'minOS: {d.get("MinimumOSVersion", "?")}  platforms: {d.get("CFBundleSupportedPlatforms", "?")}')
        caps = d.get('UIRequiredDeviceCapabilities')
        if caps:
            print(f'   required capabilities: {caps}')
        for arch, _off, slice_data in slices_of(exe_data):
            print(f'   arch {arch}: cryptid {cryptid_at(exe_data, _off)}')
            libs = linked_dylibs(exe_data, _off)
            print(f'   dylibs ({len(libs)}):')
            for name in libs:
                print(f'     {name}')
        frameworks = sorted({n.split('/')[3] for n in z.namelist()
                             if n.startswith(f'Payload/{appdir}/Frameworks/') and n.count('/') >= 4})
        if frameworks:
            print(f'   frameworks ({len(frameworks)}): {", ".join(frameworks)}')
        plugins = sorted({n.split('/')[3] for n in z.namelist()
                          if n.startswith(f'Payload/{appdir}/PlugIns/') and n.count('/') >= 4})
        if plugins:
            print(f'   plug-ins: {", ".join(plugins)}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit('usage: tools/ipa-inspect.py APP.ipa [APP.ipa ...]')
    for p in sys.argv[1:]:
        inspect(p)
