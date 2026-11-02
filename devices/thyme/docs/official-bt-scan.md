# THYME QCA6390 LE scan validation

Date: 2026-11-02

## Test build

```text
repo HEAD:       1f90331
kernel SHA-256:  e6a699afbf44938c5ea5b36dfd496cf9c42833790952386013b782028fafce2b
payload SHA-256: a3239a4789dc8a3172252976150b8c8e385dbcb965c1571583f4550b6cdd2a15
cmdline:
  umi_bt_probe=5 loader_return_after=90
```

The test used the standard RAM-only loader sequence:
`fastboot boot xnu-usb.img`, `stage`, and `oem run`. No partition was written.

## Results

QCA6390 firmware and NVM were accepted, followed by the expected post-reset
identity:

```text
UMI BT: TLV type=1 accepted: 188564 bytes, 776 segments
UMI BT: TLV type=2 accepted: 5733 bytes, 24 segments
UMI BT: firmware running after reset: HCI=0xb LMP=0xb subversion=0x27ec
```

Passive LE scan found nine unique advertisers:

```text
address types:  public and random
event types:    connectable and non-connectable advertising
RSSI range:     -72 to -96 dBm
named devices:  1 (`DL02V1`)
final counter:  LE passive scan reports=9 stop=0
```

Classic inquiry completed normally but found no discoverable classic peer:

```text
UMI BT: classic inquiry reports=0 complete=1 IBS wakes=8 sleeps=8
UMI BT: firmware/radio probe result=0
UMI BT: UART shutdown result=0
```

## Conclusion

- PASS: QCA6390 firmware boot, radio receive path, LE advertising report
  delivery, address/RSSI/name parsing, scan disable and orderly UART shutdown.
- PASS: classic inquiry command completes and reports completion.
- PENDING: classic discovery with a known peer, iOS pairing, reconnect after
  radio restart, and data/audio transfer.

The raw log contains addresses and is ignored in Git:

```text
logs/official-bt-scan-tail.log
```
