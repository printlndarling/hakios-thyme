#!/usr/bin/env python3
"""List modules in a UEFI firmware volume without extracting or modifying it.

usage: tools/uefi-modules.py FIRMWARE [--match TEXT] [--extract DIR]

FIRMWARE is a vendor XBL or UEFI image. The tool reads every `_FVH` volume it
finds, walks the firmware files, and prints each module's GUID, UI name, type,
and section sizes. Requires `uefi_firmware==1.16`.
"""
import argparse
import os
import uuid

from uefi_firmware.uefi import FirmwareFile, FirmwareFileSystemSection, FirmwareVolume


def files_in(volume):
    found = []

    def walk(item):
        if isinstance(item, FirmwareFile):
            found.append(item)
        for child in getattr(item, "objects", []):
            if hasattr(child, "__dict__"):
                walk(child)

    walk(volume)
    return found


def sections_in(item):
    found = []
    for child in getattr(item, "objects", []):
        if not hasattr(child, "__dict__"):
            continue
        if isinstance(child, FirmwareFileSystemSection):
            found.append(child)
        else:
            found.extend(sections_in(child))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("firmware")
    parser.add_argument("--match", default="", help="case-insensitive substring for the UI name")
    parser.add_argument("--extract", metavar="DIR", help="write matching PE32 sections as DIR/NAME.efi")
    args = parser.parse_args()

    data = open(args.firmware, "rb").read()
    offsets = []
    at = 0
    while True:
        at = data.find(b"_FVH", at)
        if at < 40:
            break
        offsets.append(at - 40)
        at += 4
    if not offsets:
        raise SystemExit("uefi-modules: no firmware volume header")

    match = args.match.lower()
    seen = set()
    total = 0
    for offset in offsets:
        volume = FirmwareVolume(data[offset:])
        if not volume.process():
            continue
        for firmware_file in files_in(volume):
            guid = str(uuid.UUID(bytes_le=firmware_file.guid))
            sections = sections_in(firmware_file)
            name = next((section.name for section in sections if section.type == 0x15), "")
            if match and match not in name.lower():
                continue
            key = (guid, name, firmware_file.type)
            if key in seen:
                continue
            seen.add(key)
            sizes = ",".join("%02x:%d" % (section.type, len(section.data)) for section in sections)
            print("%s  type=%02x  size=%d  name=%s  sections=%s" %
                  (guid, firmware_file.type, firmware_file.size, name or "-", sizes))
            if args.extract and name:
                pe32 = [section for section in sections if section.type == 0x10]
                if len(pe32) == 1:
                    os.makedirs(args.extract, exist_ok=True)
                    with open(os.path.join(args.extract, name + ".efi"), "wb") as output:
                        output.write(pe32[0].data)
            total += 1
    print("modules=%d volumes=%d" % (total, len(offsets)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
