# Official-main Bluetooth probe

Date: 2026-11-01

## Build

The device-owned QCA6390 driver builds as `CONFIG_HAKIOS_THYME_BT`; the Redwood
Bluetooth driver is disabled. Firmware is loaded from the system image at
runtime:

```text
/fw/qca6390-bt/htbtfw20.tlv
/fw/qca6390-bt/htnv20.bin
```

This keeps the vendor firmware out of the Git repository.

## Controller bootstrap

The QCA6390 answered over QUP6 UART and both firmware images were accepted:

```text
UMI BT: cold HCI=0x9 manufacturer=29 LMP subversion=0xd2b
UMI BT: QCA product=0x10 ROM=0x200 SoC=0x400a0200 patch=0xd2b
UMI BT: TLV type=1 accepted: 188564 bytes, 776 segments
UMI BT: TLV type=2 accepted: 5733 bytes, 24 segments
UMI BT: firmware running after reset: HCI=0xb LMP=0xb subversion=0x27ec
UMI BT: persistent H4 endpoint ready: com.hakios.bluetooth
```

## iOS integration

`bluetoothd` found the controller's local address and opened the H4 transport.
The driver then carried the initial iOS HCI sequence, including reset, local
version/features, buffer sizes, LE setup, advertising filters and connection
parameters.

```text
UMI BT: H4 client connected uid=501 unit=1
UMI BT: iOS HCI opcode=0x1001 length=4
UMI BT: iOS HCI opcode=0x1002 length=4
UMI BT: iOS HCI opcode=0x2003 length=4
UMI BT: iOS HCI opcode=0xc03
bluetooth: Got local-mac-address: ...
```

One vendor extension command returned a non-zero status:

```text
UMI BT: iOS HCI opcode=0xfd48 length=11
UMI BT: HCI command rejected opcode=0xfd48 status=0x1
```

The controller remained running and the standard iOS initialization continued.

## Remaining checks

- Scan for a nearby Bluetooth device.
- Pair and reconnect.
- Transfer a small file or use a Bluetooth audio device.
- Verify sleep/wake of the QCA6390 controller over a long session.
