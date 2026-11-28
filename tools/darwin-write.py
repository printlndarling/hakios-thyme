#!/usr/bin/env python3
"""Write an image into the phone's darwin partition, and nowhere else, from Android (adb, root).

usage: darwin-write.py [--gpt HEAD TAIL] COMMAND

Commands:
  darwin-write.py check                 darwin's identity, read from the phone now                (writes nothing)
  darwin-write.py sum   BLOCK COUNT     sha256 of COUNT blocks of darwin from BLOCK                (writes nothing)
  darwin-write.py plan  IMAGE BLOCK     every check `write` makes, and the command it would run    (writes nothing)
  darwin-write.py write IMAGE BLOCK     with DARWIN_WRITE=yes: IMAGE at darwin's block BLOCK, read back and compared
  darwin-write.py zero  BLOCK COUNT     with DARWIN_WRITE=yes: zeros over COUNT blocks, read back and compared
  darwin-write.py selftest              the refusals, against this tool's own checks and a GPT read now (writes nothing)

Blocks are counted from darwin's start, in units of 4096 bytes.
With --gpt, use an approved pair of GPT snapshots of UFS logical unit 0.
Both copies must validate and match the current phone byte for byte. The
partition node, number, name, start and size must also agree. This mode
allows writes only from the data boundary at block 2042368.
Without --gpt the tool retains the legacy sda34 identity and its original
write range, including the cryptexes from block 1374208 onward.

The writer stages and hashes the input, checks the partition immediately
before writing, and checks the written blocks against the expected digest.
Only /dev/block/by-name/darwin is used as an output.

Every attempt, refused or done, goes into logs/darwin-write.log.
"""
import argparse
import binascii
from dataclasses import dataclass
import hashlib
import os
import shlex
import struct
import subprocess
import sys
import time
import uuid

from gpt import BS, read_pair
from mkdatavol import DATA_FIRST, darwin_entry

NAME = 'darwin'
TYPE = '7C3457EF-0000-11AA-AA11-00306543ECAC'          # Apple APFS
UNIQUE = 'FD6854AD-24AA-4FC3-94FE-290116858D68'
LUN = 0
PARTN = 34
FIRST = 25374720
LAST = 30617594
BLOCKS = LAST - FIRST + 1                               # 5242875
PROTECTED = 1374208                                     # rootfs.apfs: 5628755968 bytes, never written
DEV = '/dev/block/by-name/darwin'
NODE = '/dev/block/sda34'

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'logs', 'darwin-write.log')


class Refused(Exception):
    pass


@dataclass(frozen=True)
class PlannedDarwin:
    number: int
    first: int
    last: int
    guid: uuid.UUID
    disk_blocks: int
    head_sha256: str
    tail_sha256: str

    @property
    def node(self):
        return f'/dev/block/sda{self.number}'

    @property
    def blocks(self):
        return self.last - self.first + 1


def planned_darwin(head_path, tail_path):
    head, tail, disk = read_pair(head_path, tail_path)
    number, entry = darwin_entry(head, tail, disk)
    return PlannedDarwin(number, entry['first'], entry['last'], entry['guid'], disk,
                         hashlib.sha256(head).hexdigest(), hashlib.sha256(tail).hexdigest())


