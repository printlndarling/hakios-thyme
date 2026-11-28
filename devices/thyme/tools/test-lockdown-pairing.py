#!/usr/bin/env python3
"""Exercise RAM-only approval through libimobiledevice on a guarded thyme boot.

Run under usbmux-winusb.py with idevice_id and ideviceinfo on PATH. The
single USB device and enrolled HDB state must belong to the developer.
"""

import argparse
from contextlib import contextmanager
import ctypes as c
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import plistlib
import re
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import uuid

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


APPROVAL = "/private/var/tmp/hakios-lockdown-approval.plist"
HDB = Path(__file__).resolve().parents[3] / "tools/hdb.py"
INVALID_HOST_ID, INVALID_PAIR_RECORD, UNKNOWN_ERROR = -21, -31, -256
MUX_HEADER = struct.Struct("<IIII")
PLIST_LENGTH = struct.Struct(">I")


class PairRecord(c.Structure):
    _fields_ = [(name, c.c_char_p) for name in
                ("device_certificate", "host_certificate", "root_certificate", "host_id", "system_buid")]


class Lockdown:
    def __init__(self, locator):
        self.locator = locator.encode()
        self.lib = c.CDLL("libimobiledevice-1.0.so.6")
        self.lib.idevice_new.argtypes = [c.POINTER(c.c_void_p), c.c_char_p]
        self.lib.idevice_free.argtypes = [c.c_void_p]
        self.lib.lockdownd_client_new.argtypes = [c.c_void_p, c.POINTER(c.c_void_p), c.c_char_p]
        self.lib.lockdownd_client_new_with_handshake.argtypes = self.lib.lockdownd_client_new.argtypes
        self.lib.lockdownd_client_free.argtypes = [c.c_void_p]
        self.lib.lockdownd_get_value.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, c.POINTER(c.c_void_p)]
        self.lib.lockdownd_pair.argtypes = [c.c_void_p, c.POINTER(PairRecord)]
        self.lib.lockdownd_validate_pair.argtypes = [c.c_void_p, c.POINTER(PairRecord)]

    @contextmanager
    def connection(self, handshake=False):
        device, client = c.c_void_p(), c.c_void_p()
        check("idevice_new", self.lib.idevice_new(c.byref(device), self.locator), 0)
        try:
            connect = self.lib.lockdownd_client_new_with_handshake if handshake else self.lib.lockdownd_client_new
            check(connect.__name__, connect(device, c.byref(client), b"thyme-pair-test"), 0)
            try:
                yield client
            finally:
                self.lib.lockdownd_client_free(client)
        finally:
            self.lib.idevice_free(device)


def check(label, actual, expected):
    if actual != expected:
        raise RuntimeError(f"{label} expected {expected}, got {actual}")
    print(f"{label} {actual}", flush=True)


def make_key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def issue(subject, key, issuer, signing, ca=False, tls_client=False):
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)])
    now = datetime.now(timezone.utc)
    return (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(issuer.subject if issuer else name)
        .public_key(key)
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(hours=1))
        .not_valid_after(now + timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), critical=True)
        .add_extension(x509.KeyUsage(
            digital_signature=not ca or tls_client, content_commitment=False, key_encipherment=not ca,
            data_encipherment=False, key_agreement=False, key_cert_sign=ca, crl_sign=ca,
            encipher_only=False, decipher_only=False), critical=True)
        .sign(signing, hashes.SHA256())
    )


def certificate_fixture(public_key):
    root_key, host_key, other_key = make_key(), make_key(), make_key()
    root = issue("thyme pairing test root", root_key.public_key(), None, root_key, ca=True, tls_client=True)
    other = issue("unapproved test root", other_key.public_key(), None, other_key, ca=True)
    host = issue("thyme pairing test host", host_key.public_key(), root, root_key)
    device = issue("thyme.hakios.test", public_key, root, root_key)
    unpinned_host = issue("unpinned host", other_key.public_key(), root, root_key)
    wrong_key = issue("thyme.hakios.test", host_key.public_key(), root, root_key)
    wrong_chain = issue("thyme.hakios.test", public_key, other, other_key)
    host_id, buid = str(uuid.uuid4()).upper(), str(uuid.uuid4()).upper()
    pins = {
        "HostID": host_id,
        "SystemBUID": buid,
        "RootCertificateSHA256": root.fingerprint(hashes.SHA256()),
        "HostCertificateSHA256": host.fingerprint(hashes.SHA256()),
    }
    pem = serialization.Encoding.PEM

    def record(device_cert, host_cert=host, identifier=host_id):
        return PairRecord(device_cert.public_bytes(pem), host_cert.public_bytes(pem),
                          root.public_bytes(pem), identifier.encode(), buid.encode())

    return pins, record(device), (
        ("different HostID refused", record(device, identifier=str(uuid.uuid4())), INVALID_HOST_ID),
        ("different host certificate refused", record(device, host_cert=wrong_chain), INVALID_PAIR_RECORD),
        ("different device key refused", record(wrong_key), INVALID_PAIR_RECORD),
        ("wrong certificate issuer refused", record(wrong_chain), INVALID_PAIR_RECORD),
    ), ((host.public_bytes(pem), host_key), (unpinned_host.public_bytes(pem), other_key),
        (root.public_bytes(pem), root_key))


