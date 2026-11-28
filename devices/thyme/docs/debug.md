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
its smoke checks in the same boot. Native macOS discovery,
unified log streaming, screen power state and deployment of long-lived
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

### XNU USB 2.0 bulk loopback

Build the kernel with `CONFIG_HAKIOS_THYME_USB=y` (included in
`make thyme_defconfig`) and rebuild the thyme loader. Use the debug RAM
scene and launch from Android with the cable connected:

```sh
SOAK_USB_HANDOFF=1 SOAK_USB_GADGET=1 SOAK_UIM_READONLY=1 SOAK_SECS=175 \
  SCENE=PATH_TO_DEBUG_SCENE devices/thyme/soak.sh
```

Use `XL_KERNEL` with an out-of-tree kernel binary. The probe requires the
loader's handoff property and verifies that DWC3 is halted before
reprogramming it. Inspect the controller from the pinned LAN client:

```sh
python3 tools/hdb.py --host PHONE_LAN_IP shell /var/jb/usr/sbin/sysctl kern.hakios.thyme_usb
```

Stage 2 denotes a running controller. The `connect`, `reset`, `setup`,
`address` and `configuration` fields show separate progress through USB
enumeration. The Qualcomm USB2 session-valid override follows the
charger report and is restored when the kernel leaves. Microsoft OS
descriptors advertise WinUSB and register the test interface GUID on
Windows. See [Microsoft's WinUSB device requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/automatic-installation-of-winusb).

From a native Windows PowerShell session, run:

```powershell
.\devices\thyme\tools\test-usb-bulk.ps1 -WaitSeconds 90 -Rounds 3
```

The probe echoes one packet from bulk OUT `0x01` to bulk IN `0x81` before
arming the next receive. The runner verifies lengths and bytes across
three WinUSB handle opens by default. PHY and bulk counters are readable
over LAN even while Windows uses the USB interface.

To exercise software detach and reattachment, request a reconnect from
the authenticated debug client after the first round set:

```sh
python3 tools/hdb.py --host PHONE_LAN_IP shell /var/jb/usr/sbin/sysctl -w kern.hakios.thyme_usb.reconnect=1
```

Wait for `reconnects` to increment and `configuration` to return to 1,
then rerun the WinUSB loopback. Physical cable reattachment is a separate
test.

`1209:0001` is non-unique and reserved for private testing. Obtain an
assigned identity before distributing a device that enumerates. See
[pid.codes' Test PID](https://pid.codes/1209/0001/) and the
[Qualcomm USB wrapper registers](https://github.com/torvalds/linux/blob/master/drivers/usb/dwc3/dwc3-qcom.c).

### usbmux version probe

Choose the USB protocol profile in the same guarded RAM Boot workflow:

```sh
SOAK_USB_HANDOFF=1 SOAK_USB_GADGET=2 SOAK_UIM_READONLY=1 SOAK_SECS=150 \
  SCENE=PATH_TO_DEBUG_SCENE devices/thyme/soak.sh
```

The distinct product description is `HakiOS thyme usbmux test`. It uses
the same private test VID/PID and WinUSB interface GUID. From native
Windows run `.\devices\thyme\tools\test-usb-bulk.ps1 -UsbMux`. The probe
requests a version, verifies the v1.0 wire response, and checks a closed
port response for 62079. It exercises packet fragmentation, coalesced
frames and recovery after an invalid length. Check `mux_versions`,
`mux_refused` and `mux_invalid` in `kern.hakios.thyme_usb` over LAN.
Use profile 1 for byte loopback.

The development VID/PID is outside the filter used by stock usbmuxd.
On WSL2, run the unmodified libimobiledevice client through the
development adapter:

```sh
python3 devices/thyme/tools/usbmux-winusb.py -- idevice_id -l
```

The adapter creates a private Unix socket for its child process and
uses native Windows WinUSB to verify the physical device descriptor
and usbmux version. `ListDevices` reports a development locator derived
from the Windows USB interface path in the protocol's `SerialNumber`
field. The physical serial is unavailable in this profile. A
`Connect` request opens a new connection on the same verified interface.
The device refuses every port while the service is offline and accepts
only 62078 when the development Lockdown program has opened the node.
The bridge relays one framed request at a time using the private WinUSB
interface. Requests and responses may use multiple USB packets, up to
32 KiB including the Lockdown length prefix. See the upstream
[USB filter](https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.c#L763)
and [local socket handling](https://github.com/libimobiledevice/libusbmuxd/blob/master/src/libusbmuxd.c#L155).

#### Read-only Lockdown over USB

Build the arm64 probe with the local iPhoneOS 16.5 SDK. In a timed RAM
Boot using USB profile 2 and the SSH scene, upload and run it from the
RAM data volume in a dedicated terminal.

```sh
. ./xnu-env.sh
mkdir -p out/thyme-boot
# Use OpenSSL 3 headers for the Darwin arm64 cross-build. This example uses
# the headers shipped by Ubuntu libnode-dev.
openssl_headers=/usr/include/node/openssl/archs/darwin64-arm64-cc/no-asm/include
jb_lib="$PWD/jb/root/var/jb/usr/lib"
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" \
  -O2 -Wall -Wextra -I drivers/core -I userland/src \
  -I "$openssl_headers" -I /usr/include/node \
  -framework CoreFoundation -framework Security -lresolv \
  "$jb_lib/libssl.3.dylib" "$jb_lib/libcrypto.3.dylib" -Wl,-rpath,/var/jb/usr/lib \
  -o out/thyme-boot/lockdown-usb devices/thyme/tools/lockdown-usb.c \
  devices/thyme/tools/lockdown-pairing.c devices/thyme/tools/lockdown-tls.c \
  devices/thyme/tools/afc-ram.c
python3 tools/hdb.py --host PHONE_LAN_IP push out/thyme-boot/lockdown-usb /private/var/tmp/lockdown-usb
python3 tools/hdb.py --host PHONE_LAN_IP shell chmod 700 /private/var/tmp/lockdown-usb
python3 tools/hdb.py --host PHONE_LAN_IP shell /private/var/tmp/lockdown-usb
```

For an independent test of USB, package the probe in a dedicated
SSH development scene. Append the job to a scratch copy of the
scenario. The root image remains local because it contains Wi-Fi and
SSH credentials.

```sh
work=$(mktemp -d)
cp userland/rc.jobs.ssh "$work/rc.jobs.ssh"
sed -i '1a /redwood/bin/lockdown-usb &' "$work/rc.jobs.ssh"
SCENE=usb-lockdown RC_JOBS="$work/rc.jobs.ssh" \
  EXTRA="$PWD/out/thyme-boot/lockdown-usb" devices/thyme/build-root.sh
SOAK_USB_HANDOFF=1 SOAK_USB_GADGET=2 SOAK_UIM_READONLY=1 SOAK_SECS=250 \
  SCENE=out/thyme-boot/root-usb-lockdown.apfs devices/thyme/soak.sh
```

Then read the advertised hardware on the computer using the original
libimobiledevice command.

```sh
python3 devices/thyme/tools/usbmux-winusb.py -- ideviceinfo -s
```

The root-only `/dev/hakios-lockdown` device connects the USB transport
to the user-space program. The program supports `QueryType`, `GetValue`
and `Goodbye`. `GetValue` exposes the model, board, SoC and system
name. The wire protocol type is `com.apple.mobile.lockdown`. On the
physical thyme, unmodified `ideviceinfo -s` returns Xiaomi Mi 10S,
thyme, SM8250-AC and HakiOS. `ProductVersion` and `BuildVersion` report
the compatibility metadata from the running image's SystemVersion
plist. The verified image reports 16.5 and 20F66. The explicitly
approved developer record also supports encrypted queries from
OpenSSL-based ideviceinfo 1.3.0. The same development profile also
supports the read-only diagnostics relay below.

Windows enumerated the development interface as 1209:0001 with
WINUSB while Apple Mobile Device Service was running. The Apple USB
driver on that host lists VID_05AC. i4Tools 9.22.032.0 in the operator
screenshot and 9.23.010.0 in the follow-up run both stayed on the
connection help page. Native Windows discovery remains open.

#### Pairing protocol preparation

The development daemon returns a 2048-bit `DevicePublicKey` as
PKCS#1 PEM. The corresponding key is generated on-device in RAM
and lives for the daemon's lifetime. The private host adapter defaults
to a per-process BUID and absent pair records. In an earlier guarded run, Ubuntu `idevicepair 1.3.0` read the device key
and sent a 4,855-byte framed `Pair` request with three certificates.
That request received a matching `InvalidRequest` response. The
explicit approval described below applies to a separately enrolled
host record.

#### Explicit RAM-only developer approval

The device reads one local approval from
`/private/var/tmp/hakios-lockdown-approval.plist`. Install it as root
through the existing authenticated HDB session with mode `0600`. It is a
plist dictionary with `HostID` and `SystemBUID` UUID strings and
`RootCertificateSHA256` and `HostCertificateSHA256` data values. Each
digest contains the 32 SHA-256 bytes of its DER certificate. The
daemon checks the two fingerprints, verifies both leaf certificates
against that root and compares the device certificate's public key
with the key it generated locally. [Security trust API](https://developer.apple.com/documentation/security/trust)

A successful `Pair` keeps one record in RAM. `ValidatePair` checks the
record and active approval again. Remove the approval file to revoke
it. USB reattachment can retain the record within the same daemon
lifetime.

#### Read-only authenticated Lockdown session

A successful `StartSession` reply upgrades that connection to TLS 1.2.
The device presents the paired device certificate, requires proof of
the exact approved client certificate, and rechecks the active RAM
approval before processing encrypted requests. This supports encrypted read-only queries and ends
the session on StopSession or revocation. Open a fresh connection for
another session. System management services are still pending.

The default TLS client identity is the approved host leaf. The OpenSSL
backend in libimobiledevice 1.3.0 presents the pair record's root as its
client certificate. For a dedicated development record, set the RAM
approval's `TLSClientCertificate` string to `RootCertificate` to select
the exact pinned root instead. The selected certificate must pass
OpenSSL's normal TLS client validation and prove possession of the
private key. Changing the selection revokes active sessions using the
previous identity. Use a development CA for this test. The original
client does not verify the device certificate, so use this profile on
the controlled test cable and private bridge.
[libimobiledevice OpenSSL client](https://github.com/libimobiledevice/libimobiledevice/blob/1.3.0/src/idevice.c#L978-L1006)

The bridge reads a local record only when given `--pair-record PATH`.
The file must be a regular file owned by the current user with group and
other access disabled. Add the device locator from `idevice_id -l` as
`PairRecordID` to the usual pair-record dictionary. The bridge provides
the matching record and its `SystemBUID` to the scoped host process.

The integration test requires Ubuntu's libimobiledevice 1.3.0 tools,
Python `cryptography` and enrolled HDB credentials. It generates a
local ephemeral CA and client certificate, temporarily installs the
approved pins, and uses the original libimobiledevice library for
Pair and ValidatePair. Python's TLS client exercises the encrypted
session using the approved host private key and verifies the paired
device certificate.

```sh
python3 devices/thyme/tools/usbmux-winusb.py -- \
  python3 devices/thyme/tools/test-lockdown-pairing.py --hdb-state ssh/hdb --reattach
```

The last option detaches and reattaches the USB interface before
`ValidatePair` to exercise recovery within the same RAM Boot. To include
unmodified ideviceinfo with the pairing-root identity, use this test
fixture. Its private key lives in the host's temporary RAM directory.

```sh
fixture=$(mktemp -d /dev/shm/thyme-pair.XXXXXXXX)
python3 devices/thyme/tools/usbmux-winusb.py --pair-record "$fixture/pair.plist" -- \
  python3 devices/thyme/tools/test-lockdown-pairing.py --hdb-state ssh/hdb --reattach \
  --stock-record "$fixture/pair.plist"
rmdir "$fixture"
```

#### Read-only diagnostics over USB

An authenticated Lockdown session can request
`com.apple.mobile.diagnostics_relay` with `StartService`. The returned
port is 62080 in the development profile. The offer is single use,
expires after 15 seconds, and requires a fresh mutual TLS handshake
using the same approved pairing identity. The private carrier services
one connection at a time. The original diagnostics CLI closes Lockdown
before connecting to the service port.
[libimobiledevice diagnostics CLI](https://github.com/libimobiledevice/libimobiledevice/blob/1.3.0/tools/idevicediagnostics.c#L198-L215)

With a live approval and local pair record, use the original client.

```sh
python3 devices/thyme/tools/usbmux-winusb.py --pair-record PATH -- idevicediagnostics diagnostics All
```

The reply contains the HakiOS identity and live DWC3 counters. The
implemented requests are `All` and `Goodbye`. The integration test above
also exercises this service when passed `--diagnostics` after
`--stock-record`.

#### AFC transfers within the development RAM volume

The service `com.apple.afc` uses port 62081 with the same single-use
StartService and independent TLS admission as diagnostics. Its remote
root maps to `/private/var/tmp/hakios-afc` and opens only on ramfs.
Paths are resolved within that directory. Symlinks and parent path
components are rejected. The volume supports directories, file
metadata, open, read, write, seek, tell, close and removal.

A write request can carry up to 1 MiB. Reads may return shorter chunks,
so callers should read until EOF. Each file is capped at 16 MiB and the
daemon tracks a shared 32 MiB write budget across sessions. The AFC
volume information reports that development budget. Reconnecting to
USB preserves files until the RAM Boot ends and clears open handles.

The integration test uses the original libimobiledevice AFC API. It
checks bidirectional bytes, software USB reattachment and confinement
to the RAM directory. The optional `--large` argument sends a single
1 MiB write request.

```sh
fixture=$(mktemp -d /dev/shm/thyme-afc.XXXXXXXX)
python3 devices/thyme/tools/usbmux-winusb.py --pair-record "$fixture/pair.plist" -- \
  python3 devices/thyme/tools/test-afc-ram.py --hdb-state ssh/hdb \
  --stock-record "$fixture/pair.plist"
rmdir "$fixture"
```

#### Discover the SSH address over USB

When the development scene contains both HDB and the USB Lockdown
probe, HDB can read the current DHCP address from the phone before each
connection. On WSL2, keep the original `ideviceinfo` on PATH.

```sh
python3 devices/thyme/tools/usbmux-winusb.py -- \
  python3 tools/hdb.py --usb shell /var/jb/usr/bin/uname -r
```

On native Windows, HDB uses the repository's WinUSB probe directly.
Use a Windows-local state directory with the Windows private
`client_key` enrolled in the image and the corresponding device pin
in `known_hosts`.

```powershell
python.exe .\tools\hdb.py --state PATH_TO_WINDOWS_HDB_STATE --usb shell /var/jb/usr/bin/uname -r
```

The probe reads `SSHAddress` in the `com.hakios.debug` domain. It
reports the private IPv4 address of `en0` only when the development
credentials are present and the configured LAN peer shares its subnet.
The address can change whenever DHCP assigns a new lease. HDB uses the
address for the TCP connection and verifies the enrolled SSH host key.
On thyme, WSL2 opened a pinned shell before and after software USB
reattachment and transferred files after reattachment. Native Windows
HDB also discovered the address over WinUSB, authenticated with its own
enrolled key, transferred a test file over SFTP, and recovered after
software USB reattachment. A different host pin failed verification.
Use `--usb` for shell, push and pull. Use `--host` for a LAN-only session.

#### Capture a live screenshot over SSH

Add `SOAK_HDB_SCREEN=1` to the guarded development boot command.
Then run HDB after the display has started presenting frames.

```sh
python3 devices/thyme/tools/usbmux-winusb.py -- \
  python3 tools/hdb.py --usb screenshot screen.png
```

The command also supports `--host PHONE_LAN_IP`. The kernel returns a
copy of a completed composited frame only to root with
`hdb_screen=1`. The host validates the image data and saves an RGB PNG.
The kernel releases the display lock before copying data to user space.
The transfer uses the existing authenticated SSH connection.
Physical thyme captures showed two different SpringBoard pages at
1080×2340 with EFI USB already handed off.

### USB Gadget and services

In standard RAM Boot, Windows lists `VID_18D1&PID_D00D` as the Android
Bootloader Interface. The EFI fastboot endpoint can serve a live
framebuffer capture while XNU runs. The XNU-side DWC3 probe reads the
controller's identification and status. The explicit development
profiles take over the halted controller and return it before the timed
reset.

1. Verify physical cable reconnect and native macOS and Linux enumeration.
   Extend the USB bulk transport to support concurrent system services.
2. Verify native host discovery and the standard device trust prompt.
   Export other verified thyme hardware and OS information as needed.
3. Extend AFC coverage and add authenticated installation proxy, logs
   and screenshots. The installer opens its proxy while Lockdown is
   still connected and keeps it open during AFC upload. Verify multiple
   USB streams before testing installation with an approved developer
   app and files in RAM. [ideviceinstaller connection flow](https://github.com/libimobiledevice/ideviceinstaller/blob/master/src/ideviceinstaller.c#L817-L958)
4. Test host identification and device management separately in
   i4Tools, 3uTools, iMazing, iTunes and Finder wherever available.
   Keep vendor-service and authorization results separate. Flashing
   and recovery actions remain subject to the installation gates.
5. Exercise OTG role changes, external storage, keyboard and mouse,
   and USB-C audio as separate peripheral checks.

See the [RAM Boot instructions](ram-boot.md) and
[acceptance checklist](../CHECKLIST.md).
