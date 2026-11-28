#!/usr/bin/env python3
"""Exercise RAM-only approval through libimobiledevice on a guarded thyme boot.

Run under usbmux-winusb.py with idevice_id and ideviceinfo on PATH. The
single USB device and enrolled HDB state must belong to the developer.
"""

import argparse
from contextlib import contextmanager
import ctypes as c
from datetime import datetime, timedelta, timezone
from pathlib import Path
import plistlib
import re
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
        self.lib.lockdownd_client_free.argtypes = [c.c_void_p]
        self.lib.lockdownd_pair.argtypes = [c.c_void_p, c.POINTER(PairRecord)]
        self.lib.lockdownd_validate_pair.argtypes = [c.c_void_p, c.POINTER(PairRecord)]
        self.lib.lockdownd_start_session.argtypes = [c.c_void_p, c.c_char_p, c.c_void_p, c.c_void_p]

    @contextmanager
    def connection(self):
        device, client = c.c_void_p(), c.c_void_p()
        check("idevice_new", self.lib.idevice_new(c.byref(device), self.locator), 0)
        try:
            check("lockdownd_client_new", self.lib.lockdownd_client_new(device, c.byref(client), b"thyme-pair-test"), 0)
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


def issue(subject, key, issuer, signing, ca=False):
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
            digital_signature=not ca, content_commitment=False, key_encipherment=not ca,
            data_encipherment=False, key_agreement=False, key_cert_sign=ca, crl_sign=ca,
            encipher_only=False, decipher_only=False), critical=True)
        .sign(signing, hashes.SHA256())
    )


def certificate_fixture(public_key):
    root_key, host_key, other_key = make_key(), make_key(), make_key()
    root = issue("thyme pairing test root", root_key.public_key(), None, root_key, ca=True)
    other = issue("unapproved test root", other_key.public_key(), None, other_key, ca=True)
    host = issue("thyme pairing test host", host_key.public_key(), root, root_key)
    device = issue("HakiOS thyme", public_key, root, root_key)
    wrong_key = issue("HakiOS thyme", host_key.public_key(), root, root_key)
    wrong_chain = issue("HakiOS thyme", public_key, other, other_key)
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
    )


def query(locator, key, *options):
    return subprocess.check_output(
        ["ideviceinfo", "-u", locator, "-s", *options, "-k", key], timeout=15,
    )


def run(state, reattach):
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
    pins, record, invalid = certificate_fixture(public)
    lockdown = Lockdown(locator)

    def hdb(*args):
        subprocess.run([sys.executable, str(HDB), "--state", str(state), "--host", address, *args],
                       check=True, timeout=15)

    with lockdown.connection() as client:
        check("pair without approval refused", lockdown.lib.lockdownd_pair(client, record), UNKNOWN_ERROR)
    tmp = APPROVAL + "." + uuid.uuid4().hex
    installed = False
    try:
        with tempfile.TemporaryDirectory(prefix="thyme-pair-test-") as directory:
            policy = Path(directory) / "approval.plist"
            policy.write_bytes(plistlib.dumps(pins))
            hdb("push", str(policy), tmp)
        hdb("shell", "/var/jb/usr/bin/chmod", "600", tmp)
        hdb("shell", "/var/jb/bin/ln", tmp, APPROVAL)
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
            check("StartSession remains unavailable",
                  lockdown.lib.lockdownd_start_session(client, record.host_id, None, None), UNKNOWN_ERROR)
        hdb("shell", "/var/jb/usr/bin/chmod", "644", APPROVAL)
        with lockdown.connection() as client:
            check("world-readable approval refused", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
        hdb("shell", "/var/jb/usr/bin/chmod", "600", APPROVAL)
        with lockdown.connection() as client:
            check("invalid policy cleared previous acceptance", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
            check("approved Pair accepted again", lockdown.lib.lockdownd_pair(client, record), 0)
        hdb("shell", "/var/jb/bin/rm", APPROVAL)
        installed = False
        with lockdown.connection() as client:
            check("validate after removal refused", lockdown.lib.lockdownd_validate_pair(client, record), INVALID_HOST_ID)
    finally:
        if installed:
            hdb("shell", "/var/jb/bin/rm", "-f", APPROVAL)
        hdb("shell", "/var/jb/bin/rm", "-f", tmp)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hdb-state", type=Path, required=True)
    parser.add_argument("--reattach", action="store_true", help="also exercise software USB reattachment before validation")
    args = parser.parse_args()
    run(args.hdb_state, args.reattach)
