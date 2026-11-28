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
            SOAK_SECS="5", SOAK_STEP="1", SOAK_METAL_APPS="0", SOAK_UIM_READONLY="0", TEST_UIM_READONLY="0", TEST_METAL="0",
            SOAK_USB_HANDOFF="0", TEST_USB_HANDOFF="0", SOAK_DSDS_PROBE="0", TEST_DSDS_PROBE="0", TEST_METAL_BUNDLE="1", TEST_LEVEL="75", TEST_LOG=TIMER,
            TEST_PRODUCT="thyme", TEST_FASTBOOT="", TEST_RUN_EXIT="0", TEST_PAYLOAD_EXIT="0",
        )
        self.env.pop("XL_FOLLOW", None)
        self.env.pop("XL_FOLLOW_KEEP", None)
        apfs = self.root / "tools" / "apfs"
        apfs.mkdir(parents=True)
        (apfs / "apfs.py").write_text(
            "import os, sys\n"
            "if os.environ['TEST_METAL_BUNDLE'] == '1':\n"
            "    sys.stdout.buffer.write(bytes.fromhex('cffaedfe') + "
            "b'the window server stays on its existing renderer (metal_apps=2)')\n"
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
        for arg in thermal=1 battery_stop=25 wdt=1; do
            case " $XL_CMDLINE " in *" $arg "*) ;; *) exit 1 ;; esac
        done
        case "$XL_CMDLINE:$TEST_METAL" in
            *" metal=1 metal_apps=2:1") ;;
            *" metal_apps=2:"*) exit 1 ;;
            *:0) ;;
            *) exit 1 ;;
        esac
        case "$XL_CMDLINE:$TEST_UIM_READONLY" in
            *" pcie2_uiswitch=0 pcie2_no_sms=1:1") ;;
            *" pcie2_uiswitch=0 "*) exit 1 ;;
            *:0) ;;
            *) exit 1 ;;
        esac
        case " $XL_CMDLINE :$TEST_DSDS_PROBE" in
            *" pcie2_dsds_probe=1 "*:1)
                [ "$XL_FOLLOW:$XL_FOLLOW_KEEP" = '5:1' ] || exit 1 ;;
            *" pcie2_dsds_probe=1 "*) exit 1 ;;
            *:0) ;;
            *) exit 1 ;;
        esac
        [ "$XL_USB_HANDOFF" = "$TEST_USB_HANDOFF" ] || exit 1
        if [ "$XL_USB_HANDOFF" = 1 ]; then
            exit "$TEST_RUN_EXIT"
        fi
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

    def test_explicit_metal_app_scene(self):
        result = self.soak(SOAK_METAL_APPS="1", TEST_METAL="1")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_read_only_uim_scene(self):
        result = self.soak(SOAK_UIM_READONLY="1", TEST_UIM_READONLY="1")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_dual_sim_registration_probe(self):
        result = self.soak(SOAK_DSDS_PROBE="1", TEST_DSDS_PROBE="1",
                           SOAK_UIM_READONLY="1", TEST_UIM_READONLY="1")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_usb_handoff_reset_and_failure(self):
        result = self.soak(SOAK_USB_HANDOFF="1", TEST_USB_HANDOFF="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("USB handoff reset observed", result.stdout)
        self.assertNotIn("timed return verified", result.stdout)
        self.assertFalse(list((self.root / "logs").glob("soak.*/kernel.log")))
        failed = self.soak(SOAK_USB_HANDOFF="1", TEST_USB_HANDOFF="1", TEST_RUN_EXIT="2")
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("USB handoff run failed", failed.stderr)

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
            {"SOAK_UIM_READONLY": "all"}, {"SOAK_DSDS_PROBE": "all"},
            {"TEST_FASTBOOT": "OTHER fastboot"}, {"SOAK_STEP": "00"}, {"SOAK_USB_HANDOFF": "all"}, {"TEST_PAYLOAD_EXIT": "1"}, {"SOAK_METAL_APPS": "all"},
            {"SOAK_METAL_APPS": "1", "TEST_METAL_BUNDLE": "0"},
        ):
            with self.subTest(overrides=overrides):
                result = self.soak(**overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "uploaded").exists())


if __name__ == "__main__":
    unittest.main()
