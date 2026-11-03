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

## Scan diagnostics

`umi_bt_probe=5` performs LE passive scan and classic inquiry after firmware
boot. It logs each report's address, RSSI and advertised name (when present),
then shuts the controller down and returns to the loader.

The first physical run found nine unique LE advertisers at RSSI `-72` to
`-96 dBm`, including one named `DL02V1`; classic inquiry completed with no
classic peers present. See `official-bt-scan.md`.

## Remaining checks

- Run classic discovery with a known discoverable peer.
- Pair an LE peer from iOS and reconnect after disabling/re-enabling Bluetooth.
- Transfer a small file or use a Bluetooth audio device.
- Verify sleep/wake of the QCA6390 controller over a long session.

## Bluetooth audio HAL and A2DP route

The reduced RAM system image omits
`System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver`. Without it,
`AVAudioSessionCategoryOptionAllowBluetooth` and
`AVAudioSessionCategoryOptionAllowBluetoothA2DP` are rejected with
`OSStatus -50`, and the output route remains on `Speaker`.

The matching iOS 16.5 IPSW contains the signed vendor driver. Extract it locally
from the restore image into the Git-ignored `fw-bt-audio` directory:

```text
System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver
```

`userland/build.sh` copies it into `/redwood/hal`; `init` bind-mounts that
directory over the system HAL for both RAM-root and flash-root boots. The
Bluetooth launch set also includes `BTServer.le`, `BTServer.avrcp`,
`BTServer.map`, `BTServer.pbap` and `bluetoothuserd`.

On hardware, after `BTServer.le` and `BTAvrcp` started and the paired headset
connected, AVAudioSession reported:

```text
BluetoothA2DPOutput "..." (...-tacl)
```

AudioQueue accepted a three-second tone and handed over all `144000/144000`
frames while the same A2DP route stayed selected. The Bluetooth HAL and A2DP
routing are therefore present. That counter only shows API acceptance, not
real-time delivery: direct listening found continuous hoarse, crackling audio.
Audible A2DP quality is therefore FAIL.

The HCI UART is fixed at 115200 8N1 (about 11.5 KB/s), below normal encoded
A2DP throughput. This is the leading hypothesis for the corruption, but it has
not yet been proven by a same-headset A/B run. AVRCP commands, HFP/SCO,
microphone input and deliberate disconnect/reconnect still need validation.

Explicit Bluetooth category options still fail with `OSStatus -50`; Playback
without those options succeeds and the system remains on the selected A2DP
route.
