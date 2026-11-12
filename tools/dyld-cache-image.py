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
Ranges come from imagesText (0x88), one dyld_cache_image_text_info per image.
"""
import struct
import sys


def cstr(f, off, limit=4096):
    f.seek(off)
    data = f.read(limit)
    end = data.find(b"\0")
    return data[: end if end >= 0 else limit].decode("utf-8", "replace")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, addrs = sys.argv[1], [int(a, 16) for a in sys.argv[2:]]

    with open(path, "rb") as f:
        header = f.read(0x200)
        if not header.startswith(b"dyld_v1"):
            sys.exit(f"{path}: not a dyld shared cache")
        text_offset, text_count = struct.unpack_from("<QQ", header, 0x88)
        images_offset, images_count = struct.unpack_from("<II", header, 0x1C0)
        shared_region_start = struct.unpack_from("<Q", header, 0xE0)[0]
        if images_count == 0 and text_count == 0:
            print("no image table in this file (a subcache, not the header file)", file=sys.stderr)

        texts = []
        if text_count:
            f.seek(text_offset)
            for _ in range(text_count):
                f.read(16)                      # uuid
                load, size, path_off = struct.unpack("<QII", f.read(16))
                texts.append((load, size, path_off))
            texts.sort()

        images = []
        f.seek(images_offset)
        for _ in range(images_count):
            va, _mod, _ino, path_off, _pad = struct.unpack("<QQQII", f.read(0x20))
            images.append((va, path_off))
        images.sort()

        for addr in addrs:
            hit = None
            if texts:
                for i, (load, size, path_off) in enumerate(texts):
                    end = load + size if size else (texts[i + 1][0] if i + 1 < len(texts) else load)
                    if load <= addr < end:
                        hit = (load, end, path_off)
                        break
            else:
                for i, (va, path_off) in enumerate(images):
                    end = images[i + 1][0] if i + 1 < len(images) else va + 0x100000
                    if va <= addr < end:
                        hit = (va, end, path_off)
                        break
            if hit:
                load, end, path_off = hit
                print(f"0x{addr:x} -> {cstr(f, path_off)} +0x{addr - load:x} "
                      f"(image 0x{load:x}..0x{end:x})")
            else:
                print(f"0x{addr:x} -> no image in this cache file "
                      f"(the cache is mapped from 0x{shared_region_start:x})")


if __name__ == "__main__":
    main()
