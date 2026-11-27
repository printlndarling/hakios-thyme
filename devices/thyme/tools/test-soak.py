#!/usr/bin/env python3
"""Exercise soak evidence and device selection without a phone."""
import gzip
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TIMER = "--- loader_return_after expired ---"
LOG_POLICY = b"hakios-thyme-ram-only-logs-v1\0"
GUARD = "--- battguard: the charge is 25%, at or below battery_stop=25 three looks in a row: back to the loader ---"


def boot_image(loader, prefix=b""):
    def entry(name, data):
        return struct.pack("<16sII", name, len(data), 0) + data + bytes((-len(data)) % 16)

    body = entry(b"loader.efi", loader) + entry(b"usb", b"1\0") + bytes(24)
    shim_and_fd = bytearray(0x300000 + 208)
    shim_and_fd[56:60] = b"ARM\x64"
    shim_and_fd[64:64 + len(prefix)] = prefix
    payload = b"XNUP" + struct.pack("<I", len(body)) + body
    kernel = gzip.compress(shim_and_fd + payload + bytes((-len(payload)) % 16)) + b"device-tree"
    header = bytearray(4096)
    header[:8] = b"ANDROID!"
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<II", header, 36, 4096, 1)
    return header + kernel


class SoakTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        thyme = self.root / "devices" / "thyme"
        thyme.mkdir(parents=True)
        for name in ("soak.sh", "ram-boot.py", "ram.args"):
            shutil.copy2(ROOT / "devices" / "thyme" / name, thyme)
        (self.root / "image").write_bytes(b"mock-image")
        (self.root / "boot.img").write_bytes(boot_image(b"MZ" + LOG_POLICY))
        self.env = dict(
            os.environ, PATH=f"{self.root}:{os.environ['PATH']}", XL_ADB_SERIAL="",
            SYSTEM_IMAGE="image", SCENE="image", XL_BOOT_IMAGE="boot.img", XL_LOADER="image",
            SOAK_SECS="5", SOAK_STEP="1", TEST_LEVEL="75", TEST_LOG=TIMER,
            TEST_PRODUCT="thyme", TEST_FASTBOOT="", TEST_RUN_EXIT="0", TEST_PAYLOAD_EXIT="0",
        )
        self.program("adb", """
case "$*" in
    devices) printf 'List of devices attached\nTEST\tdevice\n' ;;
    '-s TEST shell getprop ro.product.device') printf '%s\r\n' "$TEST_PRODUCT" ;;
    '-s TEST shell dumpsys battery') printf '  level: %s\r\n  temperature: 310\r\n' "$TEST_LEVEL" ;;
    '-s TEST shell getprop sys.boot_completed') printf '1\r\n' ;;
    *) exit 1 ;;
esac
""")
        self.program("fastboot", """
[ "$1" = devices ] || exit 1
printf '%s' "$TEST_FASTBOOT"
""")
        self.program("xl", """
case "$1" in
    check-payload) exit "$TEST_PAYLOAD_EXIT" ;;
    ramdisk) touch uploaded ;;
    run)
        [ "$XL_REBOOT_AFTER:$XL_DETACH:$XL_ADB_SERIAL" = '1:0:TEST' ] || exit 1
        printf '%s\n' "$TEST_LOG" > logs/123456-run.log
        ln -s 123456-run.log logs/last.log
        printf 'result: returned 123 0\r  [logs/123456-run.log]\n'
        exit "$TEST_RUN_EXIT"
        ;;
    state) printf 'android\n' ;;
    *) exit 1 ;;
esac
""")

    def program(self, name, text):
        path = self.root / name
        path.write_text("#!/bin/sh\n" + text)
        path.chmod(0o755)

    def soak(self, **overrides):
        return subprocess.run(
            ["bash", "devices/thyme/soak.sh"], cwd=self.root, env=dict(self.env, **overrides),
            capture_output=True, text=True, timeout=10,
        )

    def test_clean_timer(self):
        result = self.soak()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Android boot completed", result.stdout)
        self.assertIn("timed return verified", result.stdout)
        [log] = (self.root / "logs").glob("soak.*/kernel.log")
        self.assertEqual(log.read_text(), TIMER + "\n")

    def test_early_or_unclean_exit(self):
        for log in ("", GUARD, TIMER + "\npanic(cpu 7 caller 0x123): test"):
            with self.subTest(log=log):
                (self.root / "logs/last.log").unlink(missing_ok=True)
                result = self.soak(TEST_LOG=log)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("does not show a clean timed return", result.stderr)

    def test_loader_policy_checked_before_usb(self):
        for contents in (boot_image(b"MZlegacy"), boot_image(b"MZlegacy", LOG_POLICY), b"ANDROID!"):
            with self.subTest(bytes=len(contents)):
                (self.root / "boot.img").write_bytes(contents)
                result = self.soak()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Rebuild xnu-usb.img", result.stderr)
                self.assertFalse((self.root / "uploaded").exists())

    def test_loader_failure(self):
        result = self.soak(TEST_RUN_EXIT="2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no clean loader return", result.stderr)

    def test_preflight_refuses_before_uploading(self):
        for overrides in (
            {"TEST_LEVEL": "39"}, {"TEST_PRODUCT": "umi"}, {"XL_ADB_SERIAL": "OTHER"},
            {"TEST_FASTBOOT": "OTHER fastboot"}, {"SOAK_STEP": "00"}, {"TEST_PAYLOAD_EXIT": "1"},
        ):
            with self.subTest(overrides=overrides):
                result = self.soak(**overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "uploaded").exists())


if __name__ == "__main__":
    unittest.main()