def planned_guard(target):
    # This same check runs in the shell that opens the device. dd only
    # reads from the disk node here and the supplied hashes are hex.
    return f'''set -e
set -o pipefail
expect() {{
    if [ "$2" != "$3" ]; then
        echo "REFUSED: $1 is '$2', expected '$3'" >&2
        exit 3
    fi
}}
expect node "$(readlink -f {DEV} || true)" {target.node}
case "$(readlink -f /sys/class/block/sda/device)" in
    *ufshc*:0) ;;
    *) echo "REFUSED: sda is not UFS logical unit 0" >&2; exit 3 ;;
esac
test -b {DEV}
expect disk-block-size "$(blockdev --getss /dev/block/sda)" {BS}
expect disk-bytes "$(blockdev --getsize64 /dev/block/sda)" {target.disk_blocks * BS}
expect partition-number "$(cat /sys/class/block/sda{target.number}/partition)" {target.number}
expect partition-name "$(sed -n 's/^PARTNAME=//p' /sys/class/block/sda{target.number}/uevent)" {NAME}
expect partition-start "$(cat /sys/class/block/sda{target.number}/start)" {target.first * BS // 512}
expect partition-size "$(cat /sys/class/block/sda{target.number}/size)" {target.blocks * BS // 512}
expect partition-bytes "$(blockdev --getsize64 {DEV})" {target.blocks * BS}
head_sum=$(dd if=/dev/block/sda bs={BS} count=8 2>/dev/null | sha256sum)
tail_sum=$(dd if=/dev/block/sda bs={BS} skip={target.disk_blocks - 8} count=8 2>/dev/null | sha256sum)
expect GPT-head "${{head_sum%% *}}" {target.head_sha256}
expect GPT-tail "${{tail_sum%% *}}" {target.tail_sha256}
'''


def log(line):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), line))
    print('darwin-write: ' + line)


def phone(script, binary=False):
    """Run a script in the phone's root shell (on its standard input, as flash-fingerprint.sh does)."""
    r = subprocess.run(['adb', 'shell', 'su -c sh'], input=script.encode(), capture_output=True, timeout=3600)
    if r.returncode != 0:
        raise Refused('the phone said %d: %s' % (r.returncode, (r.stdout + r.stderr).decode(errors='replace')[-400:]))
    return r.stdout if binary else r.stdout.decode(errors='replace')


def phone_bytes(command):
    r = subprocess.run(['adb', 'exec-out', 'su -c "%s"' % command], capture_output=True, timeout=600)
    if r.returncode != 0:
        raise Refused('reading from the phone: exit %d' % r.returncode)
    return r.stdout


def check_gpt(raw):
    """raw: LUN 0's blocks 1.. (header, then the table). Returns the problems found."""
    bad = []
    hdr = raw[:BS]
    if hdr[:8] != b'EFI PART':
        return ['no GPT header at LBA 1']
    hsize = struct.unpack_from('<I', hdr, 12)[0]
    hcrc = struct.unpack_from('<I', hdr, 16)[0]
    h = bytearray(hdr[:hsize])
    h[16:20] = b'\0\0\0\0'
    if binascii.crc32(bytes(h)) & 0xffffffff != hcrc:
        bad.append('GPT header CRC wrong')
    last_usable = struct.unpack_from('<Q', hdr, 48)[0]
    ent_lba, n, esize = struct.unpack_from('<QII', hdr, 72)
    tcrc = struct.unpack_from('<I', hdr, 88)[0]
    if ent_lba != 2 or esize != 128 or n * esize > len(raw) - BS:
        return bad + ['GPT table not where it was (LBA %d, %d entries of %d)' % (ent_lba, n, esize)]
    table = raw[BS:BS + n * esize]
    if binascii.crc32(table) & 0xffffffff != tcrc:
        bad.append('GPT table CRC wrong')
    if last_usable < LAST:
        bad.append('last usable LBA %d is before darwin\'s end' % last_usable)
    found = []
    for i in range(n):
        e = table[i * esize:(i + 1) * esize]
        if e[:16] == b'\0' * 16:
            continue
        name = e[56:128].decode('utf-16le', errors='replace').rstrip('\0')
        first, last = struct.unpack_from('<QQ', e, 32)
        if name == NAME:
            found.append((i + 1, str(uuid.UUID(bytes_le=e[:16])).upper(), str(uuid.UUID(bytes_le=e[16:32])).upper(),
                          first, last))
        elif first <= LAST and last >= FIRST:
            bad.append('entry %d (%s, LBA %d..%d) overlaps darwin' % (i + 1, name, first, last))
    if len(found) != 1:
        return bad + ['%d entries named darwin' % len(found)]
    num, typ, uniq, first, last = found[0]
    for what, got, want in (('number', num, PARTN), ('type', typ, TYPE), ('unique GUID', uniq, UNIQUE),
                            ('first LBA', first, FIRST), ('last LBA', last, LAST)):
        if got != want:
            bad.append('darwin\'s %s is %s, not %s' % (what, got, want))
    return bad


