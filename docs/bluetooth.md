# Bluetooth on thyme (QCA6390): the driver, the daemon, and the 3 Mbaud switch

The phone's Bluetooth radio is a Qualcomm QCA6390 behind the SoC's QUP6
UART (115200 baud from reset, 3 Mbaud for the run).  HakiOS drives it from
the kernel, not from a user-space HCI transport:

* `xnu-src/xnu-8796.121.2/bsd/dev/hakios.thyme_bluetooth.c` powers the
  radio (RPMH rails, GCC UART clock, reset GPIO21, awake GPIO124), does the
  ROM handshake at 115200, moves both sides to 3 Mbaud, downloads
  `htbtfw20.tlv`/`htnv20.bin` out of the boot image's `fw/qca6390-bt`,
  resets, reads the supported commands and the public address, sets the
  local name "HakiOS Mi 10S", and then publishes:
  * a kern control socket named `com.hakios.bluetooth` that carries H4
    (the same name and speed go into bluetoothd's `BT_HCI_TRANSPORT`), and
  * an IOService named `bluetooth` with `HakiosBluetoothSupportedCommands`
    (`xnu-src/xnu-8796.121.2/iokit/Kernel/hakios.thyme_bluetooth.cpp`).
* `userland/src/inject_bluetooth.c`, injected into bluetoothd, only patches
  the daemon when that IOService exists; it is the optional vendor-command
  compatibility layer, not the transport.
* The root image must be built with `BLUETOOTH=1` (`devices/thyme/build-root.sh`).
  Before that was true, mkjobs.py's `--rest` denied the jobs and Settings
  showed no Bluetooth row at all.  The jobs made now are com.apple.bluetoothd
  (transport `H4:POSIX:com.hakios.bluetooth,<speed>`, RunAtLoad), the tuned
  `com.hakios.bluetool` (/redwood/bin/umi_bluetool), BTServer.le/avrcp/map/pbap
  and bluetoothuserd.  The transport speed comes from `BT_BAUDRATE` at root
  build time (default 3000000) and must equal the kernel's `bt_baud`.

## The 3 Mbaud race (runs 232624, 001249, 002956)

Run 232624 brought the radio all the way up:

    UMI BT: controller and host moved to 3000000 baud
    UMI BT: QCA product=0x10 ROM=0x200 SoC=0x400a0200 patch=0xd2b
    UMI BT: TLV type=1 accepted: 188564 bytes, 776 segments
    UMI BT: TLV type=2 accepted: 5733 bytes, 24 segments
    UMI BT: firmware running after reset: HCI=0xb LMP=0xb subversion=0x27ec
    UMI BT: local name readback status=0 name="HakiOS Mi 10S"
    UMI BT: persistent H4 endpoint ready: com.hakios.bluetooth

Runs 001249 and 002956 stopped one step earlier, with identical host-side
register dumps and an identical cold ROM handshake:

    UMI BT: cold HCI=0x9 manufacturer=29 LMP subversion=0xd2b
    UMI BT: controller and host moved to 3000000 baud
    UMI BT: H4 framing error=-1 byte=0xff
    UMI BT: firmware/radio probe result=92

EILSEQ (92) on an idle-line byte (0xff) means the controller had *not*
moved to 3 Mbaud while the host had: the vendor command 0xfc48 was sent and
the host switched on a fixed 300 ms timer, without waiting for the
controller's Command Complete.  Linux's hci_qca waits for that CC before it
moves the host.  Android's own Bluetooth was off in both failing boots
(`bluetooth_manager: state OFF`, `bluetooth_on 0`), so this is not Android
leaving the radio mid-initialization.

With no endpoint published, launchd's bluetoothd found no controller and
every spawn printed `bluetoothd: No bluetooth on this device.` and exited:
Settings showed Bluetooth "Unavailable" (the row itself is Apple's, restored
by inject_prefs' PSCapabilityManager override).

## Verification at 115200 (run 005156)

Booting with `bt_baud=115200` and a root image built with
`BT_BAUDRATE=115200` skips the switch and brings the whole stack up:

* `UMI BT: persistent H4 endpoint ready: com.hakios.bluetooth`;
* bluetoothd[26] stays alive and drives HCI through the control socket
  (Read Local Version 0x1001, LE Set Scan Parameters 0x200b, LE Set Scan
  Enable 0x200c, vendor 0xc01-0xc7a, Write Local Name 0xc13, ...);
* `Update Dynamic Store with state On value 1` -- Settings' Bluetooth row
  becomes usable instead of "Unavailable";
* one vendor command is refused (`HCI command rejected opcode=0xfd48
  status=0x1`), which the daemon tolerates.

## The fix

`bt_qca_set_speed` now issues 0xfc48 through `bt_hci_command`, i.e. it
waits for the Command Complete before moving the host, with the old timer
path kept as fallback when no acknowledgement arrives.  `bt_qca_firmware`
retries the 0xfc00 version request up to four times (clearing the frame
parser between attempts) so a controller that needs a few more milliseconds
after the switch is not given up on.  The rebuilt kernel is `hakios-kernel`
(sha256 73039cad8f54...).

## Verified at 3 Mbaud (run 010549, kernel 73039cad8f54)

    UMI BT: baud-rate command unacknowledged errno=60; timer path
    UMI BT: controller and host moved to 3000000 baud
    UMI BT: QCA product=0x10 ROM=0x200 SoC=0x400a0200 patch=0xd2b
    UMI BT: loading htbtfw20/htnv20 at 3000000 baud (Hastings tag17 byte1)
    UMI BT: TLV type=1 accepted: 188564 bytes, 776 segments
    UMI BT: TLV type=2 accepted: 5733 bytes, 24 segments
    UMI BT: firmware running after reset: HCI=0xb LMP=0xb subversion=0x27ec
    UMI BT: local name readback status=0 name="HakiOS Mi 10S"
    UMI BT: persistent H4 endpoint ready: com.hakios.bluetooth

QCA6390 never answers 0xfc48, so the timer fallback is the path taken; what
made the difference is the clean 300 ms settle plus the retried 0xfc00.  In
that boot bluetoothd[26] stays up, issues 787 HCI commands through the
socket (0x200b/0x200c LE scan, 0xc01-0xc7a vendor, 0xc13 local name), and
`Update Dynamic Store with state On value 1` follows -- the Settings
Bluetooth row is live, with no `No bluetooth on this device.` and no H4
framing errors.  0xfd48 is still refused (status 1) and tolerated, as at
115200.  The 115200 path remains available as `XL_EXTRA_ARGS=bt_baud=115200`
with a root built at `BT_BAUDRATE=115200`.

To run it: `run4.sh` (boots `devices/thyme/prebuilt/xnu-usb.img`, sends the
current `hakios-kernel` and `userland/root.apfs`), with `XL_EXTRA_ARGS=`
empty for 3 Mbaud or `XL_EXTRA_ARGS=bt_baud=115200` for the low-speed
fallback; rebuild `userland/root.apfs` with the matching `BT_BAUDRATE`.
