#!/usr/bin/env python3
"""Refuse a THYME RAM root image that silently lost a daily-driver component.

usage: check-root.py ROOT_DIR RC_JOBS

ROOT_DIR is the tree build.sh packed (userland/root); RC_JOBS is the scenario
file copied to etc/rc.jobs. The check exists because build.sh has independent
switches (GL, ADSP, BLUETOOTH) and a rebuild with one of them missing still
produces a bootable image -- it just renders without Mesa, has no DSP firmware
or never opens the Bluetooth H4 transport. Each of those has already cost a
phone test. Fail here, before the image is promoted.
"""
import hashlib
import os
import plistlib
import sys

root, rc_jobs = sys.argv[1], sys.argv[2]
missing = []


def need(rel):
    if not os.path.isfile(os.path.join(root, rel)):
        missing.append(rel)


def plist(rel):
    with open(os.path.join(root, rel), "rb") as f:
        return plistlib.load(f)


# The GPU userspace: without these the kernel's GPU stays on but no hakios-gl
# frame is submitted, and the UI runs on the slow path.
for rel in ("bin/gl_test", "lib/libhakios.gl.dylib", "lib/libOSMesa.8.dylib",
            "lib/libhakios.media.dylib", "lib/HakiosAudio.driver/HakiosAudio"):
    need(rel)

# The QCA bootstrap contract and the Settings computer name.
for rel in ("bin/umi_bluetool", "bin/scname", "lib/libhakios.bluetooth.dylib",
            "LaunchDaemons/com.apple.bluetoothd.plist",
            "LaunchDaemons/com.apple.BTServer.le.plist",
            "LaunchDaemons/com.apple.BTServer.avrcp.plist",
            "LaunchDaemons/com.apple.BTServer.map.plist",
            "LaunchDaemons/com.apple.BTServer.pbap.plist",
            "LaunchDaemons/com.apple.bluetoothuserd.plist",
            "LaunchDaemons/com.hakios.scname.plist"):
    need(rel)

# Firmware the kernel reads from /redwood/fw after it mounts the image.
for rel in ("fw/adsp.mdt", "fw/adsp.b00", "fw/adsp.b18",
            "fw/acdbdata/Tutu/Tutu_Bluetooth_cal.acdb",
            "fw/cs35l41/cs35l41-dsp1-spk-prot.bin",
            "fw/aw8697_haptic.bin", "fw/aw8697_rtp_1.bin",
            "fw/camera-s5khmx-v3.tab", "fw/camera-s5khmx-v19.tab",
            "fw/qca6390-bt/htbtfw20.tlv", "fw/qca6390/Data.msc",
            "fw/qca6390/WCNSS_qcom_cfg.ini"):
    need(rel)

# A named job must actually be the QCA responder, not stock BlueTool.
if os.path.isfile(os.path.join(root, "LaunchDaemons/com.apple.BlueTool.plist")):
    job = plist("LaunchDaemons/com.apple.BlueTool.plist")
    if job.get("Label") != "com.hakios.bluetool" or job.get("ProgramArguments") != ["/redwood/bin/umi_bluetool"]:
        missing.append("LaunchDaemons/com.apple.BlueTool.plist (not the QCA responder)")
    if job.get("RunAtLoad") is not True:
        missing.append("LaunchDaemons/com.apple.BlueTool.plist (RunAtLoad is not set)")

if os.path.isfile(os.path.join(root, "LaunchDaemons/com.apple.bluetoothd.plist")):
    env = plist("LaunchDaemons/com.apple.bluetoothd.plist").get("EnvironmentVariables", {})
    if "com.hakios.bluetooth" not in env.get("BT_HCI_TRANSPORT", ""):
        missing.append("LaunchDaemons/com.apple.bluetoothd.plist (no H4 transport)")

# The scenario file must be the one that was asked for.
jobs = os.path.join(root, "etc/rc.jobs")
source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "userland", rc_jobs)
if not os.path.isfile(jobs):
    missing.append("etc/rc.jobs")
elif os.path.isfile(source):
    want = hashlib.sha256(open(source, "rb").read()).hexdigest()
    got = hashlib.sha256(open(jobs, "rb").read()).hexdigest()
    if want != got:
        missing.append("etc/rc.jobs (expected %s)" % rc_jobs)

if missing:
    print("check-root: REFUSING this image; missing or wrong:", file=sys.stderr)
    for rel in missing:
        print("  " + rel, file=sys.stderr)
    sys.exit(1)

print("check-root: GL, Bluetooth, audio and camera components present; rc.jobs=%s" % rc_jobs)
