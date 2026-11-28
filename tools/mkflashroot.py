#!/usr/bin/env python3
"""Pack the read-only Darwin and cryptex APFS containers for the darwin partition."""
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile

from apfs.apfs import NX_MAGIC, checksum_ok
from gpt import BS
from mkdatavol import DATA_FIRST


def container_blocks(source, role):
    """Validate the on-disk span that the kernel uses to locate the next container."""
    info = os.fstat(source.fileno())
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f'{role}: expected a regular file')
    header = source.read(BS)
    if len(header) != BS or header[32:36] != NX_MAGIC:
        raise ValueError(f'{role}: expected raw APFS at the start of the file')
    if not checksum_ok(header):
        raise ValueError(f'{role}: the APFS container superblock checksum is invalid')
    block_size, blocks = struct.unpack_from('<IQ', header, 36)
    if block_size != BS or not blocks or blocks * BS != info.st_size:
        raise ValueError(f'{role}: the APFS container span does not match the file size in {BS}-byte blocks')
    source.seek(0)
    return blocks


def pack(paths, output):
    roles = ('Darwin root', 'OS cryptex', 'App cryptex')
    output = Path(output)
    with ExitStack() as opened:
        files = []
        for path, role in zip(paths, roles):
            path = Path(path)
            if not stat.S_ISREG(path.stat().st_mode):
                raise ValueError(f'{role}: expected a regular file')
            files.append(opened.enter_context(path.open('rb')))
        if output.exists():
            existing = output.stat()
            if not stat.S_ISREG(existing.st_mode):
                raise ValueError('the output must be a regular file')
            if any(os.path.samestat(existing, os.fstat(source.fileno())) for source in files):
                raise ValueError('the output also names an input')
        spans = [container_blocks(source, role) for source, role in zip(files, roles)]
        if sum(spans) > DATA_FIRST:
            raise ValueError(f'the three APFS containers cross the HFSX data boundary at block {DATA_FIRST}')

        # Do not replace an existing image unless every container was copied.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f'.{output.name}.',
                                             delete=False) as destination:
                temporary = Path(destination.name)
                start = 0
                for source, role, blocks in zip(files, roles, spans):
                    end = start + blocks
                    shutil.copyfileobj(source, destination, 1024 * 1024)
                    if destination.tell() != end * BS:
                        raise ValueError(f'{role}: the input changed size during copying')
                    print(f'{role}: partition blocks {start}–{end - 1}')
                    start = end
            os.replace(temporary, output)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    print(f'{output}: {sum(spans)} blocks. Data volume begins at block {DATA_FIRST}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', help='raw APFS root volume from the local IPSW')
    parser.add_argument('os_cryptex', help='raw APFS OS cryptex')
    parser.add_argument('app_cryptex', help='raw APFS App cryptex')
    parser.add_argument('output', help='output partition prefix image')
    args = parser.parse_args()
    try:
        pack((args.root, args.os_cryptex, args.app_cryptex), args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'mkflashroot.py: {error}\n')


if __name__ == '__main__':
    main()
