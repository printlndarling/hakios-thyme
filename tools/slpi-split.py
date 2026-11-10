#!/usr/bin/env python3
"""Reassemble a Qualcomm split firmware image from its .bXX parts.

A split image's parts are not the file laid end to end: each part is one
program header's blob, and the image file is the parts at their headers'
file offsets with the gaps zeroed (the splitter writes them out that way).
``cat slpi.b00 slpi.b01 ...`` therefore puts every part after the first at
the wrong offset -- the ELF header still parses, but the code and strings
from the second part on are shifted, and a disassembly of the result is not
the firmware.  This script places each part at its program header's
p_offset and checks every part's size against its header.

    tools/slpi-split.py fw-slpi -o /tmp/slpi.img
    llvm-readelf -h /tmp/slpi.img      # Qualcomm Hexagon, entry 0x88000000
    llvm-objdump -d /tmp/slpi.img > /tmp/slpi.dis

The disassembly addresses are the file's p_vaddr space (0xb0xxxxxx for the
SLPI payload); a string's address is its file offset's segment base plus its
offset into the segment (llvm-readelf -l shows the map).  Code reaches the
strings with `rN = ##0x...` immediates, which llvm-objdump prints in full,
and the image's own pointer tables hold 32-bit vaddrs (0xb2xxxxxx).
"""
import argparse
import os
import struct
import sys


def parts_of(directory):
    names = sorted(f for f in os.listdir(directory) if ".b" in f and f.rsplit(".b", 1)[1].isdigit())
    if not names:
        sys.exit("no .bXX parts in %s" % directory)
    return [os.path.join(directory, n) for n in names]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", help="the directory holding the .bXX parts")
    parser.add_argument("-o", "--output", default="/tmp/slpi.img", help="the reassembled image (default %(default)s)")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    parts = parts_of(args.directory)
    header = open(parts[0], "rb").read()
    if header[:4] != b"\x7fELF" or header[4] != 1 or header[5] != 1:
        sys.exit("%s is not a 32-bit little-endian ELF header" % os.path.basename(parts[0]))
    e_phoff, = struct.unpack_from("<I", header, 0x1C)
    e_phentsize, = struct.unpack_from("<H", header, 0x2A)
    e_phnum, = struct.unpack_from("<H", header, 0x2C)
    if len(parts) > e_phnum:
        sys.exit("%d parts, but the header has %d program headers" % (len(parts), e_phnum))

    image = bytearray(header)
    for index, path in enumerate(parts):
        data = open(path, "rb").read()
        p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", header, e_phoff + index * e_phentsize)
        name = os.path.basename(path)
        if len(data) != p_filesz:
            sys.exit("%s is %d bytes; its program header says %d" % (name, len(data), p_filesz))
        if not args.quiet:
            print("%-10s at %#08x  vaddr %#010x  %8d bytes  type %d" % (name, p_offset, p_vaddr, p_filesz, p_type))
        if p_offset > len(image):
            image += b"\0" * (p_offset - len(image))
        end = p_offset + p_filesz
        if end > len(image):
            image += b"\0" * (end - len(image))
        image[p_offset:end] = data

    with open(args.output, "wb") as out:
        out.write(image)
    print("wrote %s: %d bytes" % (args.output, len(image)))


if __name__ == "__main__":
    main()
