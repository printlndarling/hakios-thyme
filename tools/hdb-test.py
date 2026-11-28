#!/usr/bin/env python3
"""Check HDB key enrollment and USB address discovery."""
import base64
from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import subprocess
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


class USBDiscovery(unittest.TestCase):
    def test_reads_current_private_address_from_ideviceinfo(self):
        info = subprocess.CompletedProcess([], 0, stdout='192.168.10.6\r\n', stderr='')
        with patch.object(hdb.subprocess, 'run', return_value=info) as run:
            self.assertEqual(hdb.usb_host(), '192.168.10.6')
        run.assert_called_once_with(hdb.USB_ADDRESS, capture_output=True, text=True, check=True, timeout=20)

    def test_rejects_absent_public_and_ambiguous_addresses(self):
        for stdout in ('', '203.0.113.5', '169.254.0.2', '192.168.10.6\n192.168.10.7', '-oProxyCommand=x'):
            with self.subTest(stdout=stdout):
                info = subprocess.CompletedProcess([], 0, stdout=stdout, stderr='')
                with patch.object(hdb.subprocess, 'run', return_value=info):
                    with self.assertRaisesRegex(ValueError, 'private Wi-Fi address'):
                        hdb.usb_host()

    def test_usb_selection_keeps_pinned_ssh_options(self):
        with TemporaryDirectory() as directory:
            state = Path(directory)
            (state / 'client_key').touch()
            (state / 'known_hosts').touch()
            argv = ['hdb.py', '--state', directory, '--usb', 'shell', '/usr/bin/id', '-u']
            with patch('sys.argv', argv), patch.object(hdb, 'usb_host', return_value='192.168.10.6'), \
                    patch.object(hdb.subprocess, 'call', return_value=0) as call:
                self.assertEqual(hdb.main(), 0)
            command = call.call_args.args[0]
            self.assertEqual(command[0], 'ssh')
            self.assertIn('StrictHostKeyChecking=yes', command)
            self.assertIn(f'HostKeyAlias={hdb.ALIAS}', command)
            self.assertEqual(command[-2:], ['root@192.168.10.6', '/usr/bin/id -u'])


if __name__ == '__main__':
    unittest.main()