def identity(target=None):
    """Read darwin's identity now and compare it with the expected layout."""
    if target is not None:
        phone(planned_guard(target))
        return (f'darwin is {target.node}, entry {target.number} of logical unit {LUN}, '
                f'LBA {target.first}..{target.last} ({target.blocks} blocks of {BS}), {TYPE}, {target.guid}')
    out = phone('''
echo "node=$(readlink -f %(dev)s)"
echo "lun=$(readlink -f /sys/class/block/sda/device)"
echo "partition=$(cat /sys/class/block/sda%(n)d/partition)"
echo "start=$(cat /sys/class/block/sda%(n)d/start)"
echo "size=$(cat /sys/class/block/sda%(n)d/size)"
echo "name=$(grep ^PARTNAME= /sys/class/block/sda%(n)d/uevent | cut -d= -f2)"
echo "bytes=$(blockdev --getsize64 %(dev)s)"
echo "ss=$(blockdev --getss /dev/block/sda)"
''' % {'dev': DEV, 'n': PARTN})
    facts = dict(line.split('=', 1) for line in out.split('\n') if '=' in line)
    bad = []
    want = {'node': NODE, 'partition': str(PARTN), 'start': str(FIRST * BS // 512), 'size': str(BLOCKS * BS // 512),
            'name': NAME, 'bytes': str(BLOCKS * BS), 'ss': str(BS)}
    for k, v in want.items():
        if facts.get(k) != v:
            bad.append('%s is %r, not %r' % (k, facts.get(k), v))
    lun = facts.get('lun', '')
    if 'ufshc' not in lun or not lun.endswith(':%d' % LUN):
        bad.append('sda is %r, not logical unit %d of the UFS controller' % (lun, LUN))
    # the GPT: LBA 1 (header) and the table after it (64 entries of 128 bytes: LBA 2..3)
    raw = phone_bytes('dd if=/dev/block/sda bs=%d skip=1 count=3 2>/dev/null' % BS)
    if len(raw) != 3 * BS:
        bad.append('read %d bytes of the GPT, not %d' % (len(raw), 3 * BS))
    else:
        bad += check_gpt(raw)
    if bad:
        raise Refused('darwin is not what it was: ' + '; '.join(bad))
    return 'darwin is %s, entry %d of logical unit %d, LBA %d..%d (%d blocks of %d), %s, %s' % (
        NODE, PARTN, LUN, FIRST, LAST, BLOCKS, BS, TYPE, UNIQUE)


def blocks_ok(block, count, target=None):
    """The range rule; raises Refused."""
    if not (isinstance(block, int) and isinstance(count, int)):
        raise Refused('blocks must be whole numbers')
    if count <= 0:
        raise Refused('nothing to write (%d blocks)' % count)
    if target is not None:
        if block < DATA_FIRST:
            raise Refused(f'data write starts at block {DATA_FIRST} or later')
    elif block < PROTECTED:
        raise Refused("block %d is inside the Darwin volume (blocks 0..%d), which is never written" % (block, PROTECTED - 1))
    blocks_ok_read(block, count, target)


def number(s):
    if not s.isdigit():
        raise Refused('%r is not a block number' % s)
    return int(s)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                return h.hexdigest()
            h.update(b)


def sha256_zeros(n):
    h = hashlib.sha256()
    z = b'\0' * (1 << 20)
    while n > 0:
        h.update(z[:min(n, len(z))])
        n -= len(z)
    return h.hexdigest()


def phone_sum(block, count, target=None):
    blocks_ok_read(block, count, target)
    command = f'set -o pipefail\ndd if={DEV} bs={BS} skip={block} count={count} 2>/dev/null | sha256sum\n'
    out = phone(planned_guard(target) + command if target is not None else command).split()
    if not out or len(out[0]) != 64:
        raise Refused('readback did not return a SHA-256 digest')
    return out[0]


def blocks_ok_read(block, count, target=None):
    blocks = target.blocks if target is not None else BLOCKS
    if count <= 0 or block < 0 or block + count > blocks:
        raise Refused('blocks %d..%d are not inside darwin (%d blocks)' % (block, block + count - 1, blocks))


def guarded(command, target=None):
    """Check the name and identity in the same shell immediately before the write."""
    if target is not None:
        return planned_guard(target) + command + '\n'
    return ('t=$(readlink -f %(dev)s); s=$(blockdev --getsize64 %(dev)s)\n'
            'if [ "$t" = %(node)s ] && [ "$s" = %(bytes)d ]; then %(cmd)s 2>&1; else echo "REFUSED: $t $s"; exit 3; fi\n'
            % {'dev': DEV, 'node': NODE, 'bytes': BLOCKS * BS, 'cmd': command})


def prepare(image, block, target=None):
    size = os.path.getsize(image)
    if size % BS:
        raise Refused('%s is %d bytes, not whole blocks of %d' % (image, size, BS))
    count = size // BS
    blocks_ok(block, count, target)
    return count


def cmd_plan(image, block, target=None, write=False):
    count = prepare(image, block, target)
    who = identity(target)
    digest = sha256_file(image)
    temporary = '/data/local/tmp/darwin-write-' + uuid.uuid4().hex + '.img'
    dd = 'dd if=%s of=%s bs=%d seek=%d count=%d conv=notrunc,fsync' % (temporary, DEV, BS, block, count)
    log('%s: %s (%d blocks, sha256 %s) at darwin blocks %d..%d (bytes %d..%d); %s; would run: %s' % (
        'write' if write else 'plan', image, count, digest, block, block + count - 1, block * BS,
        (block + count) * BS - 1, who, dd))
    if not write:
        return
    if os.environ.get('DARWIN_WRITE') != 'yes':
        raise Refused('DARWIN_WRITE=yes is not set: nothing written')
    try:
        r = subprocess.run(['adb', 'push', image, temporary], capture_output=True, text=True)
        if r.returncode != 0:
            raise Refused('adb push: ' + r.stderr.strip())
        there = phone('sha256sum %s\n' % shlex.quote(temporary)).split()
        if not there or there[0] != digest:
            raise Refused('the staged image digest differs from the approved input')
        identity(target)
        out = phone(guarded(dd, target))
        log('dd: ' + ' '.join(out.split()))
        back = phone_sum(block, count, target)
        log('read back blocks %d..%d: sha256 %s -- %s' % (block, block + count - 1, back,
                                                          'the same' if back == digest else 'DIFFERENT'))
        if back != digest:
            raise Refused('what was read back is not what was written')
    finally:
        phone('rm -f %s\n' % shlex.quote(temporary))


def cmd_zero(block, count, target=None):
    blocks_ok(block, count, target)
    who = identity(target)
    dd = 'dd if=/dev/zero of=%s bs=%d seek=%d count=%d conv=notrunc,fsync' % (DEV, BS, block, count)
    log('zero: darwin blocks %d..%d; %s; runs: %s' % (block, block + count - 1, who, dd))
    if os.environ.get('DARWIN_WRITE') != 'yes':
        raise Refused('DARWIN_WRITE=yes is not set: nothing written')
    out = phone(guarded(dd, target))
    log('dd: ' + ' '.join(out.split()))
    back, want = phone_sum(block, count, target), sha256_zeros(count * BS)
    log('read back blocks %d..%d: %s' % (block, block + count - 1, 'zeros' if back == want else 'NOT ZEROS ' + back))
    if back != want:
        raise Refused('what was read back is not zeros')


def cmd_selftest():
    """The refusals: each must refuse, and none of them reaches the phone's dd."""
    ok = True
    for block, count, why in ((PROTECTED - 1, 256, 'one block into the Darwin volume'), (0, 1, 'the Darwin volume\'s superblock'),
                              (BLOCKS - 100, 256, 'past darwin\'s end'), (BLOCKS, 1, 'right after darwin'),
                              (-1, 1, 'before darwin'), (PROTECTED, 0, 'nothing'), (10 ** 12, 1, 'far away')):
        try:
            blocks_ok(block, count)
            log('selftest: %s (blocks %d+%d) was NOT refused' % (why, block, count))
            ok = False
        except Refused as e:
            log('selftest: %s refused: %s' % (why, e))
    for s in ('-5', '1e6', '0x100', ''):
        try:
            number(s)
            ok = False
            log('selftest: block %r was NOT refused' % s)
        except Refused as e:
            log('selftest: block %r refused: %s' % (s, e))
    raw = phone_bytes('dd if=/dev/block/sda bs=%d skip=1 count=3 2>/dev/null' % BS)
    log('selftest: the GPT read now: %s' % ('; '.join(check_gpt(raw)) or 'darwin as it was'))
    ok &= not check_gpt(raw)
    # the same GPT changed in one place at a time: each must be refused
    def entry_off(field):
        return BS + (PARTN - 1) * 128 + field
    for what, off, value in (('a type GUID byte', entry_off(0), None), ('a unique GUID byte', entry_off(20), None),
                             ('the first LBA', entry_off(32), struct.pack('<Q', FIRST - 8)),
                             ('the last LBA', entry_off(40), struct.pack('<Q', LAST + 1)),
                             ('the name', entry_off(56), 'Darwin'.encode('utf-16le'))):
        bad = bytearray(raw)
        if value is None:
            bad[off] ^= 0x01
        else:
            bad[off:off + len(value)] = value
        # with the table's CRC made right again, so that the field itself is what is caught
        n = struct.unpack_from('<I', bad, 80)[0]
        struct.pack_into('<I', bad, 88, binascii.crc32(bytes(bad[BS:BS + n * 128])) & 0xffffffff)
        h = bytearray(bad[:struct.unpack_from('<I', bad, 12)[0]])
        h[16:20] = b'\0\0\0\0'
        struct.pack_into('<I', bad, 16, binascii.crc32(bytes(h)) & 0xffffffff)
        problems = check_gpt(bytes(bad))
        log('selftest: GPT with %s changed: %s' % (what, '; '.join(problems) if problems else 'NOT refused'))
        ok &= bool(problems)
    bad = bytearray(raw)
    bad[BS + (PARTN - 1) * 128 + 40] ^= 0x01                 # without fixing the CRC: the CRC catches it
    problems = check_gpt(bytes(bad))
    log('selftest: GPT with a byte changed and the CRC left: %s' % ('; '.join(problems) or 'NOT refused'))
    ok &= bool(problems)
    log('selftest: %s' % ('every refusal refused, the GPT as it was' if ok else 'FAILED'))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--gpt', nargs=2, metavar=('HEAD', 'TAIL'), help='approved paired GPT snapshots of UFS LUN 0')
    parser.add_argument('command', choices=('check', 'sum', 'plan', 'write', 'zero', 'selftest'))
    parser.add_argument('args', nargs='*')
    args = parser.parse_args()
    command, a = args.command, args.args
    try:
        target = planned_darwin(*args.gpt) if args.gpt else None
        if command == 'check' and not a:
            log('check: ' + identity(target))
        elif command == 'sum' and len(a) == 2:
            block, count = number(a[0]), number(a[1])
            identity(target)
            log('sum: darwin blocks %d..%d: sha256 %s (zeros would be %s)' % (block, block + count - 1,
                                                                            phone_sum(block, count, target),
                                                                            sha256_zeros(count * BS)))
        elif command in ('plan', 'write') and len(a) == 2:
            cmd_plan(a[0], number(a[1]), target, write=(command == 'write'))
        elif command == 'zero' and len(a) == 2:
            cmd_zero(number(a[0]), number(a[1]), target)
        elif command == 'selftest' and not a and target is None:
            sys.exit(0 if cmd_selftest() else 1)
        else:
            parser.error('incorrect arguments for ' + command)
    except (Refused, OSError, ValueError, subprocess.SubprocessError) as error:
        log('REFUSED %s: %s' % (' '.join([command] + a), error))
        sys.exit(2)


if __name__ == '__main__':
    main()
