#!/usr/bin/env python3
"""Make a camera's colour tuning, as libhakios.media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning), out of the camera's tuning file of Android's camera stack
(/vendor/lib64/camera/com.qti.tuned.<name>.bin of the phone: the vendor's, kept in fw-camera/ and never in
the repository; so is what this makes of it).

usage: camtuning.py TUNED.bin OUT.isp     (camtuning.py TUNED.bin -   lists what is there only)

The tuning file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file, laid out as the sensors'
module files are (see camsensor.py): a table of symbols { id, name[32], kind, ?, parent, offset, size }
and the data they point into. Two of its modules are taken, each as Chromatix keeps it -- a tree whose
first level is the flash (led_idx_data: 0 without one, the only one taken), the next the scene's light
(aec_data: from a lux index to another, Qualcomm's measure of how dark the scene is: higher is darker),
the last the light's colour temperature (cct_data: from a CCT to another, in kelvin):

  cc13 (the colour matrix, IPE's): per light and temperature a 3x3 matrix from the sensor's colours,
       white balanced, to linear sRGB, by rows R, G, B (each row sums to 1)
  lsc34 (lens shading): per light and temperature a gain for each of R, Gr, Gb and B at each point of
       a 17 x 13 grid laid over the sensor (corners on its corners, rows from the top)

and, of its white balance (Qualcomm's AWB), the reference lights (refPtV1): ten of them for every camera
of this phone, each as the sensor sees a grey under it, red over green and blue over green -- from the
bluest daylight to a candle, in the order 10000 K, D75, D65, D50 (whose gains are the AWB's first ones,
initDataV1), cool white and TL84 fluorescent tubes, U30, A (a filament lamp), horizon, candle. Each is
given the colour temperature that the tables above are picked by for it: its own, but 3800 K for U30, a
3000 K light without the infrared a filament gives -- as a white LED is -- whose tables are TL84's (the
wide camera under a warm white LED: its colour shading measured on a white sheet is TL84's table's to
within 3 %, the 2550-2900 K one's is 12 % off in red).

OUT.isp: "RWCISP02"; the number of the matrices' light ranges and per range { lux index from, to (two
floats), the number of temperature ranges, per range { CCT from, to, the matrix (9 floats) } }; the
same for the lens shading, with per temperature range { CCT from, to, R, Gr, Gb, B grids (17 x 13 floats
each) }; the number of reference lights (0 where there are not the ten) and each { R/G, B/G, CCT }.
Everything little-endian, counts as 32-bit words.

Files of the older "Parameter Parser V2.0.0" (THYME's S5KHMX tuning is one)
are read too: their 0x48-byte symbol entries, eight-byte fields and padded
lens-shading grids are described at symbols_v2/module_v2. Their colour
correction lives in cc12 (cc13 is the IPE instance and is identity here), and
their reference lights are not in the V3 layout, so the output has none.
"""
import struct
import sys

GRID = 17 * 13


def symbols(data):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, at = {}, table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += 0x38
    return syms


