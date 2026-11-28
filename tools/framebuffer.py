#!/usr/bin/env python3
"""Convert a packed or row-padded BGRX framebuffer to PNG."""

import argparse
import os
from pathlib import Path
import struct
import tempfile
import zlib


MAX_FRAME_BYTES = 16 * 1024 * 1024
HEADER = struct.Struct("<4sIII")


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def write_png(output, width, height, stride, pixels):
    if not (0 < width <= 4096 and 0 < height <= 4096 and stride >= width * 4 and
            len(pixels) == stride * height <= MAX_FRAME_BYTES):
        raise ValueError("invalid or incomplete framebuffer")
    rows = bytearray()
    for y in range(height):
        line = pixels[y * stride:y * stride + width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(0)
        rows.extend(rgb)
    output = Path(output)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as file:
        temporary = Path(file.name)
        try:
            file.write(b"\x89PNG\r\n\x1a\n")
            file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
            file.write(chunk(b"IDAT", zlib.compress(rows, 6)))
            file.write(chunk(b"IEND", b""))
            file.close()
            os.replace(temporary, output)
        finally:
            file.close()
            temporary.unlink(missing_ok=True)


def write_snapshot_png(output, source):
    header = source.read(HEADER.size)
    if len(header) != HEADER.size:
        raise ValueError("incomplete HDB framebuffer header")
    magic, width, height, stride = HEADER.unpack(header)
    if magic != b"HKFB":
        raise ValueError("unknown HDB framebuffer format")
    write_png(output, width, height, stride, source.read(MAX_FRAME_BYTES + 1))
    return width, height


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("png", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("stride", type=int)
    args = parser.parse_args()
    try:
        with args.raw.open("rb") as source:
            write_png(args.png, args.width, args.height, args.stride, source.read(MAX_FRAME_BYTES + 1))
    except (OSError, ValueError) as error:
        parser.exit(1, f"framebuffer: {error}\n")


if __name__ == "__main__":
    main()
