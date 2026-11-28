#!/usr/bin/env python3
"""Exercise the RAM AFC volume with the installed libimobiledevice library."""

import argparse
from contextlib import contextmanager
import ctypes as c
import hashlib
from pathlib import Path
import plistlib
import random
import re
import runpy
from itertools import takewhile
import socket
import subprocess
import sys
import tempfile
import time
import uuid

fixture = runpy.run_path(str(Path(__file__).with_name("test-lockdown-pairing.py")))
check, query = fixture["check"], fixture["query"]
APPROVAL, HDB = fixture["APPROVAL"], fixture["HDB"]
STRINGS = c.POINTER(c.c_char_p)
VOID = c.c_void_p
U64 = c.c_uint64
U32 = c.c_uint32


class Afc:
    def __init__(self, locator):
        self.lockdown = fixture["Lockdown"](locator)
        self.lib = self.lockdown.lib
        signatures = {
            "client_start_service": (VOID, c.POINTER(VOID), c.c_char_p),
            "client_free": (VOID,),
            "get_device_info": (VOID, c.POINTER(STRINGS)),
            "get_file_info": (VOID, c.c_char_p, c.POINTER(STRINGS)),
            "read_directory": (VOID, c.c_char_p, c.POINTER(STRINGS)),
            "dictionary_free": (STRINGS,),
            "make_directory": (VOID, c.c_char_p),
            "remove_path": (VOID, c.c_char_p),
            "file_open": (VOID, c.c_char_p, c.c_int, c.POINTER(U64)),
            "file_close": (VOID, U64),
            "file_read": (VOID, U64, VOID, U32, c.POINTER(U32)),
            "file_write": (VOID, U64, c.c_char_p, U32, c.POINTER(U32)),
            "file_seek": (VOID, U64, c.c_int64, c.c_int),
            "file_tell": (VOID, U64, c.POINTER(U64)),
        }
        for name, args in signatures.items():
            getattr(self.lib, "afc_" + name).argtypes = args

    @contextmanager
    def connection(self):
        device, client = VOID(), VOID()
        check("idevice_new", self.lib.idevice_new(c.byref(device), self.lockdown.locator), 0)
        try:
            check("afc_client_start_service", self.lib.afc_client_start_service(device, c.byref(client), b"thyme-ram-test"), 0)
            try:
                yield client
            finally:
                self.lib.afc_client_free(client)
        finally:
            self.lib.idevice_free(device)

    def strings(self, function, client, *args):
        result = STRINGS()
        check(function, getattr(self.lib, "afc_" + function)(client, *args, c.byref(result)), 0)
        try:
            return [entry.decode() for entry in takewhile(lambda entry: entry is not None, (result[i] for i in range(4096)))]
        finally:
            self.lib.afc_dictionary_free(result)

    @contextmanager
    def file(self, client, path, mode):
        handle = U64()
        check("afc_file_open", self.lib.afc_file_open(client, path, mode, c.byref(handle)), 0)
        try:
            yield handle.value
        finally:
            check("afc_file_close", self.lib.afc_file_close(client, handle), 0)

    def read(self, client, handle, limit=65536):
        buffer, length = c.create_string_buffer(limit), U32()
        check("afc_file_read", self.lib.afc_file_read(client, handle, buffer, limit, c.byref(length)), 0)
        return buffer.raw[:length.value]


def pairs(fields):
    return dict(zip(fields[::2], fields[1::2], strict=True))


