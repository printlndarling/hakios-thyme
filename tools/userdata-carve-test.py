#!/usr/bin/env python3
"""Exercise the host-only GPT planner with synthetic userdata snapshots."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib

from mkdatavol import MIN_DARWIN_BLOCKS, darwin_blocks_from_gpt

BS = 4096
DISK = 2_000_000
FIRST = 32768
LAST = DISK - 6
SEGMENT_BLOCKS = 512
MAIN_BLOCK = 4096
FS_BLOCKS = LAST - FIRST + 1
SEGMENTS = (FS_BLOCKS - MAIN_BLOCK) // SEGMENT_BLOCKS
CUT = 2**30 // BS
PLANNER = Path(__file__).with_name("userdata-carve.py")


def crc(data):
    return zlib.crc32(data) & 0xffffffff


def table_images(directory, overlapping=False, disk=DISK):
    last = disk - 6
    table = bytearray(4 * 128)
    table[:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
    table[16:32] = uuid.UUID(int=7).bytes_le
    struct.pack_into("<QQ", table, 32, FIRST, last)
    table[56:72] = "userdata".encode("utf-16le")
    if overlapping:
        table[128:144] = uuid.UUID(int=9).bytes_le
        table[144:160] = uuid.UUID(int=10).bytes_le
        struct.pack_into("<QQ", table, 160, FIRST - 10, FIRST + 10)
    disk_guid = uuid.UUID(int=8).bytes_le

    def header(my, alternate, table_lba):
        block = bytearray(BS)
        struct.pack_into("<8sIIIIQQQQ16sQIII", block, 0, b"EFI PART", 0x10000, 92, 0, 0,
                         my, alternate, 4, last, disk_guid, table_lba, 4, 128, crc(table))
        struct.pack_into("<I", block, 16, crc(block[:92]))
        return block

    head = bytearray(8 * BS)
    tail = bytearray(8 * BS)
    head[BS:2 * BS] = header(1, disk - 1, 2)
    head[2 * BS:2 * BS + len(table)] = table
    tail[6 * BS:6 * BS + len(table)] = table
    tail[7 * BS:] = header(disk - 1, 1, disk - 2)
    (directory / "gpt-head.bin").write_bytes(head)
    (directory / "gpt-tail.bin").write_bytes(tail)


def superblock(path, blocks, segments, magic=0xf2f52010):
    sb = bytearray(BS)
    struct.pack_into("<I", sb, 1024, magic)
    struct.pack_into("<II", sb, 1024 + 16, 12, 9)
    struct.pack_into("<Q", sb, 1024 + 36, blocks)
    struct.pack_into("<I", sb, 1024 + 68, segments)
    struct.pack_into("<I", sb, 1024 + 92, MAIN_BLOCK)
    path.write_bytes(sb)


class Carve(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.directory = Path(tmp)
        table_images(self.directory)
        superblock(self.directory / "userdata-meta-before.bin", FS_BLOCKS, SEGMENTS)
        entries = ["0|1"] + ["0|0"] * (SEGMENTS - 1)
        text = "".join(f"{i} {' '.join(entries[i:i + 10])}\n" for i in range(0, SEGMENTS, 10))
        (self.directory / "segment_info-before.txt").write_text(text, encoding="utf-8")

    def run_plan(self, size="1", *names):
        return subprocess.run([sys.executable, str(PLANNER), str(self.directory), size, *names],
                              capture_output=True, text=True, check=False)

    def assert_no_gpt(self):
        self.assertFalse(list(self.directory.glob("gpt-*.new")))

    def test_shrink_plan_does_not_emit_gpt_before_readback(self):
        result = self.run_plan()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("sb-after.bin", result.stdout)
        self.assertNotIn("of=/dev/block/sda", result.stdout)
        self.assert_no_gpt()

    def test_verified_shrink_emits_both_gpts(self):
        after = self.directory / "sb-after.bin"
        superblock(after, FS_BLOCKS - CUT, SEGMENTS - CUT // SEGMENT_BLOCKS)
        result = self.run_plan()
        self.assertEqual(result.returncode, 0, result.stderr)
        head = (self.directory / "gpt-head.new").read_bytes()
        tail = (self.directory / "gpt-tail.new").read_bytes()
        self.assertEqual(head[2*BS:2*BS+512], tail[6*BS:6*BS+512])
        self.assertEqual(struct.unpack_from("<Q", head, 2*BS + 40)[0], LAST - CUT)
        first, last = struct.unpack_from("<QQ", head, 2*BS + 128 + 32)
        self.assertEqual(first % SEGMENT_BLOCKS, 0)
        self.assertGreater(first, LAST - CUT)
        self.assertEqual(last, LAST)

    def test_corrupt_readback_refuses(self):
        superblock(self.directory / "sb-after.bin", FS_BLOCKS - CUT, SEGMENTS - CUT // SEGMENT_BLOCKS, magic=0)
        result = self.run_plan()
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_gpt()

    def test_overlapping_gpt_refuses_even_when_crcs_match(self):
        table_images(self.directory, overlapping=True)
        result = self.run_plan()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not check out", result.stderr)
        self.assert_no_gpt()

    def test_explicit_wipe_creates_only_local_outputs(self):
        result = subprocess.run([sys.executable, str(PLANNER), "--wipe", str(self.directory), "1"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list(self.directory.glob("gpt-*.new"))), 2)

    def test_invalid_size_and_name_refuse(self):
        for size, name in [("0", "darwin"), ("-1", "darwin"), ("100", "darwin"), ("1", "x"*37)]:
            with self.subTest(size=size, name=name):
                self.assertNotEqual(self.run_plan(size, name).returncode, 0)
                self.assert_no_gpt()


class DataVolumeGeometry(unittest.TestCase):
    def test_planned_geometry_and_mismatched_gpt_copies(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            table_images(directory, disk=13_672 * SEGMENT_BLOCKS)
            before_head = directory / "gpt-head.bin"
            before_tail = directory / "gpt-tail.bin"
            with self.assertRaisesRegex(ValueError, "exactly one.*named darwin"):
                darwin_blocks_from_gpt(before_head, before_tail)
            subprocess.run([sys.executable, str(PLANNER), "--wipe", str(directory), "20"],
                           capture_output=True, text=True, check=True)
            head = directory / "gpt-head.new"
            tail = directory / "gpt-tail.new"
            self.assertEqual(darwin_blocks_from_gpt(head, tail), MIN_DARWIN_BLOCKS + 512)
            with self.assertRaisesRegex(ValueError, "GPT copies must validate and agree"):
                darwin_blocks_from_gpt(head, before_tail)


if __name__ == "__main__":
    unittest.main()
