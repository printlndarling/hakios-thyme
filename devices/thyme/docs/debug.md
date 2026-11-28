# Real-device debugging on thyme

## LAN development scene

The SSH scene uses the Procursus OpenSSH already present in the local
system image. It listens on the phone's private IPv4 address on the
thyme Wi-Fi interface and accepts one configured LAN peer with an
authorized public key. The host verifies the device key pinned at image
build time. The development shell runs as root, and forwarding and
password authentication are disabled. [OpenSSH configuration](https://man.openbsd.org/sshd_config)

Use the computer's **LAN** IPv4 address as `HOST_LAN_IP`. When running
inside WSL2, this is the address that the phone sees for the host
connection. The scene joins the network configured in the local
`fw-wifi/network` file. Use a system image with the OpenSSH prefix.

```sh
python3 tools/hdb.py prepare HOST_LAN_IP
SCENE=ssh devices/thyme/build-root.sh
SOAK_UIM_READONLY=1 SOAK_SECS=240 SCENE=out/thyme-boot/root-ssh.apfs devices/thyme/soak.sh
```

While the phone is running, read its address from the `hdb: SSH on`
line in the live RAM Boot log. Use that address as `PHONE_LAN_IP`.
Ordinary boot scenes do not build the launcher or carry its
credentials. The SSH scene holds a device private host key and the
local Wi-Fi profile. Keep this image and the development state local.

```sh
python3 tools/hdb.py --host PHONE_LAN_IP shell
python3 tools/hdb.py --host PHONE_LAN_IP shell /var/jb/usr/bin/uname -r
python3 tools/hdb.py --host PHONE_LAN_IP shell /var/jb/usr/sbin/sysctl -n kern.hakios.usb_dwc3_id
python3 tools/hdb.py --host PHONE_LAN_IP push ./probe /private/var/tmp/probe
python3 tools/hdb.py --host PHONE_LAN_IP pull /private/var/tmp/probe ./probe.readback
```

A Python 3 host with OpenSSH can use the same interface. The default
state directory is `ssh/hdb`. To keep a separate development image and
its host pin, use the same absolute directory with `hdb.py --state`
and the build's `HDB_STATE`. The RAM Boot timeout and shutdown guards
remain in force during SSH sessions.

## Windows-native OpenSSH

Generate a separate client key with Windows OpenSSH in your Windows
user profile. Keep the private key on Windows. Before building the
development scene, pass only its public key to HDB from WSL2.

```powershell
New-Item -ItemType Directory -Path "$env:USERPROFILE/.ssh" -Force
ssh-keygen.exe -t ed25519 -f "$env:USERPROFILE/.ssh/hakios-thyme"
```

```sh
python3 tools/hdb.py prepare HOST_LAN_IP --authorize /mnt/c/Users/USER/.ssh/hakios-thyme.pub
SCENE=ssh devices/thyme/build-root.sh
```

The build adds the Windows public key beside the WSL client's key.
Both are limited to the configured LAN peer. Copy only the public
`ssh/hdb/known_hosts` file to your Windows SSH directory. Use these
OpenSSH config settings on Windows, with your device address and local
paths. The same entry can be used by `ssh.exe` and `scp.exe`.

```sshconfig
Host hakios-thyme-hdb
    HostName PHONE_LAN_IP
    User root
    Port 2222
    IdentityFile ~/.ssh/hakios-thyme
    UserKnownHostsFile ~/.ssh/hakios-thyme-known_hosts
    GlobalKnownHostsFile none
    HostKeyAlias hakios-thyme-hdb
    StrictHostKeyChecking yes
    IdentitiesOnly yes
```

## Verified LAN functions

A 240-second protected RAM Boot on thyme accepted shell commands from
WSL2 and refused both a wrong client key and a wrong device pin. An
authenticated reconnect succeeded. SFTP transferred an arm64 program in
both directions with identical bytes, and HakiOS ran the uploaded copy
from the RAM data volume. The same session read the DWC3 ID and kernel
log. Use `shell /var/jb/sbin/dmesg` for the current kernel buffer.
`xl screenshot-live` also captured the screen during the run through
the loader's existing live USB control channel.

On the same device, Windows OpenSSH 9.5.4.1 verified the device pin,
authenticated with a Windows-generated key and received UID 0. Its
SFTP upload and readback matched by SHA-256. The WSL2 client passed
its smoke checks in the same boot. Native macOS, automatic discovery,
unified log streaming, SSH screen capture and deployment of long-lived
services have separate acceptance.

## USB device and host work

### EFI USB handoff test

The normal RAM Boot uses the loader's fastboot interface while XNU runs.
The optional thyme handoff run closes EFI USB just before XNU starts.
The loader's CPU continues to service the charger and storage mailbox.
The XNU watchdog stays enabled and the loader has a separate deadline
for a kernel that overruns its timer.
Qualcomm's Stop contract ends access to the device protocol, so the
loader cold-resets to Android when XNU hands control back. See the
[Qualcomm EFI USB device protocol](https://github.com/SHIFTPHONES/android_bootable_bootloader_edk2/blob/sos-3.x/QcomModulePkg/Include/Protocol/EFIUsbDevice.h).

Build a thyme RAM loader with `devices/thyme/build-loader.sh` and use the
authenticated LAN debug image. With the cable attached and the handset
in Android, run:

```sh
SOAK_USB_HANDOFF=1 SOAK_UIM_READONLY=1 SOAK_SECS=150 \
  SCENE=PATH_TO_DEBUG_SCENE devices/thyme/soak.sh
```

Set `XL_BOOT_IMAGE` and `XL_LOADER` when using a separate build directory.
Watch the native host for the removal of `VID_18D1&PID_D00D`. From the
pinned LAN client, run `python3 tools/hdb.py --host PHONE_LAN_IP shell`
and read `kern.hakios.usb_dwc3_id` and `kern.hakios.usb_dwc3_dsts` while
the EFI interface is absent. On thyme, Windows observed the detach and
the pinned WSL2 client read ID `0x33313130` and DSTS `0x00d2dd7c`. Bit 22
of DSTS reports that the device controller halted. See the
[DWC3 register definitions](https://github.com/torvalds/linux/blob/master/drivers/usb/dwc3/core.h).
The handoff mode records Android boot completion. Capture diagnostics
over SSH while the kernel is live, because it discards the RAM log at
reset. Use standard RAM Boot for the final kernel log and the
`loader_return_after expired` marker.

### USB Gadget and services

In standard RAM Boot, Windows lists `VID_18D1&PID_D00D` as the Android
Bootloader Interface. The EFI fastboot endpoint can serve a live
framebuffer capture while XNU runs. The XNU-side DWC3 probe reads the
controller's identification and status. The next stage is to use the
halted controller for an XNU device-mode driver and verify its shutdown
during the timed return.

1. Bring up the XNU USB Device driver with the EFI interface stopped.
   Validate enumeration, endpoint transfers, disconnect, reconnect
   and timed shutdown on the native host.
2. Validate discovery and transport with usbmuxd and libimobiledevice,
   then implement and test trust and lockdown session handling. Export
   actual thyme hardware and OS information where the protocol permits,
   and document compatibility identifiers.
3. Verify AFC, installation proxy, logs and screenshots with the open
   tools. Use an approved developer app and temporary files in the RAM
   session.
4. Test host identification and device management separately in
   i4Tools, 3uTools, iMazing, iTunes and Finder wherever available.
   Keep vendor-service and authorization results separate. Flashing
   and recovery actions remain subject to the installation gates.
5. Exercise OTG role changes, external storage, keyboard and mouse,
   and USB-C audio as separate peripheral checks.

See the [RAM Boot instructions](ram-boot.md) and
[acceptance checklist](../CHECKLIST.md).
