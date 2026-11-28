#!/usr/bin/env python3
"""Use OpenSSH for a development RAM boot of HakiOS on thyme."""
import argparse
import base64
import binascii
import ipaddress
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

PORT = 2222
ALIAS = 'hakios-thyme-hdb'
ROOT = Path(__file__).resolve().parent.parent
LAN = tuple(ipaddress.IPv4Network(cidr) for cidr in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def lan_ip(value):
    try:
        host = ipaddress.IPv4Address(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('an IPv4 address is required') from error
    if not any(host in net for net in LAN):
        raise argparse.ArgumentTypeError('the address must be a private LAN IPv4 address')
    return str(host)


def write_private(path, text):
    with tempfile.NamedTemporaryFile(dir=path.parent, mode='w', encoding='ascii', newline='\n', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(text)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def key(state, name):
    path = state / name
    if not path.exists():
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(path)], check=True)
    public = subprocess.check_output(['ssh-keygen', '-y', '-f', str(path)], text=True)
    return ' '.join(public.split()[:2])


def read_public_key(path):
    with path.open('rb') as source:
        data = source.read(4097)
    lines = data.splitlines()
    if len(data) > 4096 or len(lines) != 1:
        raise ValueError(f'{path}: expected one OpenSSH Ed25519 public key line')
    fields = lines[0].split(maxsplit=2)
    algorithm = b'ssh-ed25519'
    try:
        if len(fields) >= 2 and fields[0] == algorithm:
            blob = base64.b64decode(fields[1], validate=True)
            header = struct.pack('>I11sI', len(algorithm), algorithm, 32)
            if blob.startswith(header) and len(blob) == len(header) + 32:
                return f'{algorithm.decode()} {base64.b64encode(blob).decode()}'
    except binascii.Error:
        pass
    raise ValueError(f'{path}: expected an OpenSSH Ed25519 public key without options')


def prepare(state, peer, authorized_files=()):
    additional = [read_public_key(path) for path in authorized_files]
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    client = key(state, 'client_key')
    host = key(state, 'host_key')
    keys = dict.fromkeys([client, *additional])
    write_private(state / 'authorized_keys', ''.join(f'from="{peer}" {public}\n' for public in keys))
    write_private(state / 'known_hosts', f'{ALIAS} {host}\n')
    write_private(state / 'peer', f'{peer}\n')
    print(f'hdb: development credentials ready in {state}')


def client_flags(state, port_flag):
    state = state.resolve()
    for name in ('client_key', 'known_hosts'):
        if not (state / name).is_file():
            raise ValueError('run hdb prepare for this development image first')
    # Use only the key pinned at image preparation, even if an address is reused.
    return ['-F', 'none', port_flag, str(PORT), '-i', str(state / 'client_key'),
            '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'GlobalKnownHostsFile=none', '-o', f'UserKnownHostsFile="{(state / "known_hosts").as_posix()}"',
            '-o', f'HostKeyAlias={ALIAS}', '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=15',
            '-o', 'ServerAliveCountMax=2']


def remote_path(value):
    if not value.startswith('/') or any(ord(char) < 32 for char in value):
        raise argparse.ArgumentTypeError('use an absolute device path')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, default=ROOT / 'ssh/hdb')
    parser.add_argument('--host', type=lan_ip, help='the Wi-Fi address reported by the development scene')
    commands = parser.add_subparsers(dest='action', required=True)
    init = commands.add_parser('prepare', help='create the development keys and configure the allowed host')
    init.add_argument('peer', metavar='HOST_LAN_IP', type=lan_ip)
    init.add_argument('--authorize', metavar='ED25519_PUB', type=Path, action='append', default=[],
                      help='also authorize a public key generated on the same LAN peer')
    shell = commands.add_parser('shell', help='open a shell or execute arguments on thyme')
    shell.add_argument('command', nargs=argparse.REMAINDER)
    push = commands.add_parser('push', help='copy a file to thyme using SFTP')
    push.add_argument('source', type=Path)
    push.add_argument('destination', type=remote_path)
    pull = commands.add_parser('pull', help='copy a file from thyme using SFTP')
    pull.add_argument('source', type=remote_path)
    pull.add_argument('destination', type=Path)
    args = parser.parse_args()

    try:
        if args.action == 'prepare':
            prepare(args.state, args.peer, args.authorize)
            return 0
        if not args.host:
            parser.error('--host is required for shell, push and pull')
        device = 'root@' + args.host
        if args.action == 'shell':
            command = args.command[1:] if args.command[:1] == ['--'] else args.command
            target = [device, shlex.join(command)] if command else [device]
            return subprocess.call(['ssh', *client_flags(args.state, '-p'), *target])
        if args.action == 'push':
            files = [str(args.source.resolve()), f'{device}:{args.destination}']
        else:
            files = [f'{device}:{args.source}', str(args.destination.resolve())]
        return subprocess.call(['scp', *client_flags(args.state, '-P'), *files])
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f'hdb: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
