#!/usr/bin/env python3
"""Verify that native OpenSSH enrollment only consumes public key material."""
import base64
from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import hdb


def public_key(byte):
    name = b'ssh-ed25519'
    blob = struct.pack('>I11sI', len(name), name, 32) + bytes([byte]) * 32
    return name + b' ' + base64.b64encode(blob)


class Enrollment(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.state = Path(directory.name) / 'state'
        self.public = self.state.parent / 'windows.pub'

    def test_adds_native_key_without_options_or_duplicates(self):
        local_key, windows_key, device_key = (public_key(n) for n in range(3))
        self.public.write_bytes(windows_key + ' 開發用戶\r\n'.encode())
        with patch.object(hdb, 'key', side_effect=[local_key.decode(), device_key.decode()]):
            with redirect_stdout(io.StringIO()):
                hdb.prepare(self.state, '192.168.1.2', [self.public, self.public])
        self.assertEqual(
            (self.state / 'authorized_keys').read_bytes(),
            b'from="192.168.1.2" ' + local_key + b'\nfrom="192.168.1.2" ' + windows_key + b'\n',
        )
        self.assertEqual((self.state / 'known_hosts').read_bytes(), hdb.ALIAS.encode() + b' ' + device_key + b'\n')

    def test_malformed_key_does_not_modify_existing_credentials(self):
        self.state.mkdir()
        authorized = self.state / 'authorized_keys'
        authorized.write_bytes(b'current fixture')
        key = public_key(2)
        for data in (b'', b'command="id" ' + key, key + b'\n' + key,
                     b'ssh-ed25519 !!!', b'ssh-ed25519 ' + base64.b64encode(b'invalid wire data')):
            with self.subTest(data=data), patch.object(hdb, 'key') as make_key:
                self.public.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'OpenSSH Ed25519 public key'):
                    hdb.prepare(self.state, '192.168.1.2', [self.public])
                make_key.assert_not_called()
                self.assertEqual(authorized.read_bytes(), b'current fixture')


if __name__ == '__main__':
    unittest.main()
