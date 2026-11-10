#!/usr/bin/env python3
"""A first look at a camera raw too large for camraw.py's per-pixel pass.

usage: camfull.py CAMERA.raw [PREFIX]

Reads the RWCAM001 header, reports the frame's statistics from sampled lines
and writes PREFIX.png: every SAMPLE-th 2x2 Bayer cell as one pixel, GRBG (the
S5KHMX module's order), grey-world white balance and gamma 2.2, as
tools/camraw.py does for its full-size pictures.
"""
import mmap
import struct
import sys
import zlib

STEP = 8            # every 8th 2x2 cell: 6016/8 x 4512/8 = 752x564


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))


def png(path, width, height, rows):
    data = b''.join(b'\0' + bytes(r) for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(data, 6)) + chunk(b'IEND', b''))


def raw10(buf, off, width):
    """width 10-bit values from four-pixels-in-five-bytes."""
    out = [0] * (width & ~3)
    n = len(out) // 4
    for i in range(n):
        b = buf[off + 5 * i:off + 5 * i + 5]
        low = b[4]
        out[4 * i] = b[0] << 2 | (low & 3)
        out[4 * i + 1] = b[1] << 2 | (low >> 2 & 3)
        out[4 * i + 2] = b[2] << 2 | (low >> 4 & 3)
        out[4 * i + 3] = b[3] << 2 | (low >> 6 & 3)
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else path.rsplit('.', 1)[0]
    f = open(path, 'rb')
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    magic, width, height, stride, fmt, bayer, frames, frame_bytes, source = struct.unpack_from('<8s8I', mm, 0)
    sensor = mm[40:56].split(b'\0')[0].decode(errors='replace')
    last = struct.unpack_from('<I', mm, 56)[0]
    if magic != b'RWCAM001':
        sys.exit('camfull.py: not frames of the camera driver')
    print(f'{sensor}: {width}x{height}, stride {stride}, format {fmt}, bayer {bayer}, '
          f'{frames} frame(s) whole, frame_bytes {frame_bytes}, last place {last}, file {len(mm)} bytes')
    if fmt != 1:
        sys.exit('camfull.py: this is not a RAW10 frame (format %d); use tools/camraw.py' % fmt)
    if frames == 0:
        sys.exit('camfull.py: the driver wrote no frame whole')
    # The place the driver read is `last`; bank 0 begins after the header.
    base = 4096 + last * frame_bytes
    if base + frame_bytes > len(mm):
        base = 4096
    # Statistics from up to 96 lines spread over the frame.
    lines = sorted(set(range(0, height, max(1, height // 96))))
    lo, hi, total, count = 1 << 20, 0, 0, 0
    for y in lines:
        vals = raw10(mm, base + y * stride, width)
        lo = min(lo, min(vals))
        hi = max(hi, max(vals))
        total += sum(vals)
        count += len(vals)
    print(f'sampled {len(lines)} lines: min {lo}, mean {total // count}, max {hi} (10-bit)')
    # The preview: one pixel per STEP x STEP cells.
    offs = {0: ((0, 0), (0, 1), (1, 0), (1, 1)), 1: ((0, 1), (0, 0), (1, 1), (1, 0)),
            2: ((1, 0), (0, 0), (1, 1), (0, 1)), 3: ((1, 1), (0, 1), (1, 0), (0, 0))}[bayer]
    (ry, rx), (g1y, g1x), (g2y, g2x), (by, bx) = offs
    cells = []
    sums = [0, 0, 0]
    for y in range(0, height - 1, 2 * STEP):
        row = raw10(mm, base + y * stride, width)
        row2 = raw10(mm, base + (y + 1) * stride, width)
        out = []
        for x in range(0, width - 1, 2 * STEP):
            r = row[x + rx]
            g = (row[x + g1x] + row2[x + g2x]) / 2
            b = row2[x + bx]
            out.append((r, g, b))
            sums[0] += r
            sums[1] += g
            sums[2] += b
        cells.append(out)
    gains = [sums[1] / s if s else 1.0 for s in sums]
    # A fixed black level of 64 and a high percentile (not the brightest noise
    # spike) as white: a dim frame keeps its contrast instead of washing out.
    black = 64
    values = sorted(max(0.0, max(c[0] * gains[0], c[1], c[2] * gains[2]) - black) for row in cells for c in row)
    top = values[int(len(values) * 0.995)] if values else 1
    if top <= 0:
        top = 1
    rows = []
    for row in cells:
        out = bytearray()
        for c in row:
            for v in (c[0] * gains[0], c[1], c[2] * gains[2]):
                out.append(max(0, min(255, int((max(0.0, v - black) / top) ** (1 / 2.2) * 255 + 0.5))))
        rows.append(out)
    png(prefix + '.png', len(cells[0]), len(cells), rows)
    print(f'{prefix}.png ({len(cells[0])}x{len(cells)}, one pixel per {STEP}x{STEP} cells)')


if __name__ == '__main__':
    main()