def run(state, pair_file, large=False):
    ids = subprocess.check_output(["idevice_id", "-l"], text=True, timeout=15).split()
    if len(ids) != 1 or not re.fullmatch(r"thyme-usb-[a-f0-9]{20}", ids[0]):
        raise ValueError("expected one thyme development device")
    locator = ids[0]
    address = subprocess.check_output(
        ["ideviceinfo", "-u", locator, "-s", "-q", "com.hakios.debug", "-k", "SSHAddress"],
        text=True, timeout=15,
    ).strip()

    def hdb(*args):
        subprocess.run([sys.executable, str(HDB), "--state", str(state), "--host", address, *args],
                       check=True, timeout=15)

    public = fixture["serialization"].load_pem_public_key(plistlib.loads(query(locator, "DevicePublicKey", "-x")))
    pins, record, _, credentials = fixture["certificate_fixture"](public)
    pins["TLSClientCertificate"] = "RootCertificate"
    nonce = uuid.uuid4().hex
    folder = f"/afc-test-{nonce}".encode()
    path = folder + b"/sample.bin"
    on_device = "/private/var/tmp/hakios-afc" + folder.decode()
    payload = random.Random(870).randbytes(1024 * 1024 if large else 100 * 1024)
    staged = APPROVAL + "." + nonce
    afc = Afc(locator)
    policy_installed = False
    folder_created = False
    try:
        with tempfile.TemporaryDirectory(prefix="thyme-afc-policy-") as directory:
            approval = Path(directory) / "approval.plist"
            approval.write_bytes(plistlib.dumps(pins))
            hdb("push", str(approval), staged)
        hdb("shell", "/var/jb/usr/bin/chmod", "600", staged)
        hdb("shell", "/var/jb/bin/ln", staged, APPROVAL)
        policy_installed = True
        hdb("shell", "/var/jb/bin/rm", staged)
        with afc.lockdown.connection() as client:
            check("AFC test Pair", afc.lib.lockdownd_pair(client, record), 0)

        with fixture["stock_pair_record"](pair_file, locator, record, credentials[2][1]):
            with afc.connection() as client:
                report = pairs(afc.strings("get_device_info", client))
                check("AFC device model", report["Model"], "Xiaomi Mi 10S")
                check("create RAM directory", afc.lib.afc_make_directory(client, folder), 0)
                folder_created = True
                with afc.file(client, path, 3) as handle:
                    written = U32()
                    check(f"write one {len(payload) // 1024} KiB AFC packet",
                          afc.lib.afc_file_write(client, handle, payload, len(payload), c.byref(written)), 0)
                    check("AFC write length", written.value, len(payload))
                    previous_handle = handle
                info = pairs(afc.strings("get_file_info", client, path))
                check("RAM file reported by AFC", int(info["st_size"]), len(payload))
                check("AFC directory lists uploaded file", "sample.bin" in afc.strings("read_directory", client, folder), True)
                hdb("shell", "/var/jb/bin/ln", "-s", "/System/Library/CoreServices/SystemVersion.plist", on_device + "/link")
                for name, expected in (
                    (folder + b"/link", 10),
                    (folder + b"/../sample.bin", 7),
                    (b"/System/Library/CoreServices/SystemVersion.plist", 8),
                ):
                    handle = U64()
                    result = afc.lib.afc_file_open(client, name, 1, c.byref(handle))
                    check("AFC denied path outside the RAM export", result, expected)

            hdb("shell", "/var/jb/usr/sbin/sysctl", "kern.hakios.thyme_usb.reconnect=1")
            time.sleep(3)
            for _ in range(8):
                if subprocess.check_output(["idevice_id", "-l"], text=True, timeout=15).split() == ids:
                    break
                time.sleep(2)
            else:
                raise RuntimeError("thyme did not reappear after USB reattachment")

            with afc.connection() as client:
                stale_buffer, stale_size = c.create_string_buffer(1), U32()
                check("old AFC file handle rejected after reconnect",
                      afc.lib.afc_file_read(client, previous_handle, stale_buffer, 1, c.byref(stale_size)), 7)
                with afc.file(client, path, 1) as handle:
                    loaded = bytearray()
                    while len(loaded) < len(payload):
                        data = afc.read(client, handle)
                        if not data:
                            raise RuntimeError("early AFC EOF")
                        loaded.extend(data)
                    checksum = hashlib.sha256(payload).hexdigest()
                    check("AFC USB download SHA-256 after reattach", hashlib.sha256(loaded).hexdigest(), checksum)
                    check("AFC EOF", len(afc.read(client, handle)), 0)
                    check("AFC seek", afc.lib.afc_file_seek(client, handle, 12000, 0), 0)
                    offset = U64()
                    check("AFC tell", afc.lib.afc_file_tell(client, handle, c.byref(offset)), 0)
                    check("AFC offset", offset.value, 12000)
                    check("AFC read range", afc.read(client, handle, 256) == payload[12000:12256], True)
                check("AFC remove RAM file", afc.lib.afc_remove_path(client, path), 0)
                hdb("shell", "/var/jb/bin/rm", "-f", on_device + "/link")
                check("AFC remove empty RAM directory", afc.lib.afc_remove_path(client, folder), 0)
                folder_created = False
                hdb("shell", "/var/jb/bin/rm", APPROVAL)
                policy_installed = False
                values = STRINGS()
                status = afc.lib.afc_get_device_info(client, c.byref(values))
                check("revocation stopped the active AFC connection", status != 0, True)
    finally:
        if policy_installed:
            hdb("shell", "/var/jb/bin/rm", "-f", APPROVAL)
        hdb("shell", "/var/jb/bin/rm", "-f", staged)
        if folder_created:
            hdb("shell", "/var/jb/bin/rm", "-f", on_device + "/link", on_device + "/sample.bin")
            hdb("shell", "/var/jb/bin/rmdir", on_device)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hdb-state", type=Path, required=True)
    parser.add_argument("--stock-record", type=Path, required=True)
    parser.add_argument("--large", action="store_true", help="send one 1 MiB AFC write request")
    args = parser.parse_args()
    run(args.hdb_state, args.stock_record, args.large)
