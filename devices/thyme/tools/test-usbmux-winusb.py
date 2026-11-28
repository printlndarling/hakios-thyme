#!/usr/bin/env python3
"""Replay the local usbmuxd framing and check what reaches the WinUSB transport."""

from contextlib import contextmanager
import errno
from pathlib import Path
import plistlib
import runpy
import socket
import tempfile
import threading
import unittest
import uuid

bridge = runpy.run_path(str(Path(__file__).with_name("usbmux-winusb.py")))
HEADER = bridge["HEADER"]
LOCATION = "15abcdef" * 8
LOCKDOWN_HEADER = bridge["LOCKDOWN_HEADER"]


class Transport:
    def __init__(self):
        self.present = True
        self.calls = []

    def probe(self):
        self.calls.append(("list",))
        return LOCATION if self.present else None

    @contextmanager
    def connect(self, port, location):
        self.calls.append((port, location))
        yield (b"connected" if port == 62078 else b"refused") if self.present else b"missing", self

    def exchange(self, request):
        self.calls.append(request)
        return LOCKDOWN_HEADER.pack(5) + b"reply"


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = str(Path(self.directory.name) / "mux.sock")
        self.server = bridge["MuxServer"](self.path, Transport())
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.worker.start()
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.worker.join)
        self.addCleanup(self.server.shutdown)

    @contextmanager
    def exchange(self, payload, version=1):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(self.path)
            client.sendall(HEADER.pack(len(payload) + HEADER.size, version, 8, 4123) + payload)
            with client.makefile("rb") as stream:
                length, version, message, tag = HEADER.unpack(stream.read(HEADER.size))
                self.assertEqual((version, message, tag), (1, 8, 4123))
                yield plistlib.loads(stream.read(length - HEADER.size)), client, stream

    def request(self, **fields):
        return self.request_payload(plistlib.dumps(fields))

    def request_payload(self, payload, version=1):
        with self.exchange(payload, version) as (reply, _, _):
            return reply

    def test_discovery_and_refused_port(self):
        entries = self.request(MessageType="ListDevices")["DeviceList"]
        self.assertEqual(len(entries), 1)
        properties = entries[0]["Properties"]
        self.assertEqual(properties["ProductID"], 1)
        self.assertTrue(properties["SerialNumber"].startswith("thyme-usb-"))
        identifier = entries[0]["DeviceID"]
        reply = self.request(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62079))
        self.assertEqual(reply["Number"], 3)
        self.assertEqual(self.server.transport.calls, [("list",), (62079, LOCATION)])
        self.server.transport.present = False
        reply = self.request(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62079))
        self.assertEqual(reply["Number"], 2)
        self.assertEqual(self.request(MessageType="ListDevices"), {"DeviceList": []})

    def test_connected_stream_forwards_binary_frame(self):
        identifier = self.request(MessageType="ListDevices")["DeviceList"][0]["DeviceID"]
        connect = plistlib.dumps(dict(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62078)))
        message = LOCKDOWN_HEADER.pack(3) + b"one"
        with self.exchange(connect) as (reply, client, stream):
            self.assertEqual(reply["Number"], 0)
            client.sendall(message[:2])
            client.sendall(message[2:])
            self.assertEqual(stream.read(9), LOCKDOWN_HEADER.pack(5) + b"reply")
            client.sendall(LOCKDOWN_HEADER.pack(bridge["LOCKDOWN_MESSAGE_MAX"]))
            self.assertEqual(stream.read(), b"")
        self.assertEqual(self.server.transport.calls, [("list",), (62078, LOCATION), message])

    def test_tls_upgrade_relays_records_and_skips_empty_output(self):
        identifier = self.request(MessageType="ListDevices")["DeviceList"][0]["DeviceID"]
        connect = plistlib.dumps(dict(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62078)))
        request = plistlib.dumps({"Request": "StartSession"})
        response = plistlib.dumps({"Request": "StartSession", "EnableSessionSSL": True}, fmt=plistlib.FMT_BINARY)
        first, second = b"\x16\x03\x01\x00\x01x", b"\x14\x03\x03\x00\x01y"
        answer = b"\x16\x03\x03\x00\x01z"
        answers = iter([LOCKDOWN_HEADER.pack(len(response)) + response, b"", answer])
        seen = []

        def exchange(frame):
            seen.append(frame)
            return next(answers)

        self.server.transport.exchange = exchange
        self.server.transport.start_tls = lambda: seen.append("tls")
        with self.exchange(connect) as (reply, client, stream):
            self.assertEqual(reply["Number"], 0)
            client.sendall(LOCKDOWN_HEADER.pack(len(request)) + request)
            self.assertEqual(stream.read(len(response) + LOCKDOWN_HEADER.size),
                             LOCKDOWN_HEADER.pack(len(response)) + response)
            client.sendall(first[:3])
            client.sendall(first[3:] + second)
            self.assertEqual(stream.read(len(answer)), answer)
            client.sendall(b"\x17\x03\x03\x48\x01")
            self.assertEqual(stream.read(), b"")
        self.assertEqual(seen, [LOCKDOWN_HEADER.pack(len(request)) + request, "tls", first, second])

    def test_pairing_metadata_is_available_during_a_lockdown_connection(self):
        entry = self.request(MessageType="ListDevices")["DeviceList"][0]
        identifier = entry["DeviceID"]
        locator = entry["Properties"]["SerialNumber"]
        connect = plistlib.dumps(dict(MessageType="Connect", DeviceID=identifier, PortNumber=socket.htons(62078)))
        body = b"certificate" * 1000
        message = LOCKDOWN_HEADER.pack(len(body)) + body
        with self.exchange(connect) as (reply, client, stream):
            self.assertEqual(reply["Number"], 0)
            buid = self.request(MessageType="ReadBUID")["BUID"]
            self.assertEqual(uuid.UUID(buid).version, 4)
            self.assertEqual(self.request(MessageType="ReadBUID")["BUID"], buid)
            self.assertEqual(self.request(MessageType="ReadPairRecord", PairRecordID=locator)["Number"], errno.ENOENT)
            self.assertEqual(self.request(MessageType="ReadPairRecord", PairRecordID="elsewhere")["Number"], 2)
            self.assertEqual(self.request(MessageType="ListDevices"), {"DeviceList": [entry]})
            client.sendall(message)
            self.assertEqual(stream.read(9), LOCKDOWN_HEADER.pack(5) + b"reply")
        self.assertEqual(self.server.transport.calls, [("list",), (62078, LOCATION), message])

    def test_invalid_client_requests_stay_off_usb(self):
        self.assertEqual(self.request_payload(b"<plist>")["Number"], 1)
        self.assertEqual(self.request_payload(b"bad", version=0)["Number"], 6)
        self.assertEqual(self.request(MessageType="SavePairRecord")["Number"], 1)
        self.assertEqual(self.request(MessageType="Connect", DeviceID=1, PortNumber=65536)["Number"], 1)
        self.assertEqual(self.request(MessageType="Connect", DeviceID=1, PortNumber=62078)["Number"], 2)
        self.assertEqual(self.server.transport.calls, [])


class DirectAddressTest(unittest.TestCase):
    def test_reads_the_debug_address_over_the_same_lockdown_port(self):
        transport = Transport()

        def exchange(frame):
            self.assertEqual(plistlib.loads(frame[LOCKDOWN_HEADER.size:]),
                             {"Request": "GetValue", "Domain": "com.hakios.debug", "Key": "SSHAddress"})
            reply = plistlib.dumps({"Value": "192.168.10.6"})
            return LOCKDOWN_HEADER.pack(len(reply)) + reply

        transport.exchange = exchange
        self.assertEqual(bridge["ssh_address"](transport), "192.168.10.6")
        self.assertEqual(transport.calls, [("list",), (62078, LOCATION)])

    def test_device_absence_refuses_a_connection(self):
        transport = Transport()
        transport.present = False
        with self.assertRaisesRegex(ValueError, "no thyme development WinUSB device"):
            bridge["ssh_address"](transport)
        self.assertEqual(transport.calls, [("list",)])


if __name__ == "__main__":
    unittest.main()
