#!/usr/bin/env python3
"""Give a WSL client a private usbmuxd socket backed by the thyme WinUSB development profile."""

import argparse
import base64
from contextlib import contextmanager
import errno
import ipaddress
import os
from pathlib import Path
import plistlib
import queue
import re
import select
import socket
import socketserver
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import uuid
from xml.parsers.expat import ExpatError


HEADER = struct.Struct("<IIII")
CLIENT_VERSION, PLIST_PACKET = 1, 8
BAD_COMMAND, BAD_DEVICE, CONNECTION_REFUSED, BAD_VERSION = 1, 2, 3, 6
MAX_MESSAGE = 131072
PAIR_RECORD_MAX = 16384
LOCKDOWN_HEADER = struct.Struct(">I")
LOCKDOWN_MESSAGE_MAX = 32768
DIAGNOSTICS_PORT = 62080
AFC_PORT = 62081
TLS_HEADER = struct.Struct(">BBBH")
TLS_PAYLOAD_MAX = 18432
MAX_ENCODED_LINE = 4 * ((LOCKDOWN_MESSAGE_MAX + 2) // 3) + 2
PROBE = Path(__file__).with_name("test-usb-bulk.ps1").resolve()


class WinUSB:
    def __init__(self):
        self.path = str(PROBE) if sys.platform == "win32" else subprocess.check_output(
            ["wslpath", "-aw", str(PROBE)], text=True,
        ).strip()

    def argv(self, options):
        quoted = self.path.replace("'", "''")
        command = (
            "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
            f"$p=[ScriptBlock]::Create((Get-Content -Raw -LiteralPath '{quoted}')); "
            f"& $p {options}"
        )
        encoded = base64.b64encode(command.encode("utf-16le")).decode("ascii")
        return ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded]

    def probe(self):
        result = subprocess.run(
            self.argv("-BridgePort 0"),
            capture_output=True, text=True, check=True, timeout=10,
        )
        value = result.stdout.strip()
        if not value:
            return None
        if not re.fullmatch(r"[a-f0-9]{64}", value):
            raise ValueError("unexpected WinUSB probe response")
        return value

    @contextmanager
    def connect(self, port, location):
        if not re.fullmatch(r"[a-f0-9]{64}", location):
            raise ValueError("invalid WinUSB location")
        options = f"-BridgeStream -BridgePort {port} -BridgeLocation {location}"
        with subprocess.Popen(self.argv(options), stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
            session = WinUSBSession(process)
            session.tls = port in (DIAGNOSTICS_PORT, AFC_PORT)
            try:
                state = session.line(10)
                if state not in (b"connected", b"refused", b"missing"):
                    raise ValueError("unexpected WinUSB connection result")
                yield state, session
            finally:
                try:
                    process.stdin.close()
                    process.wait(timeout=4)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait()


class WinUSBSession:
    def __init__(self, process):
        self.process = process
        self.tls = False

    def line(self, timeout):
        if sys.platform == "win32":
            # Windows select() only accepts sockets. A timed-out read is
            # unblocked when connect() closes the child process.
            result = queue.SimpleQueue()
            def read():
                try:
                    result.put(self.process.stdout.readline(MAX_ENCODED_LINE))
                except OSError as error:
                    result.put(error)
            threading.Thread(target=read, daemon=True).start()
            try:
                line = result.get(timeout=timeout)
            except queue.Empty as error:
                raise TimeoutError("WinUSB session timed out") from error
            if isinstance(line, OSError):
                raise line
        else:
            if not select.select([self.process.stdout], [], [], timeout)[0]:
                raise TimeoutError("WinUSB session timed out")
            line = self.process.stdout.readline(MAX_ENCODED_LINE)
        if not line.endswith(b"\n"):
            raise ValueError("incomplete WinUSB response")
        return line.strip()

    def start_tls(self):
        self.process.stdin.write(b"TLS\n")
        self.process.stdin.flush()
        self.tls = True

    def exchange(self, request):
        self.process.stdin.write(base64.b64encode(request) + b"\n")
        self.process.stdin.flush()
        line = self.line(10)
        if self.tls and line == b"empty":
            return b""
        reply = base64.b64decode(line, validate=True)
        if self.tls:
            if not 0 < len(reply) <= LOCKDOWN_MESSAGE_MAX:
                raise ValueError("unexpected TLS response length")
            return reply
        if not LOCKDOWN_HEADER.size < len(reply) <= LOCKDOWN_MESSAGE_MAX:
            raise ValueError("unexpected Lockdown response length")
        if LOCKDOWN_HEADER.unpack_from(reply)[0] != len(reply) - LOCKDOWN_HEADER.size:
            raise ValueError("incomplete Lockdown response")
        return reply


def attached(location):
    # The plist API calls this field SerialNumber. It is a development
    # locator derived from the actual Windows interface path.
    identifier = (int(location[:8], 16) & 0x7fffffff) or 1
    props = {
        "DeviceID": identifier,
        "ProductID": 1,  # The private test PID checked by the WinUSB probe.
        "SerialNumber": "thyme-usb-" + location[:20],
        "ConnectionType": "USB",
    }
    return {"DeviceID": identifier, "MessageType": "Attached", "Properties": props}


def result(number):
    return {"MessageType": "Result", "Number": number}


def read_pair_record(path, locator):
    if path is None:
        return None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        with os.fdopen(fd, "rb") as source:
            attributes = os.fstat(source.fileno())
            if (not stat.S_ISREG(attributes.st_mode) or attributes.st_uid != os.geteuid() or
                    attributes.st_mode & 0o077 or not 0 < attributes.st_size <= PAIR_RECORD_MAX):
                return None
            data = source.read(PAIR_RECORD_MAX + 1)
        if not 0 < len(data) <= PAIR_RECORD_MAX:
            return None
        record = plistlib.loads(data)
        if not isinstance(record, dict) or record.get("PairRecordID") != locator:
            return None
        for name in ("HostID", "SystemBUID"):
            value = record.get(name)
            if not isinstance(value, str) or len(value) != 36 or uuid.UUID(value).int == 0:
                return None
        for name in ("RootCertificate", "RootPrivateKey", "HostCertificate", "DeviceCertificate"):
            value = record.get(name)
            if not isinstance(value, bytes) or not value:
                return None
        return data, record["SystemBUID"]
    except (OSError, ValueError, OverflowError, ExpatError):
        return None


# The socket adapter is only constructed inside WSL. Native Windows
# imports this program for direct WinUSB discovery.
class MuxServer(getattr(socketserver, "ThreadingUnixStreamServer", socketserver.BaseServer)):
    def __init__(self, address, transport, pair_record=None):
        self.transport = transport
        self.pair_record = pair_record
        self.location = None
        self.wire = threading.Lock()
        self.buid = str(uuid.uuid4()).upper()
        super().__init__(address, Client)


class Client(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(15)
        try:
            while True:
                header = self.rfile.read(HEADER.size)
                if len(header) != HEADER.size:
                    return
                length, version, message, tag = HEADER.unpack(header)
                if not HEADER.size < length <= MAX_MESSAGE:
                    return
                payload = self.rfile.read(length - HEADER.size)
                if len(payload) != length - HEADER.size:
                    return
                if version != CLIENT_VERSION:
                    reply = result(BAD_VERSION)
                elif message != PLIST_PACKET:
                    reply = result(BAD_COMMAND)
                else:
                    try:
                        request = plistlib.loads(payload)
                    except (ValueError, OverflowError, ExpatError):
                        reply = result(BAD_COMMAND)
                    else:
                        if isinstance(request, dict) and request.get("MessageType") == "Connect":
                            self.connect(request, tag)
                            return
                        reply = self.dispatch(request)
                self.reply(reply, tag)
        except (OSError, TimeoutError):
            return

    def reply(self, reply, tag):
        body = plistlib.dumps(reply)
        self.wfile.write(HEADER.pack(HEADER.size + len(body), CLIENT_VERSION, PLIST_PACKET, tag) + body)

    def dispatch(self, request):
        if not isinstance(request, dict):
            return result(BAD_COMMAND)
        command = request.get("MessageType")
        if command in ("ReadBUID", "ReadPairRecord"):
            location = self.server.location
            locator = attached(location)["Properties"]["SerialNumber"] if location else None
            if command == "ReadPairRecord" and (not locator or request.get("PairRecordID") != locator):
                return result(BAD_DEVICE)
            pair = read_pair_record(self.server.pair_record, locator) if locator else None
            if command == "ReadBUID":
                return {"BUID": pair[1] if pair else self.server.buid}
            return {"PairRecordData": pair[0]} if pair else result(errno.ENOENT)
        if command != "ListDevices":
            return result(BAD_COMMAND)
        if self.server.wire.acquire(blocking=False):
            try:
                self.server.location = self.server.transport.probe()
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                print("WinUSB probe failed (" + type(error).__name__ + ")", file=sys.stderr)
                self.server.location = None
            finally:
                self.server.wire.release()
        location = self.server.location
        return {"DeviceList": [attached(location)] if location else []}

    def connect(self, request, tag):
        if not self.server.wire.acquire(timeout=2):
            self.reply(result(CONNECTION_REFUSED), tag)
            return
        try:
            self.connect_locked(request, tag)
        finally:
            self.server.wire.release()

    def connect_locked(self, request, tag):
        raw_port = request.get("PortNumber")
        identifier = request.get("DeviceID")
        if type(raw_port) is not int or not 1 <= raw_port <= 65535 or type(identifier) is not int:
            self.reply(result(BAD_COMMAND), tag)
            return
        expected = self.server.location
        if not expected or attached(expected)["DeviceID"] != identifier:
            self.reply(result(BAD_DEVICE), tag)
            return
        connected = False
        port = socket.ntohs(raw_port)
        try:
            with self.server.transport.connect(port, expected) as (state, session):
                if state == b"missing":
                    self.server.location = None
                    self.reply(result(BAD_DEVICE), tag)
                    return
                if state == b"refused":
                    self.reply(result(CONNECTION_REFUSED), tag)
                    return
                connected = True
                self.reply(result(0), tag)
                if port in (DIAGNOSTICS_PORT, AFC_PORT):
                    self.relay_tls(session)
                else:
                    self.relay_lockdown(session)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print("WinUSB connection ended (" + type(error).__name__ + ")", file=sys.stderr)
            if not connected:
                self.server.location = None
                self.reply(result(BAD_DEVICE), tag)

    def relay_lockdown(self, session):
        while True:
            header = self.rfile.read(LOCKDOWN_HEADER.size)
            if len(header) != LOCKDOWN_HEADER.size:
                return
            size, = LOCKDOWN_HEADER.unpack(header)
            if not 0 < size <= LOCKDOWN_MESSAGE_MAX - len(header):
                return
            payload = self.rfile.read(size)
            if len(payload) != size:
                return
            reply = session.exchange(header + payload)
            # Only a successful StartSession reply changes the record framing.
            if b"StartSession" in payload:
                request = plistlib.loads(payload)
                response = plistlib.loads(reply[len(header):])
                if (isinstance(request, dict) and request.get("Request") == "StartSession" and
                        isinstance(response, dict) and response.get("Request") == "StartSession" and
                        response.get("EnableSessionSSL") is True and "Error" not in response):
                    session.start_tls()
                    self.wfile.write(reply)
                    self.relay_tls(session)
                    return
            self.wfile.write(reply)

    def relay_tls(self, session):
        while True:
            header = self.rfile.read(TLS_HEADER.size)
            if len(header) != TLS_HEADER.size:
                return
            kind, major, minor, size = TLS_HEADER.unpack(header)
            if not 20 <= kind <= 23 or major != 3 or minor > 3 or size > TLS_PAYLOAD_MAX:
                return
            payload = self.rfile.read(size)
            if len(payload) != size:
                return
            reply = session.exchange(header + payload)
            if reply:
                self.wfile.write(reply)


def ssh_address(transport):
    location = transport.probe()
    if location is None:
        raise ValueError("no thyme development WinUSB device found")
    body = plistlib.dumps({"Request": "GetValue", "Domain": "com.hakios.debug", "Key": "SSHAddress"})
    with transport.connect(62078, location) as (state, session):
        if state != b"connected":
            raise ValueError("the thyme development Lockdown service is unavailable")
        reply = session.exchange(LOCKDOWN_HEADER.pack(len(body)) + body)
    response = plistlib.loads(reply[LOCKDOWN_HEADER.size:])
    address = response.get("Value") if isinstance(response, dict) else None
    if not isinstance(address, str):
        raise ValueError("the development scene did not report a Wi-Fi address")
    return str(ipaddress.IPv4Address(address))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pair-record", type=Path, help="read a private local pair record for the attached device")
    parser.add_argument("--ssh-address", action="store_true", help="query the HDB Wi-Fi address directly over WinUSB")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="WSL client command to run with the private socket")
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if args.ssh_address:
        if command or args.pair_record:
            parser.error("--ssh-address does not take a client command or pair record")
        try:
            print(ssh_address(WinUSB()))
        except (OSError, ValueError, ExpatError, subprocess.SubprocessError) as error:
            parser.exit(1, f"WinUSB discovery failed: {error}\n")
        return 0
    if sys.platform == "win32":
        parser.error("use --ssh-address on native Windows or run the socket bridge within WSL")
    if not command:
        parser.error("supply a client command after --")

    transport = WinUSB()
    with tempfile.TemporaryDirectory(prefix="hakios-usbmux-") as directory:
        address = str(Path(directory) / "mux.sock")
        with MuxServer(address, transport, args.pair_record) as server:
            worker = threading.Thread(target=server.serve_forever)
            worker.start()
            try:
                env = dict(os.environ, USBMUXD_SOCKET_ADDRESS="UNIX:" + address)
                return subprocess.run(command, env=env).returncode
            finally:
                server.shutdown()
                worker.join()


if __name__ == "__main__":
    raise SystemExit(main())
