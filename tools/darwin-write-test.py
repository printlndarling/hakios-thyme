#!/usr/bin/env python3
"""Check the approved GPT guard using synthetic snapshots and a mock Android shell."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import uuid

from gpt import APFS_CONTAINER, BS, crc, seal
from mkdatavol import DATA_FIRST, MIN_DARWIN_BLOCKS

spec = importlib.util.spec_from_file_location('darwin_write', Path(__file__).with_name('darwin-write.py'))
writer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = writer
spec.loader.exec_module(writer)

DISK = 8_000_000
PARTITION = 36
BLOCKS = MIN_DARWIN_BLOCKS + 512
LAST = DISK - 6
FIRST = LAST - BLOCKS + 1


def write_gpt_pair(head_path, tail_path):
    table = bytearray(64 * 128)
    start = (PARTITION - 1) * 128
    struct.pack_into('<16s16sQQ', table, start, APFS_CONTAINER.bytes_le, uuid.UUID(int=12).bytes_le, FIRST, LAST)
    table[start + 56:start + 68] = 'darwin'.encode('utf-16le')

    def header(my, other, entries):
        raw = bytearray(BS)
        struct.pack_into('<8sIIIIQQQQ16sQIII', raw, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         my, other, 4, LAST, uuid.UUID(int=7).bytes_le, entries, 64, 128, crc(table))
        return seal(raw, table)

    head = bytearray(8 * BS)
    tail = bytearray(8 * BS)
    head[BS:2 * BS] = header(1, DISK - 1, 2)
    head[2 * BS:4 * BS] = table
    tail[5 * BS:7 * BS] = table
    tail[7 * BS:] = header(DISK - 1, 1, DISK - 3)
    head_path.write_bytes(head)
    tail_path.write_bytes(tail)


class DarwinWriter(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        directory = Path(temp.name)
        self.head, self.tail = directory / 'head', directory / 'tail'
        self.image = directory / 'image'
        self.image.write_bytes(bytes(BS))
        write_gpt_pair(self.head, self.tail)
        with contextlib.redirect_stdout(io.StringIO()):
            self.target = writer.planned_darwin(self.head, self.tail)

    def run_guard(self, **overrides):
        prelude = f'''
readlink() {{
    case "$2" in
        {writer.DEV}) printf '%s\\n' "$FAKE_NODE" ;;
        /sys/class/block/sda/device) printf '%s\\n' "$FAKE_LUN" ;;
        *) return 9 ;;
    esac
}}
blockdev() {{
    case "$*" in
        '--getss /dev/block/sda') echo {BS} ;;
        '--getsize64 /dev/block/sda') echo {DISK * BS} ;;
        '--getsize64 {writer.DEV}') echo {BLOCKS * BS} ;;
        *) return 9 ;;
    esac
}}
cat() {{
    case "$1" in
        /sys/class/block/sda{PARTITION}/partition) echo {PARTITION} ;;
        /sys/class/block/sda{PARTITION}/start) echo "$FAKE_START" ;;
        /sys/class/block/sda{PARTITION}/size) echo {BLOCKS * BS // 512} ;;
        *) return 9 ;;
    esac
}}
sed() {{ echo darwin; }}
test() {{ builtin test "$1" = '-b' && builtin test "$2" = '{writer.DEV}'; }}
dd() {{
    case "$*" in
        'if=/dev/block/sda bs={BS} count=8') command cat "$FAKE_HEAD" ;;
        'if=/dev/block/sda bs={BS} skip={DISK - 8} count=8') command cat "$FAKE_TAIL" ;;
        *) return 9 ;;
    esac
}}
'''
        env = dict(os.environ, FAKE_NODE=self.target.node, FAKE_START=str(FIRST * BS // 512),
                   FAKE_LUN='/sys/devices/soc/1d84000.ufshc/host0/target0:0:0/0:0:0:0',
                   FAKE_HEAD=str(self.head), FAKE_TAIL=str(self.tail))
        env.update(overrides)
        return subprocess.run(['bash'], input=prelude + writer.guarded('echo allowed', self.target),
                              capture_output=True, text=True, env=env, check=False)

    def test_approved_sda36_matches_live_facts(self):
        self.assertEqual((self.target.number, self.target.blocks), (PARTITION, BLOCKS))
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'allowed')

    def test_live_changes_cannot_reach_the_command(self):
        changed = self.tail.with_name('changed')
        data = bytearray(self.tail.read_bytes())
        data[0] ^= 1
        changed.write_bytes(data)
        for changes, message in (
            ({'FAKE_NODE': '/dev/block/sda35'}, 'node'),
            ({'FAKE_LUN': '/sys/devices/soc/ufshc/0:0:0:1'}, 'logical unit 0'),
            ({'FAKE_START': str(FIRST * BS // 512 + 8)}, 'partition-start'),
            ({'FAKE_HEAD': str(changed)}, 'GPT-head'),
            ({'FAKE_TAIL': str(changed)}, 'GPT-tail'),
        ):
            with self.subTest(changes=changes):
                result = self.run_guard(**changes)
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertNotIn('allowed', result.stdout)

    def test_damaged_approved_copy_is_refused(self):
        tail = bytearray(self.tail.read_bytes())
        tail[7 * BS + 16] ^= 1
        self.tail.write_bytes(tail)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, 'validate and agree'):
            writer.planned_darwin(self.head, self.tail)

    def test_planned_writes_protect_system_and_stop_at_partition_end(self):
        writer.blocks_ok(DATA_FIRST, BLOCKS - DATA_FIRST, self.target)
        writer.blocks_ok_read(0, DATA_FIRST, self.target)
        with self.assertRaisesRegex(writer.Refused, 'data write starts'):
            writer.blocks_ok(DATA_FIRST - 1, 2, self.target)
        with self.assertRaisesRegex(writer.Refused, 'not inside darwin'):
            writer.blocks_ok(BLOCKS - 1, 2, self.target)
        writer.blocks_ok(writer.PROTECTED, 1)
        with self.assertRaises(writer.Refused):
            writer.blocks_ok(writer.PROTECTED - 1, 1)

    def test_no_upload_for_plan_or_write_without_explicit_gate(self):
        with (patch.object(writer, 'identity', return_value='mock identity'),
              patch.object(writer, 'log'), patch.object(writer.subprocess, 'run') as command,
              patch.dict(os.environ, {'DARWIN_WRITE': ''})):
            writer.cmd_plan(str(self.image), DATA_FIRST, self.target)
            with self.assertRaisesRegex(writer.Refused, 'DARWIN_WRITE=yes is not set'):
                writer.cmd_plan(str(self.image), DATA_FIRST, self.target, write=True)
            command.assert_not_called()


    def test_write_readback_mismatch_is_refused_and_staging_is_cleaned_up(self):
        digest = hashlib.sha256(self.image.read_bytes()).hexdigest()
        with (patch.object(writer, 'identity', return_value='mock identity') as identity,
              patch.object(writer, 'log'), patch.object(writer.subprocess, 'run') as adb,
              patch.object(writer, 'phone', side_effect=[digest + '  staged', 'copied', '']) as shell,
              patch.object(writer, 'phone_sum', return_value='0' * 64) as readback,
              patch.dict(os.environ, {'DARWIN_WRITE': 'yes'})):
            adb.return_value = subprocess.CompletedProcess([], 0, '', '')
            with self.assertRaisesRegex(writer.Refused, 'read back'):
                writer.cmd_plan(str(self.image), DATA_FIRST, self.target, write=True)
            identity.assert_called_with(self.target)
            self.assertEqual(identity.call_count, 2)
            readback.assert_called_once_with(DATA_FIRST, 1, self.target)
            upload = adb.call_args.args[0]
            self.assertEqual(upload[:3], ['adb', 'push', str(self.image)])
            temporary = upload[3]
            self.assertEqual(shell.call_args_list[0].args[0].strip(), 'sha256sum ' + temporary)
            guarded = shell.call_args_list[1].args[0]
            self.assertTrue(guarded.startswith(writer.planned_guard(self.target)))
            self.assertIn(f'if={temporary} of={writer.DEV} bs={BS} seek={DATA_FIRST} count=1', guarded)
            self.assertEqual(shell.call_args_list[2].args[0].strip(), 'rm -f ' + temporary)


if __name__ == '__main__':
    unittest.main()
