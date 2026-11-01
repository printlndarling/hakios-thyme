# Official-main Wi-Fi probe

Date: 2026-11-01

## Build

The device-owned QCA6390 stack builds as `CONFIG_HAKIOS_THYME_WIFI`:

```text
hakios.thyme_wifi.c
hakios.thyme_wlfw.c
hakios.thyme_lx.c
hakios.thyme_apple80211.c
hakios.thyme_qmi.c
hakios.thyme_qca6390.c
hakios.thyme_mhi.c
hakios.thyme_pcie.c
hakios.thyme_sta.c
hakios.thyme_wlan.cpp
```

Linux 7.0.14 `ath11k` and FreeBSD LinuxKPI are built separately by
`tools/mk-ath11k.sh` and linked through `xnu-build.sh`.

## Firmware and mission mode

The QCA6390 passed PCIe enumeration, SMMU setup, SBL/AMSS download and MHI
mission mode:

```text
UMI PCIe: firmware mission environment = 0x2
ath11k: fw_version 0x1016046e
lx: 294 ms after its start the driver IS UP as far as it is built:
    the firmware has said "unified ready"; its flags: 0xc100
```

The initial probe had no local address because the firmware reports
00:00:00:00:00:00. Supplying the Android `wlan0` address through `wifi_mac=`
created en0.

## Scan and association

The driver performed both 2.4 GHz and 5 GHz scans:

```text
lx: THE SCAN HEARD 13 NETWORKS ... 13 at 2.4 GHz, 0 at 5 GHz
lx: THE SCAN HEARD 21 NETWORKS ... 17 at 2.4 GHz, 4 at 5 GHz
```

iOS selected a WPA2-PSK network on 5 GHz channel 157 at -41 dBm:

```text
lx: iOS join reached the station driver: security 1, SSID length 4, key length 0
lx: ASSOCIATION: THE ACCESS POINT ACCEPTS (status 0); we are its station number 4
lx: THE KEYS ARE GIVEN TO THE DRIVER, 107 ms after the association
```

## DHCP and traffic

Configd and IPConfiguration brought up en0. DHCP assigned `192.168.0.103`, and
the kernel carried TCP traffic for multiple iOS processes over en0.

```text
net_test: dhcp: IPConfiguration says of "setservice en0 DHCP": 0 (done)
tcp connected: [192.168.0.103:49304<->113.142.189.21:443] interface: en0
tcp connected: [192.168.0.103:49306<->17.179.252.2:443] interface: en0
```

The link was intentionally brought down when the watchdog ended the RAM
session.

## Remaining checks

- Sustain a long transfer and verify reconnect after link loss.
- Test both 2.4 GHz and 5 GHz networks.
- Exercise AirDrop, AirPlay and other higher-level Apple services.
- Verify the radio's sleep/wake behavior with display sleep.