def receive(stream, size):
    received = bytearray()
    while len(received) < size:
        segment = stream.recv(size - len(received))
        if not segment:
            raise EOFError("incomplete USB response")
        received.extend(segment)
    return bytes(received)


def plist_request(stream, request):
    body = plistlib.dumps(request)
    stream.sendall(PLIST_LENGTH.pack(len(body)) + body)
    length, = PLIST_LENGTH.unpack(receive(stream, PLIST_LENGTH.size))
    if not 0 < length <= 32764:
        raise ValueError("invalid Lockdown response length")
    return plistlib.loads(receive(stream, length))


def mux_request(address, request, switch=False):
    @contextmanager
    def connection():
        with socket.socket(socket.AF_UNIX) as stream:
            stream.settimeout(12)
            stream.connect(address)
            body = plistlib.dumps(request)
            stream.sendall(MUX_HEADER.pack(MUX_HEADER.size + len(body), 1, 8, 1) + body)
            length, version, message, tag = MUX_HEADER.unpack(receive(stream, MUX_HEADER.size))
            if (version, message, tag) != (1, 8, 1) or not MUX_HEADER.size < length <= 131072:
                raise ValueError("invalid USB mux response")
            yield plistlib.loads(receive(stream, length - MUX_HEADER.size)), stream

    if switch:
        return connection()
    with connection() as (reply, _):
        return reply


