#!/usr/bin/env python3
"""Give a WSL client a private usbmuxd socket backed by the thyme WinUSB probe."""

import argparse
import base64
import os
from pathlib import Path
import plistlib
import re
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
PROBE = Path(__file__).with_name("test-usb-bulk.ps1").resolve()


class WinUSB:
    def __init__(self):
        self.path = subprocess.check_output(["wslpath", "-aw", str(PROBE)], text=True).strip()

    def probe(self, port=0, expected=None):
        quoted = self.path.replace("'", "''")
        command = (
            "$ErrorActionPreference='Stop'; "
            f"$p=[ScriptBlock]::Create((Get-Content -Raw -LiteralPath '{quoted}')); "
            f"& $p -BridgePort {port}"
        )
        if expected:
            command += f" -BridgeLocation {expected}"
        encoded = base64.b64encode(command.encode("utf-16le")).decode("ascii")
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
            capture_output=True, text=True, check=True, timeout=10,
        )
        value = result.stdout.strip()
        if not value:
            return None
        if not re.fullmatch(r"[a-f0-9]{64}", value):
            raise ValueError("unexpected WinUSB probe response")
        return value


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
                        reply = self.dispatch(request)
                body = plistlib.dumps(reply)
                self.wfile.write(HEADER.pack(HEADER.size + len(body), CLIENT_VERSION, PLIST_PACKET, tag) + body)
        except (OSError, TimeoutError):
            return

    def dispatch(self, request):
        if not isinstance(request, dict):
            return result(BAD_COMMAND)
        command = request.get("MessageType")
        if command == "ListDevices":
            location = self.probe()
            self.server.location = location
            return {"DeviceList": [attached(location)] if location else []}
        if command == "Connect":
            raw_port = request.get("PortNumber")
            identifier = request.get("DeviceID")
            if type(raw_port) is not int or not 1 <= raw_port <= 65535 or type(identifier) is not int:
                return result(BAD_COMMAND)
            expected = self.server.location
            if not expected or attached(expected)["DeviceID"] != identifier:
                return result(BAD_DEVICE)
            if self.probe(socket.ntohs(raw_port), expected) != expected:
                self.server.location = None
                return result(BAD_DEVICE)
            # Probe() verifies the physical device's matching RST/ACK.
            return result(CONNECTION_REFUSED)
        return result(BAD_COMMAND)

    def probe(self, port=0, expected=None):
        try:
            return self.server.transport.probe(port, expected)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print("WinUSB probe failed (" + type(error).__name__ + ")", file=sys.stderr)
            return None


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
