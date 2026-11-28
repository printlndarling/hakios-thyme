#!/usr/bin/env python3
"""Replay the local usbmuxd framing and check what reaches the WinUSB transport."""

from pathlib import Path
import plistlib
import runpy
import socket
import socketserver
import tempfile
import threading
import unittest

bridge = runpy.run_path(str(Path(__file__).with_name("usbmux-winusb.py")))
HEADER = bridge["HEADER"]
LOCATION = "15abcdef" * 8


class Transport:
    def __init__(self):
        self.present = True
        self.calls = []

    def probe(self, port=0, expected=None):
        self.calls.append((port, expected))
        return LOCATION if self.present else None


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = str(Path(self.directory.name) / "mux.sock")
        self.server = socketserver.UnixStreamServer(self.path, bridge["Client"])
        self.server.transport = Transport()
        self.server.location = None
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.worker.start()
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.worker.join)
        self.addCleanup(self.server.shutdown)

    def exchange(self, payload, version=1):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(self.path)
            client.sendall(HEADER.pack(len(payload) + HEADER.size, version, 8, 4123) + payload)
            with client.makefile("rb") as stream:
                length, version, message, tag = HEADER.unpack(stream.read(HEADER.size))
                self.assertEqual((version, message, tag), (1, 8, 4123))
                return plistlib.loads(stream.read(length - HEADER.size))

    def request(self, **fields):
        return self.exchange(plistlib.dumps(fields))

    def test_discovery_and_refused_port(self):
        entries = self.request(MessageType="ListDevices")["DeviceList"]
        self.assertEqual(len(entries), 1)
        properties = entries[0]["Properties"]
        self.assertEqual(properties["ProductID"], 1)
        self.assertTrue(properties["SerialNumber"].startswith("thyme-usb-"))
        identifier = entries[0]["DeviceID"]
        reply = self.request(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62078))
        self.assertEqual(reply["Number"], 3)
        self.assertEqual(self.server.transport.calls, [(0, None), (62078, LOCATION)])
        self.server.transport.present = False
        reply = self.request(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62078))
        self.assertEqual(reply["Number"], 2)
        self.assertEqual(self.request(MessageType="ListDevices"), {"DeviceList": []})

    def test_invalid_client_requests_stay_off_usb(self):
        self.assertEqual(self.exchange(b"<plist>")["Number"], 1)
        self.assertEqual(self.exchange(b"bad", version=0)["Number"], 6)
        self.assertEqual(self.request(MessageType="SavePairRecord")["Number"], 1)
        self.assertEqual(self.request(MessageType="Connect", DeviceID=1, PortNumber=65536)["Number"], 1)
        self.assertEqual(self.request(MessageType="Connect", DeviceID=1, PortNumber=62078)["Number"], 2)
        self.assertEqual(self.server.transport.calls, [])


if __name__ == "__main__":
    unittest.main()
