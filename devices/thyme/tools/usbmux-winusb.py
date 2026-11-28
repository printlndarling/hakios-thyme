#!/usr/bin/env python3
"""Give a WSL client a private usbmuxd socket backed by the thyme WinUSB development profile."""

import argparse
import base64
from contextlib import contextmanager
import os
from pathlib import Path
import plistlib
import re
import select
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
from xml.parsers.expat import ExpatError


HEADER = struct.Struct("<IIII")
CLIENT_VERSION, PLIST_PACKET = 1, 8
BAD_COMMAND, BAD_DEVICE, CONNECTION_REFUSED, BAD_VERSION = 1, 2, 3, 6
MAX_MESSAGE = 131072
LOCKDOWN_HEADER = struct.Struct(">I")
LOCKDOWN_REQUEST_MAX = 1024
LOCKDOWN_REPLY_MAX = 480
PROBE = Path(__file__).with_name("test-usb-bulk.ps1").resolve()


class WinUSB:
    def __init__(self):
        self.path = subprocess.check_output(["wslpath", "-aw", str(PROBE)], text=True).strip()

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

    def line(self, timeout):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            raise TimeoutError("WinUSB session timed out")
        return self.process.stdout.readline(1100).strip()

    def exchange(self, request):
        self.process.stdin.write(base64.b64encode(request) + b"\n")
        self.process.stdin.flush()
        reply = base64.b64decode(self.line(5), validate=True)
        if not LOCKDOWN_HEADER.size < len(reply) <= LOCKDOWN_REPLY_MAX:
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


class Client(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(8)
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
        if not isinstance(request, dict) or request.get("MessageType") != "ListDevices":
            return result(BAD_COMMAND)
        try:
            location = self.server.transport.probe()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print("WinUSB probe failed (" + type(error).__name__ + ")", file=sys.stderr)
            location = None
        self.server.location = location
        return {"DeviceList": [attached(location)] if location else []}

    def connect(self, request, tag):
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
        try:
            with self.server.transport.connect(socket.ntohs(raw_port), expected) as (state, session):
                if state == b"missing":
                    self.server.location = None
                    self.reply(result(BAD_DEVICE), tag)
                    return
                if state == b"refused":
                    self.reply(result(CONNECTION_REFUSED), tag)
                    return
                connected = True
                self.reply(result(0), tag)
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
            if not 0 < size <= LOCKDOWN_REQUEST_MAX - len(header):
                return
            payload = self.rfile.read(size)
            if len(payload) != size:
                return
            self.wfile.write(session.exchange(header + payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="WSL client command to run with the private socket")
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("supply a client command after --")

    transport = WinUSB()
    with tempfile.TemporaryDirectory(prefix="hakios-usbmux-") as directory:
        address = str(Path(directory) / "mux.sock")
        with socketserver.UnixStreamServer(address, Client) as server:
            server.transport = transport
            server.location = None
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
