#!/usr/bin/env python3
"""Draw the HakiOS Weather icon as PNGs (a sun behind a cloud on a blue sky).

The application ships plain CFBundleIconFiles PNGs, so the icon is generated
here rather than kept as an opaque asset: the same command makes it again.

    tools/make-weather-icon.py userland/icons
"""
import os
import struct
import sys
import zlib


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def png(width, height, rows):
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def draw(size):
    rows = []
    sky_top = (52, 122, 181)
    sky_bottom = (24, 72, 121)
    sun = (255, 205, 79)
    cloud = (246, 249, 252)
    cx, cy, r2 = size * 0.36, size * 0.34, (size * 0.135) ** 2
    circles = [
        (size * 0.42, size * 0.60, (size * 0.115) ** 2),
        (size * 0.56, size * 0.55, (size * 0.145) ** 2),
        (size * 0.70, size * 0.61, (size * 0.105) ** 2),
        (size * 0.57, size * 0.63, (size * 0.125) ** 2),
    ]
    base = (size * 0.34, size * 0.60, size * 0.76, size * 0.72)  # x0, y0, x1, y1
    for y in range(size):
        t = y / max(1, size - 1)
        background = tuple(round(a + (b - a) * t) for a, b in zip(sky_top, sky_bottom))
        row = bytearray()
        for x in range(size):
            colour = background + (255,)
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r2:
                colour = sun + (255,)
            for px, py, pr in circles:
                dx, dy = x - px, y - py
                if dx * dx + dy * dy <= pr:
                    colour = cloud + (255,)
                    break
            else:
                if base[0] <= x <= base[2] and base[1] <= y <= base[3]:
                    colour = cloud + (255,)
            row += bytes(colour)
        rows.append(bytes(row))
    return png(size, size, rows)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "userland/icons"
    os.makedirs(out, exist_ok=True)
    for name, size in (("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180)):
        path = os.path.join(out, name)
        with open(path, "wb") as f:
            f.write(draw(size))
        print(f"make-weather-icon: {path} ({size}x{size})")


if __name__ == "__main__":
    main()
