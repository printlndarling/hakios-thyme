#!/usr/bin/env python3
"""
Pack a Project Silicium UEFI image, the XNU loader payload and a device tree into an
Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container] + alignment) + DTB

For redwood the UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/,
see uefi/README.md) or, when there is none, from devices/redwood/prebuilt/.

Another device names its own prebuilt UEFI FD and DTB with --device and supplies the
three addresses from its UEFI memory map:

  ./mkxnuimg.py -d thyme --fd-base 0x9fc00000 --fd-size 0x300000 \
      --payload-base 0xf0000000 -p usb-payload.bin -o xnu-usb.img

The boot shim copies the FD to FD_BASE and the payload to PAYLOAD_BASE.
"""
import argparse
import gzip
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, "Mu-Silicium")
GENERIC_PREBUILT = os.path.join(HERE, "devices", "redwood", "prebuilt")
FD_BASE, FD_SIZE = 0x9FD00000, 0x200000
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument("-d", "--device", default="redwood")
ap.add_argument("-p", "--payload")
ap.add_argument("-o", "--output")
ap.add_argument("--mode", default="RELEASE")
ap.add_argument("--fd", help="UEFI FD (default: devices/DEVICE/prebuilt/SILICIUM_UEFI.fd)")
ap.add_argument("--dtb", help="device tree (default: devices/DEVICE/prebuilt/device-tree.dtb)")
ap.add_argument("--shim", help="prebuilt boot shim; otherwise build devices/DEVICE/tools/bootshim.S")
ap.add_argument("--fd-base", type=lambda value: int(value, 0))
ap.add_argument("--fd-size", type=lambda value: int(value, 0))
ap.add_argument("--payload-base", type=lambda value: int(value, 0))
args = ap.parse_args()

output = args.output or os.path.join(HERE, f"xnu-{args.device}.img")
device_prebuilt = os.path.join(HERE, "devices", args.device, "prebuilt")
mu_fd = os.path.join(MU, f"Build/{args.device}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd")
fd_base = args.fd_base if args.fd_base is not None else FD_BASE
fd_size = args.fd_size if args.fd_size is not None else FD_SIZE
payload_base = args.payload_base if args.payload_base is not None else PAYLOAD_BASE

if (args.device == "redwood" and os.path.isfile(mu_fd) and os.path.isdir(os.path.join(MU, "BootShim")) and
        args.fd_base is None and args.fd_size is None and args.payload_base is None):
    shim_dir = os.path.join(MU, "BootShim")
    for name in ("BootShim.elf", "BootShim.bin"):
        try:
            os.unlink(os.path.join(shim_dir, name))
        except FileNotFoundError:
            pass
    subprocess.check_call(
        ["make", "REQUIRES_KERNEL_HEADER=1", f"FD_BASE={FD_BASE:#x}", f"FD_SIZE={FD_SIZE:#x}",
         f"CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}"],
        cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {
        "shim": os.path.join(shim_dir, "BootShim.bin"),
        "fd": mu_fd,
        "dtb": os.path.join(MU, "Resources/DTBs/redwood.dtb"),
        "ramdisk": os.path.join(MU, "Resources/ramdisk"),
        "mkbootimg": os.path.join(MU, "Resources/Scripts/mkbootimg.py"),
    }
else:
    if args.device != "redwood" and (args.fd_base is None or args.fd_size is None or args.payload_base is None):
        sys.exit("mkxnuimg.py: --fd-base, --fd-size and --payload-base are required when not using the Mu-Silicium redwood build")
    if fd_base % 16 or fd_size <= 0 or fd_size % 16 or payload_base % 0x10000:
        sys.exit("mkxnuimg.py: unaligned UEFI or payload address")
    if fd_base < payload_base + PAYLOAD_MAX and payload_base < fd_base + fd_size:
        sys.exit("mkxnuimg.py: the UEFI FD overlaps the payload reservation")

    fd_path = args.fd or os.path.join(device_prebuilt, "SILICIUM_UEFI.fd")
    dtb_path = args.dtb or os.path.join(device_prebuilt, "device-tree.dtb")
    ramdisk_path = os.path.join(device_prebuilt, "ramdisk")
    if not os.path.isfile(ramdisk_path):
        ramdisk_path = os.path.join(GENERIC_PREBUILT, "ramdisk")
    mkbootimg_path = os.path.join(device_prebuilt, "mkbootimg.py")
    if not os.path.isfile(mkbootimg_path):
        mkbootimg_path = os.path.join(GENERIC_PREBUILT, "mkbootimg.py")

    for name, path in (("UEFI FD", fd_path), ("device tree", dtb_path), ("ramdisk", ramdisk_path),
                       ("mkbootimg.py", mkbootimg_path)):
        if not os.path.isfile(path):
            sys.exit(f"mkxnuimg.py: {name} not found: {path}")

    if args.shim:
        shim_path = args.shim
    elif args.device == "redwood":
        shim_path = os.path.join(GENERIC_PREBUILT, "BootShim.bin")
    else:
        shim_source = os.path.join(HERE, "devices", args.device, "tools", "bootshim.S")
        if not os.path.isfile(shim_source):
            sys.exit(f"mkxnuimg.py: boot shim source not found: {shim_source}")
        shim_tmp = tempfile.TemporaryDirectory(prefix=f"hakios-{args.device}-shim-")
        shim_obj = os.path.join(shim_tmp.name, "shim.o")
        shim_path = os.path.join(shim_tmp.name, "shim.bin")
        subprocess.check_call(
            ["clang", "--target=aarch64-linux-gnu", "-c", shim_source,
             f"-DFD_BASE={fd_base:#x}", f"-DFD_SIZE={fd_size:#x}",
             f"-DPAYLOAD_BASE={payload_base:#x}", "-o", shim_obj])
        subprocess.check_call(["aarch64-linux-gnu-objcopy", "-O", "binary", shim_obj, shim_path])

    parts = {
        "shim": shim_path,
        "fd": fd_path,
        "dtb": dtb_path,
        "ramdisk": ramdisk_path,
        "mkbootimg": mkbootimg_path,
    }

fd = open(parts["fd"], "rb").read()
if len(fd) != fd_size:
    sys.exit(f"mkxnuimg.py: UEFI FD size {len(fd):#x} differs from FD_SIZE={fd_size:#x}")
shim = open(parts["shim"], "rb").read()
dtb = open(parts["dtb"], "rb").read()

blob = shim + fd
if args.payload:
    data = open(args.payload, "rb").read()
    if data[:4] != b"XNUP" or len(data) > PAYLOAD_MAX - 0x1000:
        sys.exit("mkxnuimg.py: bad or oversized XNUP payload")
    blob += data
    blob += b"\0" * (-len(blob) % 16)
    print(f"payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}")

kernel = output + ".kernel~"
open(kernel, "wb").write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call(
        [sys.executable, parts["mkbootimg"], "--kernel", kernel, "--ramdisk", parts["ramdisk"],
         "--header_version", "1", "-o", output])
finally:
    os.unlink(kernel)
print(f"{output}: {os.path.getsize(output)} bytes (shim {len(shim)}, uncompressed {len(blob)})")