def session_test(locator, pins, record, credentials, hdb):
    address = os.environ["USBMUXD_SOCKET_ADDRESS"].removeprefix("UNIX:")
    entries = mux_request(address, {"MessageType": "ListDevices"})["DeviceList"]
    device = next(item for item in entries if item["Properties"]["SerialNumber"] == locator)
    start = {"Request": "StartSession", "HostID": pins["HostID"], "SystemBUID": pins["SystemBUID"]}
    connect = {"MessageType": "Connect", "DeviceID": device["DeviceID"], "PortNumber": socket.htons(62078)}
    device_der = x509.load_pem_x509_certificate(record.device_certificate).public_bytes(serialization.Encoding.DER)

    def context(credential):
        ctx = ssl.create_default_context(cadata=record.root_certificate.decode())
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        if credential:
            certificate, key = credential
            # The temporary credential files stay on WSL tmpfs.
            with tempfile.TemporaryDirectory(dir="/dev/shm", prefix="thyme-tls-") as directory:
                bundle = Path(directory) / "client.pem"
                fd = os.open(bundle, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                with os.fdopen(fd, "wb") as handle:
                    handle.write(certificate)
                    handle.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                   serialization.NoEncryption()))
                ctx.load_cert_chain(bundle)
        return ctx

    with mux_request(address, connect, switch=True) as (reply, stream):
        check("Lockdown socket connected", reply["Number"], 0)
        for field in ("HostID", "SystemBUID"):
            invalid = dict(start, **{field: str(uuid.uuid4())})
            check(f"StartSession with different {field} refused", plist_request(stream, invalid).get("Error"), "InvalidHostID")
    valid, unpinned, root = credentials
    session_ids = set()
    cases = ((None, "missing certificate"), (unpinned, "unpinned leaf"),
             (root, "pairing root with default policy"), (valid, "approved leaf"))
    for credential, label in cases:
        with mux_request(address, connect, switch=True) as (reply, stream):
            check("Lockdown socket connected", reply["Number"], 0)
            response = plist_request(stream, start)
            check("StartSession requests TLS", response.get("EnableSessionSSL"), True)
            session_id = uuid.UUID(response["SessionID"])
            check("fresh session ID", session_id in session_ids, False)
            session_ids.add(session_id)
            try:
                tls = context(credential).wrap_socket(stream, server_hostname="thyme.hakios.test")
            except ssl.SSLError as error:
                if credential is valid or isinstance(error, ssl.SSLCertVerificationError):
                    raise
                print(f"TLS refused {label}", flush=True)
                continue
            with tls:
                if credential is not valid:
                    raise RuntimeError(f"TLS accepted {label}")
                check("approved client received TLS", tls.version(), "TLSv1.2")
                check("peer is paired device certificate", tls.getpeercert(binary_form=True) == device_der, True)
                query = {"Request": "GetValue", "Key": "HardwareModel"}
                body = plistlib.dumps(query)
                tls.sendall(PLIST_LENGTH.pack(len(body))[:2])
                tls.sendall(PLIST_LENGTH.pack(len(body))[2:] + body)
                length, = PLIST_LENGTH.unpack(receive(tls, PLIST_LENGTH.size))
                check("HardwareModel through fragmented TLS request", plistlib.loads(receive(tls, length)).get("Value"), "thyme")
                check("unknown StopSession refused", plist_request(
                    tls, {"Request": "StopSession", "SessionID": str(uuid.uuid4())}).get("Error"), "InvalidSessionID")
                response = plist_request(tls, {"Request": "StopSession", "SessionID": str(session_id).upper()})
                check("StopSession acknowledged", response, {"Request": "StopSession"})
                check("StopSession closes TLS", tls.recv(1), b"")
    with mux_request(address, connect, switch=True) as (reply, stream):
        check("Lockdown socket reconnected", reply["Number"], 0)
        start_reply = plist_request(stream, start)
        check("StartSession after reconnect requests TLS", start_reply.get("EnableSessionSSL"), True)
        with context(valid).wrap_socket(stream, server_hostname="thyme.hakios.test") as tls:
            hdb("shell", "/var/jb/bin/rm", APPROVAL)
            tls.sendall(PLIST_LENGTH.pack(len(body)) + body)
            check("active TLS approval removed", tls.recv(1), b"")
    with mux_request(address, connect, switch=True) as (reply, stream):
        check("Lockdown socket after revocation connected", reply["Number"], 0)
        check("StartSession after revocation refused", plist_request(stream, start).get("Error"), "InvalidHostID")


def query(locator, key, *options):
    return subprocess.check_output(
        ["ideviceinfo", "-u", locator, "-s", *options, "-k", key], timeout=15,
    )


@contextmanager
def stock_pair_record(path, locator, record, root_key):
    if path is None:
        yield
        return
    if Path("/dev/shm") not in path.resolve().parents:
        raise ValueError("put the temporary stock client pair record under /dev/shm")
    data = {
        "PairRecordID": locator, "HostID": record.host_id.decode(), "SystemBUID": record.system_buid.decode(),
        "DeviceCertificate": record.device_certificate, "RootCertificate": record.root_certificate,
        "HostCertificate": record.host_certificate,
        "RootPrivateKey": root_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                                 serialization.NoEncryption()),
    }
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            plistlib.dump(data, stream, fmt=plistlib.FMT_BINARY)
        yield
    finally:
        path.unlink(missing_ok=True)


def check_stock_client(locator, accepts):
    reply = subprocess.run(["ideviceinfo", "-u", locator, "-k", "HardwareModel"], capture_output=True, timeout=18)
    if accepts and (reply.returncode != 0 or reply.stdout.strip() != b"thyme"):
        raise RuntimeError(f"stock ideviceinfo could not open its TLS session (status {reply.returncode})")
    if not accepts and reply.stdout.strip() == b"thyme":
        raise RuntimeError("stock ideviceinfo read HardwareModel without root authorization")
    print("Stock ideviceinfo over TLS read HardwareModel=thyme" if accepts else
          "Stock ideviceinfo did not authenticate with the pairing root", flush=True)


