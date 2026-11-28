#!/usr/bin/env python3
"""Exercise host flash-root packing without proprietary images or a device."""
import os
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest

from apfs.apfs import NX_MAGIC, fletcher64
from gpt import BS
from mkdatavol import DATA_FIRST
from mkflashroot import pack


def container(path, blocks, marker):
    header = bytearray(BS)
    struct.pack_into('<4sIQ', header, 32, NX_MAGIC, BS, blocks)
    header[100] = marker
    struct.pack_into('<Q', header, 0, fletcher64(header[8:]))
    with path.open('wb') as output:
        output.write(header)
        output.truncate(blocks * BS)


class FlashRootTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        base = Path(directory.name)
        self.inputs = tuple(base / role for role in ('root', 'os', 'app'))
        self.output = base / 'partition.img'
        for index, path in enumerate(self.inputs, 1):
            container(path, index, index)

    def test_copies_complete_containers_in_order(self):
        pack(self.inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b''.join(path.read_bytes() for path in self.inputs))

    def test_checksum_failure_preserves_output(self):
        self.output.write_bytes(b'previous image')
        with self.inputs[1].open('r+b') as source:
            source.seek(100)
            source.write(b'\x00')
        with self.assertRaisesRegex(ValueError, 'OS cryptex: .*checksum'):
            pack(self.inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b'previous image')

    def test_rejects_an_incomplete_container(self):
        with self.inputs[2].open('r+b') as source:
            source.truncate(BS)
        with self.assertRaisesRegex(ValueError, 'App cryptex: .*span'):
            pack(self.inputs, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_an_image_crossing_the_hfsx_boundary(self):
        container(self.inputs[0], DATA_FIRST, 1)
        with self.assertRaisesRegex(ValueError, 'cross the HFSX data boundary'):
            pack(self.inputs, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_an_output_hardlinked_to_an_input(self):
        os.link(self.inputs[0], self.output)
        original = self.inputs[0].read_bytes()
        with self.assertRaisesRegex(ValueError, 'output also names an input'):
            pack(self.inputs, self.output)
        self.assertEqual(self.output.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
