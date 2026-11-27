"""Read and check 4 KiB GPT snapshots of the first and last eight blocks."""
import struct
import uuid
import zlib

BS = 4096
APFS_CONTAINER = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def header_of(block):
    if block[:8] != b'EFI PART':
        raise ValueError('no GPT header where one should be')
    size = struct.unpack_from('<I', block, 12)[0]
    if not 92 <= size <= BS:
        raise ValueError('invalid GPT header size')
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    struct.pack_into('<I', h, 16, 0)
    return {
        'size': size, 'crc_right': crc(bytes(h)) == want,
        'my': struct.unpack_from('<Q', block, 24)[0], 'alternate': struct.unpack_from('<Q', block, 32)[0],
        'first_usable': struct.unpack_from('<Q', block, 40)[0], 'last_usable': struct.unpack_from('<Q', block, 48)[0],
        'disk_guid': block[56:72],
        'entries_lba': struct.unpack_from('<Q', block, 72)[0], 'count': struct.unpack_from('<I', block, 80)[0],
        'entry_size': struct.unpack_from('<I', block, 84)[0], 'entries_crc': struct.unpack_from('<I', block, 88)[0],
    }


def seal(block, entries):
    """block with its header's entries CRC and header CRC made right for these entries"""
    b = bytearray(block)
    h = header_of(b)
    struct.pack_into('<I', b, 88, crc(entries))
    struct.pack_into('<I', b, 16, 0)
    struct.pack_into('<I', b, 16, crc(bytes(b[:h['size']])))
    return bytes(b)


def entries_of(blob, h, base):
    """the table bytes and its parsed entries, from blob that starts at LBA base"""
    at = (h['entries_lba'] - base) * BS
    size = h['count'] * h['entry_size']
    if h['count'] == 0 or h['entry_size'] < 128 or at < 0 or at + size > len(blob):
        raise ValueError('the partition table is not within the saved blocks')
    raw = blob[at:at + size]
    parsed = []
    for i in range(h['count']):
        e = raw[i * h['entry_size']:(i + 1) * h['entry_size']]
        parsed.append({
            'type': uuid.UUID(bytes_le=e[:16]), 'guid': uuid.UUID(bytes_le=e[16:32]),
            'first': struct.unpack_from('<Q', e, 32)[0], 'last': struct.unpack_from('<Q', e, 40)[0],
            'attrs': struct.unpack_from('<Q', e, 48)[0],
            'name': e[56:128].decode('utf-16le').split('\0', 1)[0],
        })
    return raw, parsed, at


def check(name, head, tail, disk):
    """both copies: header CRCs, table CRCs, where each says the other is; the parsed table"""
    ph = header_of(head[BS:2 * BS])
    bh = header_of(tail[7 * BS:8 * BS])
    praw, pe, _ = entries_of(head, ph, 0)
    braw, _, _ = entries_of(tail, bh, disk - 8)
    first, last = ph['first_usable'], ph['last_usable']
    primary_end = ph['entries_lba'] + (len(praw) + BS - 1) // BS
    backup_end = bh['entries_lba'] + (len(braw) + BS - 1) // BS
    extents = sorted((e['first'], e['last']) for e in pe if e['type'].int != 0)
    layout_ok = (2 <= ph['entries_lba'] < primary_end <= first <= last < bh['entries_lba'] < backup_end <= disk - 1
                 and all(first <= start <= end <= last for start, end in extents)
                 and all(a[1] < b[0] for a, b in zip(extents, extents[1:])))
    ok = (ph['crc_right'] and bh['crc_right'] and crc(praw) == ph['entries_crc'] and crc(braw) == bh['entries_crc']
          and ph['my'] == 1 and ph['alternate'] == disk - 1 and bh['my'] == disk - 1 and bh['alternate'] == 1
          and praw == braw and layout_ok
          and all(ph[key] == bh[key] for key in ('first_usable', 'last_usable', 'count', 'entry_size', 'disk_guid')))
    print(f'{name}: primary header crc {"right" if ph["crc_right"] else "WRONG"}, table crc '
          f'{"right" if crc(praw) == ph["entries_crc"] else "WRONG"}; backup header crc '
          f'{"right" if bh["crc_right"] else "WRONG"}, table crc {"right" if crc(braw) == bh["entries_crc"] else "WRONG"}; '
          f'the two tables {"the same" if praw == braw else "DIFFER"} -> {"good" if ok else "BAD"}')
    return ok, ph, bh, pe


def read_pair(head_path, tail_path):
    """Return the two saved regions and the number of blocks in their disk."""
    with open(head_path, 'rb') as source:
        head = source.read(8 * BS + 1)
    with open(tail_path, 'rb') as source:
        tail = source.read(8 * BS + 1)
    if len(head) != 8 * BS or len(tail) != 8 * BS:
        raise ValueError('GPT head and tail snapshots must each contain eight 4 KiB blocks')
    disk = header_of(tail[7 * BS:8 * BS])['my'] + 1
    return head, tail, disk