def run(state, reattach, stock_record=None):
    ids = subprocess.check_output(["idevice_id", "-l"], text=True, timeout=15).split()
    if len(ids) != 1 or not re.fullmatch(r"thyme-usb-[a-f0-9]{20}", ids[0]):
        raise ValueError("expected one thyme development device")
    locator = ids[0]
    if query(locator, "HardwareModel").strip() != b"thyme":
        raise ValueError("device did not identify its board as thyme")
    address = subprocess.check_output(
        ["ideviceinfo", "-u", locator, "-s", "-q", "com.hakios.debug", "-k", "SSHAddress"],
        text=True, timeout=15,
    ).strip()
    public = serialization.load_pem_public_key(plistlib.loads(query(locator, "DevicePublicKey", "-x")))
    pins, record, invalid, credentials = certificate_fixture(public)
    lockdown = Lockdown(locator)

    def hdb(*args):
        subprocess.run([sys.executable, str(HDB), "--state", str(state), "--host", address, *args],
                       check=True, timeout=15)

    tmp = APPROVAL + "." + uuid.uuid4().hex

    def stage_policy(policy):
        with tempfile.TemporaryDirectory(prefix="thyme-pair-test-") as directory:
            source = Path(directory) / "approval.plist"
            source.write_bytes(plistlib.dumps(policy))
            hdb("push", str(source), tmp)
        hdb("shell", "/var/jb/usr/bin/chmod", "600", tmp)

    def install(policy):
        stage_policy(policy)
        hdb("shell", "/var/jb/bin/ln", tmp, APPROVAL)

    with lockdown.connection() as client:
        check("pair without approval refused", lockdown.lib.lockdownd_pair(client, record), UNKNOWN_ERROR)
    installed = False
    try:
        install(pins)
        installed = True
        hdb("shell", "/var/jb/bin/rm", tmp)
        with lockdown.connection() as client:
            check("validate before Pair refused", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
            for label, candidate, code in invalid:
                check(label, lockdown.lib.lockdownd_pair(client, candidate), code)
            check("approved Pair accepted", lockdown.lib.lockdownd_pair(client, record), 0)
        if reattach:
            hdb("shell", "/var/jb/usr/sbin/sysctl", "kern.hakios.thyme_usb.reconnect=1")
            time.sleep(3)
            for _ in range(6):
                seen = subprocess.check_output(["idevice_id", "-l"], text=True, timeout=15).split()
                if seen == ids:
                    break
                time.sleep(2)
            else:
                raise RuntimeError("thyme did not reappear after USB reattachment")
        with lockdown.connection() as client:
            check("approved ValidatePair on reopened connection", lockdown.lib.lockdownd_validate_pair(client, record), 0)
        hdb("shell", "/var/jb/usr/bin/chmod", "644", APPROVAL)
        with lockdown.connection() as client:
            check("world-readable approval refused", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
        hdb("shell", "/var/jb/usr/bin/chmod", "600", APPROVAL)
        with lockdown.connection() as client:
            check("invalid policy cleared previous acceptance", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
            check("approved Pair accepted again", lockdown.lib.lockdownd_pair(client, record), 0)
        with stock_pair_record(stock_record, locator, record, credentials[2][1]):
            if stock_record:
                check_stock_client(locator, accepts=False)
            session_test(locator, pins, record, credentials, hdb)
            installed = False
            with lockdown.connection() as client:
                check("validate after removal refused", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
            if stock_record:
                check_stock_client(locator, accepts=False)
                install(dict(pins, TLSClientCertificate="RootCertificate"))
                installed = True
                hdb("shell", "/var/jb/bin/rm", tmp)
                with lockdown.connection() as client:
                    check("Pair under explicit pairing-root policy", lockdown.lib.lockdownd_pair(client, record), 0)
                check_stock_client(locator, accepts=True)
                check_stock_client(locator, accepts=True)
                with lockdown.connection(handshake=True) as client:
                    stage_policy(pins)
                    hdb("shell", "/var/jb/usr/bin/mv", "-f", tmp, APPROVAL)
                    value = c.c_void_p()
                    status = lockdown.lib.lockdownd_get_value(client, None, b"HardwareModel", c.byref(value))
                    check("changing the certificate role closed the active stock TLS session", status != 0, True)
                hdb("shell", "/var/jb/bin/rm", APPROVAL)
                installed = False
                check_stock_client(locator, accepts=False)
    finally:
        if installed:
            hdb("shell", "/var/jb/bin/rm", "-f", APPROVAL)
        hdb("shell", "/var/jb/bin/rm", "-f", tmp)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hdb-state", type=Path, required=True)
    parser.add_argument("--reattach", action="store_true", help="also exercise software USB reattachment before validation")
    parser.add_argument("--stock-record", type=Path, help="temporary pair record path passed to the bridge under /dev/shm")
    args = parser.parse_args()
    run(args.hdb_state, args.reattach, args.stock_record)
