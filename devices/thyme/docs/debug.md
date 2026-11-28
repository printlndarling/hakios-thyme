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

## Verified LAN functions

A 240-second protected RAM Boot on thyme accepted shell commands from
WSL2 and refused both a wrong client key and a wrong device pin. An
authenticated reconnect succeeded. SFTP transferred an arm64 program in
both directions with identical bytes, and HakiOS ran the uploaded copy
from the RAM data volume. The same session read the DWC3 ID and kernel
log. Use `shell /var/jb/sbin/dmesg` for the current kernel buffer.
`xl screenshot-live` also captured the screen during the run through
the loader's existing live USB control channel.

The native Windows OpenSSH client reached the device and verified its
pinned key. Windows refused the private key at the WSL UNC path because
its Windows permissions were too open. A Windows-native key and
authorized public-key enrollment are still needed to verify shell and
file transfer there. Native macOS, automatic discovery, unified log
streaming, SSH screen capture and deployment of long-lived services
have separate acceptance.

## USB device and host work

1. Verify DWC3 ownership and the transition from the RAM Boot loader.
   Bring up USB Device mode and validate enumeration, endpoint transfers,
   disconnect and reconnect on the native host. The current kernel
   provides the controller identification and status registers.
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