def module(syms, name, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree, without the flash"""
    led = next(v for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name)
    _, regions, sid = next(e for e in struct.iter_unpack('<3I', led) if e[0] == 0)
    out = []
    for i in range(regions):
        lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<4f2I', syms[sid][1], 24 * i)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            v = struct.unpack_from('<%df' % (2 + floats), cct, size * j)
            ranges.append((v[0], v[1], v[2:]))
        out.append((lux_from, lux_to, ranges))
    return out


# the colour temperature each of the ten reference lights picks the tables by (see above)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


# ---- the older "Parameter Parser V2.0.0", which some phones' tuning files use ---------------------------
#
# V2 keeps the symbol table in 0x48-byte entries (name[0x20], then five 8-byte
# fields: 0, -1, offset, size, id) starting at the "revision" entry; the data
# they point into begins at the pool offset the header gives at 0xc0. A float
# field is stored in the low four bytes of an eight-byte slot; a matrix or a
# lens-shading grid is a packed array of 32-bit floats after the two CCT
# bounds (cct_from at +0, cct_to at +8, the values at +16).
def symbols_v2(data):
    """(by_name, by_id) of the V2 symbol table, or None"""
    try:
        count = struct.unpack_from('<I', data, 0xe8)[0]
        pool = struct.unpack_from('<I', data, 0xc0)[0]
    except struct.error:
        return None
    start = data.find(b'revision')
    if (start < 0 or count < 16 or count > 20000 or
            start + 0x48 * count > len(data) or pool > len(data)):
        return None
    by_name, by_id = {}, {}
    for k in range(count):
        at = start + 0x48 * k
        name = data[at:at + 0x20].split(b'\0')[0].decode('latin1')
        off, size, sid = struct.unpack_from('<3Q', data, at + 0x20 + 16)
        if pool + off + size > len(data):
            continue
        blob = data[pool + off:pool + off + size]
        if name and name not in by_name:
            by_name[name] = blob
        by_id[sid] = blob
    return by_name, by_id


def module_v2(by_name, by_id, name, floats, padded=False):
    """[(lux from, lux to, [(cct from, cct to, values)])] of a V2 module's first tree, without the flash"""
    led = by_name.get('mod_%s_led_idx_data' % name)
    if led is None:
        return []
    sid = None
    for i in range(0, len(led) - 23, 24):
        flash, regions, s = struct.unpack_from('<3Q', led, i)
        if flash == 0 and regions:
            sid = int(s)
            break
    # A V2 reference names the symbol one below the one it means (the file's
    # ids are the symbol's own; a tree's pointers are consistently one less).
    aec = by_id.get(sid + 1)
    if aec is None:
        return []
    out = []
    for i in range(0, len(aec) - 47, 48):
        lux_from = struct.unpack_from('<f', aec, i)[0]
        lux_to = struct.unpack_from('<f', aec, i + 8)[0]
        n = struct.unpack_from('<I', aec, i + 32)[0]
        cct_sid = struct.unpack_from('<I', aec, i + 40)[0]
        cct = by_id.get(cct_sid + 1)
        if not n or cct is None or len(cct) // n < 16 + 4 * floats:
            continue
        size = len(cct) // n
        ranges = []
        for j in range(n):
            base = size * j + 16
            cct_from = struct.unpack_from('<f', cct, base - 16)[0]
            cct_to = struct.unpack_from('<f', cct, base - 8)[0]
            if padded:
                # A lens-shading entry keeps each 17 x 13 grid followed by one
                # zero (222 floats a grid); the .isp wants the grids alone.
                per = floats // 4
                vals = []
                for c in range(4):
                    vals.extend(struct.unpack_from('<%df' % per, cct, base + c * (per + 1) * 4))
                values = tuple(vals)
            else:
                values = struct.unpack_from('<%df' % floats, cct, base)
            ranges.append((cct_from, cct_to, values))
        out.append((lux_from, lux_to, ranges))
    return out


def references(syms):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    f = struct.unpack('<%df' % (len(v) // 4), v)
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header'):
        sys.exit('camtuning.py: not a tuning file of the kind known here')
    if b'Parameter Parser V2.0.0' in data[:0x80]:
        parsed = symbols_v2(data)
        if parsed is None:
            sys.exit('camtuning.py: the V2 symbol table is not where it was expected')
        by_name, by_id = parsed
        # V2's cc13 is the IPE instance's matrix (identity in this phone's
        # file); the sensor's own colour correction is cc12's.
        ccm = module_v2(by_name, by_id, 'cc12' if 'mod_cc12_led_idx_data' in by_name else 'cc13', 9)
        lsc = module_v2(by_name, by_id, 'lsc34', 4 * GRID, padded=True)
        refs = []               # V2's reference lights are not laid out as V3's
        if not ccm:
            sys.exit('camtuning.py: the V2 file has no colour matrices')
    elif b'Parameter Parser V3.0.1' in data[:0x60]:
        syms = symbols(data)
        ccm = module(syms, 'cc13', 9)
        lsc = module(syms, 'lsc34', 4 * GRID)
        refs = references(syms)
    else:
        sys.exit('camtuning.py: not a tuning file of the kind known here')

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    shown = 'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(corners)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)


main()
