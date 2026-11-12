#!/usr/bin/env python3
"""Name the dyld shared cache image that contains an unslid address.

The fault lines of userland/src/inject.c print addresses with the shared
cache's slide taken off. For code in the cache that is the address the cache
was built with, and this says which image it is and how far into it:

    tools/dyld-cache-image.py sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64 \\
        0x1d0cd50ec 0x1d0cd372c
    -> /usr/lib/system/libsystem_pthread.dylib +0x30ec

The split-cache format (iOS 15/16, many dyld_shared_cache_arm64.NN files) keeps
the image table in the header file only: imagesOffset/imagesCount are at 0x1C0
of dyld_cache_header, not the unused 0x18/0x1C pair an older reader looks at.
Ranges come from imagesText (0x88), one dyld_cache_image_text_info per image;
the older image table (0x270) is used for the paths when a cache has no
imagesText, and then an image's end is only known from the next one's start.
"""
import struct
import sys


def cstr(f, off):
    f.seek(off)
    data = f.read(4096)
    end = data.find(b"\0")
    return data[: end if end >= 0 else len(data)].decode("utf-8", "replace")


def ranges(f):
    """(table, shared region start): (start, end or None, path offset) per image, in order."""
    header = f.read(0x200)
    if not header.startswith(b"dyld_v1"):
        sys.exit("not a dyld shared cache")
    text_offset, text_count = struct.unpack_from("<QQ", header, 0x88)
    images_offset, images_count = struct.unpack_from("<II", header, 0x1C0)
    shared_region_start = struct.unpack_from("<Q", header, 0xE0)[0]

    if text_count:
        f.seek(text_offset)
        texts = []
        for _ in range(text_count):
            f.read(16)                              # uuid
            load, size, path_off = struct.unpack("<QII", f.read(16))
            texts.append((load, size, path_off))
        texts.sort()
        table = [(load, load + size if size else (texts[i + 1][0] if i + 1 < len(texts) else None), path_off)
                 for i, (load, size, path_off) in enumerate(texts)]
        return table, shared_region_start

    if images_count == 0:
        sys.exit("no image table in this file (a subcache, not the header file)")
    f.seek(images_offset)
    images = []
    for _ in range(images_count):
        va, _mod, _ino, path_off, _pad = struct.unpack("<QQQII", f.read(0x20))
        images.append((va, path_off))
    images.sort()
    table = [(va, images[i + 1][0] if i + 1 < len(images) else None, path_off)
             for i, (va, path_off) in enumerate(images)]
    return table, shared_region_start


def find(table, addr):
    for start, end, path_off in table:
        if start <= addr and (end is None or addr < end):
            return start, end, path_off
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, addrs = sys.argv[1], [int(a, 16) for a in sys.argv[2:]]

    with open(path, "rb") as f:
        table, shared_region_start = ranges(f)
        for addr in addrs:
            hit = find(table, addr)
            if hit is None:
                print(f"0x{addr:x} -> no image in this cache file "
                      f"(the cache is mapped from 0x{shared_region_start:x})")
                continue
            start, end, path_off = hit
            span = f"image 0x{start:x}.." + (f"0x{end:x}" if end is not None else "end unknown")
            print(f"0x{addr:x} -> {cstr(f, path_off)} +0x{addr - start:x} ({span})")


if __name__ == "__main__":
    main()
