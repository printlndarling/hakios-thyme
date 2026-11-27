#!/usr/bin/env python3
"""Start a locally prepared Xiaomi Mi 10S HakiOS bundle from RAM."""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "out" / "thyme-ram-boot"
FILES = {
    "xnu-usb.img": "devices/thyme/prebuilt/xnu-usb.img",
    "hakios-kernel": "hakios-kernel",
    "root.apfs": "userland/root.apfs",
    "system.apfs": "userland/system.apfs",
}
PLATFORMS = {"Windows": "windows", "Linux": "linux", "Darwin": "darwin"}
BOOT_ARGS_LIMIT = 607
PAYLOAD_LIMIT = 66_060_288


def download(url):
    if urllib.parse.urlsplit(url).scheme != "https":
        raise ValueError("Downloads require HTTPS")
    response = urllib.request.urlopen(url, timeout=60)
    if urllib.parse.urlsplit(response.geturl()).scheme != "https":
        response.close()
        raise ValueError("The download redirected away from HTTPS")
    return response


def sha256(path):
    with path.open("rb") as file:
        checksum = hashlib.sha256()
        for block in iter(lambda: file.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def bundle_from_url(url):
    with download(url) as response:
        manifest = json.loads(response.read(65_537))
    if not isinstance(manifest, dict) or manifest.get("device") != "thyme" or manifest.get("version") != 1:
        raise ValueError("Expected a version 1 thyme image manifest")
    entries = manifest.get("files")
    if not isinstance(entries, dict) or set(entries) != FILES.keys():
        raise ValueError("The manifest must name exactly the four thyme bundle files")

    result = {}
    for name, entry in entries.items():
        digest = entry.get("sha256") if isinstance(entry, dict) else None
        source = entry.get("url") if isinstance(entry, dict) else None
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError(f"Invalid SHA-256 for {name}")
        if not isinstance(source, str) or not source:
            raise ValueError(f"Missing download URL for {name}")
        digest = digest.lower()
        target = CACHE / "images" / digest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or not target.stat().st_size or sha256(target) != digest:
            print(f"Downloading {name}", flush=True)
            with tempfile.TemporaryDirectory(dir=target.parent) as folder:
                part = Path(folder) / name
                with download(urllib.parse.urljoin(url, source)) as response, part.open("wb") as out:
                    shutil.copyfileobj(response, out)
                if not part.stat().st_size or sha256(part) != digest:
                    raise ValueError(f"SHA-256 mismatch for {name}")
                part.replace(target)
        result[name] = target
    return result


def local_bundle(directory):
    if directory is None and (ROOT / "thyme-bundle").is_dir():
        directory = ROOT / "thyme-bundle"
    files = {name: directory / name if directory else ROOT / rel for name, rel in FILES.items()}
    missing = [str(path) for path in files.values() if not path.is_file() or path.stat().st_size == 0]
    if missing:
        raise ValueError("Missing images:\n  " + "\n  ".join(missing) +
                         "\nSupply a prepared bundle with --bundle-dir or a trusted HTTPS --bundle-url. "
                         "See the README for how to build the private images.")
    return files


def platform_tools():
    adb, fastboot = shutil.which("adb"), shutil.which("fastboot")
    if adb and fastboot:
        return adb, fastboot
    system = platform.system()
    suffix = ".exe" if system == "Windows" else ""
    names = ("adb" + suffix, "fastboot" + suffix)
    required = set(names)
    if suffix:
        required.update(("AdbWinApi.dll", "AdbWinUsbApi.dll"))
    if system not in PLATFORMS:
        raise ValueError(f"Android Platform-Tools are needed on {system}")
    folder = CACHE / PLATFORMS[system]
    if not all((folder / name).is_file() for name in required):
        folder.mkdir(parents=True, exist_ok=True)
        print("Downloading Google Android Platform-Tools", flush=True)
        url = "https://dl.google.com/android/repository/platform-tools-latest-" + PLATFORMS[system] + ".zip"
        with tempfile.TemporaryDirectory(dir=folder.parent) as temporary:
            archive = Path(temporary) / "tools.zip"
            with download(url) as response, archive.open("wb") as out:
                shutil.copyfileobj(response, out)
            with zipfile.ZipFile(archive) as sdk:
                for name in required:
                    member = "platform-tools/" + name
                    if member not in sdk.namelist():
                        raise ValueError(f"The Platform-Tools archive did not contain {name}")
                    extracted = Path(temporary) / name
                    with sdk.open(member) as source, extracted.open("wb") as target:
                        shutil.copyfileobj(source, target)
                    if not suffix:
                        extracted.chmod(0o755)
                for name in required:
                    (Path(temporary) / name).replace(folder / name)
    return tuple(str(folder / name) for name in names)


def command(program, *args, timeout=30, check=True):
    result = subprocess.run([program, *map(str, args)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
    if check and result.returncode:
        raise ValueError(f"{Path(program).name} {args[0]} failed:\n{result.stdout.strip()}")
    return result.stdout


def devices(program, state):
    return [fields[0] for line in command(program, "devices").splitlines()
            if len(fields := line.split()) >= 2 and fields[1] == state]


def fastboot_args(serial):
    return ("-s", serial) if serial and not re.fullmatch(r"\?+", serial) else ()


def wait_for_device(fastboot, product, deadline):
    while time.monotonic() < deadline:
        attached = devices(fastboot, "fastboot")
        if len(attached) > 1:
            raise ValueError("Disconnect other fastboot devices")
        if attached:
            candidate = attached[0]
            try:
                output = command(fastboot, *fastboot_args(candidate), "getvar", "product", timeout=5, check=False)
            except subprocess.TimeoutExpired:
                time.sleep(1)
                continue
            if re.search(rf"^product:\s*{re.escape(product)}\s*$", output, re.MULTILINE):
                return candidate
        time.sleep(2)
    raise ValueError(f"Timed out waiting for {product} over fastboot")


def preflight(adb):
    attached = devices(adb, "device")
    if len(attached) != 1 or ":" in attached[0]:
        raise ValueError("Connect one authorized Xiaomi Mi 10S over USB, running Android")
    serial = attached[0]
    if command(adb, "-s", serial, "shell", "getprop", "ro.product.device").strip() != "thyme":
        raise ValueError("The Android device did not identify itself as thyme")
    battery = command(adb, "-s", serial, "shell", "dumpsys", "battery")
    level = re.search(r"^\s*level:\s*(\d+)\s*$", battery, re.MULTILINE)
    temp = re.search(r"^\s*temperature:\s*(\d+)\s*$", battery, re.MULTILINE)
    if not level or not temp or int(level[1]) < 40 or int(temp[1]) >= 450:
        raise ValueError("RAM boot requires at least 40% charge and battery temperature below 45 °C")
    return serial


def build_payload(files, payload):
    args = " ".join((ROOT / "devices/thyme/ram.args").read_text().split())
    cmdline = (f"loader_return_after=0 epoch={int(time.time())} sb_delay=8 debug=0x8 rd=md0 "
               f"{args} pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=2 pcie2_nr=0")
    mac_file = ROOT / "fw-wifi" / "mac"
    if mac_file.is_file():
        mac = mac_file.read_text().strip()
        if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", mac):
            raise ValueError("fw-wifi/mac does not contain a Wi-Fi MAC address")
        cmdline += " wifi_mac=" + mac
    if len(cmdline.encode("ascii")) >= BOOT_ARGS_LIMIT:
        raise ValueError("The boot arguments exceed the loader limit")
    subprocess.run([sys.executable, str(ROOT / "xnuloader/mkpayload.py"), "-o", str(payload),
                    f"kernel={files['hakios-kernel']}", f"ramdisk={files['root.apfs']}",
                    "--cmdline", cmdline, "--no-version"], check=True)
    if payload.stat().st_size > PAYLOAD_LIMIT:
        raise ValueError(f"Kernel payload exceeds the thyme limit of {PAYLOAD_LIMIT:,} bytes")


def ram_boot(adb, fastboot, files, payload):
    android = preflight(adb)
    if devices(fastboot, "fastboot"):
        raise ValueError("Disconnect other fastboot devices before rebooting thyme")
    print("Entering the thyme bootloader", flush=True)
    command(adb, "-s", android, "reboot", "bootloader")
    serial = wait_for_device(fastboot, "thyme", time.monotonic() + 60)
    try:
        unlocked = command(fastboot, *fastboot_args(serial), "getvar", "unlocked", check=False)
        if not re.search(r"^unlocked:\s*yes\s*$", unlocked, re.IGNORECASE | re.MULTILINE):
            info = command(fastboot, *fastboot_args(serial), "oem", "device-info", check=False)
            if not re.search(r"Device unlocked:\s*true\b", info, re.IGNORECASE):
                raise ValueError("Fastboot did not confirm an unlocked bootloader")
        command(fastboot, *fastboot_args(serial), "boot", files["xnu-usb.img"], timeout=40)
        serial = wait_for_device(fastboot, "xnuloader", time.monotonic() + 60)
        print("Uploading system.apfs to RAM. Keep the USB cable connected.", flush=True)
        command(fastboot, *fastboot_args(serial), "oem", "ramdisk")
        command(fastboot, *fastboot_args(serial), "stage", files["system.apfs"], timeout=1800)
        command(fastboot, *fastboot_args(serial), "stage", payload, timeout=60)
        command(fastboot, *fastboot_args(serial), "oem", "run")
    except (ValueError, OSError, subprocess.SubprocessError, KeyboardInterrupt):
        try:
            if devices(fastboot, "fastboot") == [serial]:
                command(fastboot, *fastboot_args(serial), "reboot", check=False)
        except (ValueError, OSError, subprocess.SubprocessError):
            pass
        raise
    print("RAM boot requested. Wait for SpringBoard. Restart the phone to return to Android.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    bundle = parser.add_mutually_exclusive_group()
    bundle.add_argument("--bundle-dir", type=Path, help="directory holding the four image files")
    bundle.add_argument("--bundle-url", help="trusted HTTPS manifest for first-run image download")
    parser.add_argument("--check", action="store_true", help="prepare and validate the payload without USB")
    args = parser.parse_args()
    url = args.bundle_url
    if url is None and args.bundle_dir is None:
        url = os.environ.get("HAKIOS_THYME_BUNDLE_URL")

    try:
        files = bundle_from_url(url) if url else local_bundle(args.bundle_dir)
        with tempfile.TemporaryDirectory() as folder:
            payload = Path(folder) / "run-payload.bin"
            build_payload(files, payload)
            if not args.check:
                ram_boot(*platform_tools(), files, payload)
    except (ValueError, OSError, subprocess.SubprocessError, zipfile.BadZipFile, KeyboardInterrupt) as error:
        parser.exit(1, f"RAM boot: {error or 'interrupted'}\n")


if __name__ == "__main__":
    main()
