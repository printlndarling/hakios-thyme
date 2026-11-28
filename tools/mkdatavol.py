#!/usr/bin/env python3
"""The data volume of Darwin for the phone: an empty HFS Plus volume in darwin past the Darwin volume and the cryptexes,
as an Android sparse image of the whole of darwin that writes that volume and nothing else.

usage: mkdatavol.py [--gpt HEAD TAIL] OUT.simg [MKFS]

For a new layout, pass the planned or read-back GPT snapshots (eight 4 KiB
blocks each) from the same LUN 0. Omitting them selects the legacy size.
MKFS defaults to mkfs.hfsplus in $PATH.

    fastboot flash darwin OUT.simg

The kernel gives that stretch of darwin out as /dev/ufs_darwin_data (drivers/storage/hakios.ufs.c, ufs_write=1) and
init mounts it on /private/var (userland/src/init.c, var_hfs=ufs_darwin_data). Flashing this again empties it.

The image marks the Darwin volume and both cryptexes DONT_CARE, along with
the HFS free space. It emits RAW or FILL chunks for all allocated HFS blocks,
including both volume headers, the allocation bitmap, trees and the journal.

Case-sensitive (HFSX), as Darwin's own data volume is, and journaled.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

from gpt import APFS_CONTAINER, BS, check, read_pair

MIN_DARWIN_BLOCKS = 5242875        # BLOCK_DARWIN_MIN in xnuloader/loader.c
HFS_JOURNALED = 1 << 13
DATA_FIRST = 2042368               # where the volume begins: drivers/storage/hakios.ufs.c, UFS_WRITE_FIRST
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def darwin_entry(head, tail, disk):
    ok, header, _, entries = check('darwin GPT', head, tail, disk)
    if not ok:
        raise ValueError('the two GPT copies must validate and agree')
    darwin = [(i, e) for i, e in enumerate(entries, 1) if e['type'].int and e['name'] == 'darwin']
    if len(darwin) != 1:
        raise ValueError('expected exactly one GPT partition named darwin')
    number, entry = darwin[0]
    if entry['type'] != APFS_CONTAINER or entry['guid'].int == 0 or entry['last'] != header['last_usable']:
        raise ValueError('darwin must have an APFS type, a unique GUID and reach the last usable LBA')
    if sum(part['type'].int != 0 and part['guid'] == entry['guid'] for part in entries) != 1:
        raise ValueError('darwin GUID also identifies another GPT partition')
    blocks = entry['last'] - entry['first'] + 1
    if not MIN_DARWIN_BLOCKS <= blocks <= 0xffffffff:
        raise ValueError(f'darwin has {blocks} blocks outside the supported range')
    return number, entry


def darwin_blocks_from_gpt(head_path, tail_path):
    number, entry = darwin_entry(*read_pair(head_path, tail_path))
    blocks = entry['last'] - entry['first'] + 1
    print(f'darwin: GPT entry {number}, LBA {entry["first"]}..{entry["last"]}, {blocks} blocks')
    return blocks


def volume_header(img, offset, blocks):
    img.seek(offset)
    vh = img.read(512)
    if len(vh) != 512 or vh[:2] != b'HX' or not (struct.unpack_from('>I', vh, 4)[0] & HFS_JOURNALED):
        raise ValueError(f'expected a journaled HFSX volume header at byte {offset}')
    if struct.unpack_from('>II', vh, 40) != (BS, blocks):
        raise ValueError(f'HFSX volume header at byte {offset} has the wrong geometry')
    return vh


def used_blocks(img, blocks):
    """Read the allocation bitmap and require both volume headers in written blocks."""
    vh = volume_header(img, 1024, blocks)
    volume_header(img, blocks * BS - 1024, blocks)
    fork = vh[112:192]                                  # allocationFile: HFSPlusForkData
    extents = [struct.unpack_from('>II', fork, 16 + 8 * i) for i in range(8)]
    bitmap = bytearray()
    for start, count in extents:
        if count:
            if start >= blocks or count > blocks - start:
                raise ValueError('the allocation bitmap extent leaves the HFSX volume')
            img.seek(start * BS)
            bitmap += img.read(count * BS)
    if len(bitmap) * 8 < blocks:
        raise ValueError('the allocation file is in more pieces than its fork says')

    def used(block):
        return bitmap[block >> 3] & (0x80 >> (block & 7))

    if not used(0) or not used(blocks - 1):
        raise ValueError('the primary and alternate header blocks must be marked in use')
    return used


def main():
    parser = argparse.ArgumentParser(description='Build a journaled HFSX data image on the host')
    parser.add_argument('--gpt', nargs=2, metavar=('HEAD', 'TAIL'), help='paired LUN 0 GPT snapshots')
    parser.add_argument('out', metavar='OUT.simg')
    parser.add_argument('mkfs', metavar='MKFS', nargs='?', default='mkfs.hfsplus')
    args = parser.parse_args()
    darwin_blocks = darwin_blocks_from_gpt(*args.gpt) if args.gpt else MIN_DARWIN_BLOCKS
    blocks = darwin_blocks - DATA_FIRST
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)
        # its root is /private/var: root's, 0755, as on an iPhone
        subprocess.run([args.mkfs, '-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data', path],
                       check=True)
        with open(path, 'rb') as img:
            used = used_blocks(img, blocks)
            runs = []                                   # [kind, first, count], in order
            for b in range(blocks):
                kind = RAW if used(b) else DONT_CARE
                if runs and runs[-1][0] == kind and runs[-1][1] + runs[-1][2] == b:
                    runs[-1][2] += 1
                else:
                    runs.append([kind, b, 1])
            chunks, written = [chunk(DONT_CARE, DATA_FIRST, 0)], 0
            for kind, first, count in runs:
                if kind == DONT_CARE:
                    chunks.append(chunk(DONT_CARE, count, 0))
                    continue
                img.seek(first * BS)
                data = img.read(count * BS)
                # zeros in a used stretch go as FILL, which costs nothing to send; everything else as RAW
                at = 0
                while at < count:
                    zero = data[at * BS:(at + 1) * BS] == bytes(BS)
                    end = at
                    while end < count and (data[end * BS:(end + 1) * BS] == bytes(BS)) == zero:
                        end += 1
                    if zero:
                        chunks.append(chunk(FILL, end - at, 4) + bytes(4))
                    else:
                        chunks.append(chunk(RAW, end - at, (end - at) * BS) + data[at * BS:end * BS])
                    written += end - at
                    at = end
    with open(args.out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, darwin_blocks, len(chunks), 0))
        for c in chunks:
            f.write(c)
    print(f'{args.out}: {os.path.getsize(args.out)} bytes; the volume: {blocks} blocks ({blocks * BS >> 20} MB) from darwin '
          f'block {DATA_FIRST}, {written} of them written, the remaining blocks marked DONT_CARE')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'mkdatavol.py: {error}')
