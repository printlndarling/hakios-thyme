# Cellular

Where the modem stands (October 2026). The detailed, step-by-step diary in Russian is
`docs/ru/cellular.md`.

## What works

- The modem (the Hexagon MPSS) boots from XNU: its firmware goes through TrustZone, the shared
  memory, SMP2P, GLINK and QRTR come up, and it announces about 88 QMI services.
- Its file systems (modemst1, modemst2, fsc, fsg) are served from the UFS partitions over RMTFS,
  read-only: writes stay in kernel memory for the session, the phone's flash is never touched.
- TFTP over QRTR and the service registry (what Android's tqftpserv and pd-mapper do) are ours.
- The IPA packet engine runs: firmware through TrustZone, its clock through RPMh, both of its SMMU
  streams through a translation context of ours (the hypervisor takes one match register per
  stream), the registers Android programs, the modem's filter-table item in shared memory, and the
  SMP2P entry `ipa` the modem's driver waits for. The INIT_DRIVER handshake goes through.
- The modem goes online and stays up. The SIM is found (physical slot 2 on the first phone),
  logical slot 1 is pointed at it, the provisioning session is bound to its USIM, and NAS reads
  the signal (-70 to -87 dBm on UMTS).
- SpringBoard's status bar: the signal item is hooked (`userland/src/inject_sb.c`) and shows the
  kernel's bars (`sysctl kern.hakios.cellular_bars`) once the modem is registered.

- The data session: once NAS reports registration, the kernel binds its QMI WDS client to QMAP
  mux 1 of the embedded port (the IPA), sets the port's data format (WDA: raw IP in QMAP, no
  checksum offload), starts an IPv4 session on the SIM's default profile and reads its settings
  (address, gateway, DNS, MTU: `sysctl kern.hakios.cellular_ip`, `_gateway`, `_dns`, `_mtu`,
  `_data`).

## The data path (2.0)

Packets go through the IPA, the SoC's packet engine, the way Linux's `drivers/net/ipa` does it
for IPA 4.11; everything is in `drivers/modem/hakios.ipa.c` and `hakios.cell.c`.

- Before the modem starts, the kernel sets up this processor's side of the IPA: four GSI
  channels with their event rings (the command channel 5, the LAN receive channel 14, the modem
  transmit channel 2 and the modem receive channel 7), the matching endpoints (7, 9, 2, 16:
  QMAP headers, deaggregation on the way out, aggregation up to 12 KB or 500 us on the way in),
  and the IPA's filter, route and header tables through immediate commands, so that the modem
  finds valid tables and adds its own rules to them.
- The rings are polled by the interface's thread; no interrupt is taken from the GSI.
- The modem's packets are an Ethernet-looking interface, `en1`, with a made-up router behind it.
  The kernel answers ARP for every address and DHCP with the session's address, a /24 around it,
  the router and the DNS servers, so Darwin's configd configures `en1` as it would a USB Ethernet
  adapter and routes through it. IPv6 is not carried.
- The modem is told about this side the way ModemManager does for a phone behind the IPA
  (`net_driver == "ipa"`): **DPM Open Port** (Data Port Mapper) names the embedded port's IPA
  endpoints (the modem receives on our TX endpoint 2 and sends on our RX endpoint 16), then **WDA
  Set Data Format** sets raw IP in QMAP on it. Only then do the WDS steps run: **Bind Mux Data
  Port** (mux 1), **Set IP Family** (IPv4), **Start Network Interface** (with the APN) once NAS is
  registered, retried while the packet domain is still attaching. Skipping the DPM step was why
  WDA and the session answered InvalidOperation and the bind Internal (cycles 38-41): the modem
  had no embedded port to bind to.
- The rings are polled by the interface's thread; no interrupt is taken from the GSI.
- The modem's packets are an Ethernet-looking interface, `en1`, with a made-up router behind it.
  The kernel answers ARP for every address and DHCP with the session's address, a /24 around it,
  the router and the DNS servers, so Darwin's configd configures `en1` as it would a USB Ethernet
  adapter and routes through it. IPv6 is not carried.
- `modem_ipa_data_off=1` leaves all of this out; `modem_dpm_off=1` / `modem_wda_off=1` skip those
  steps; `modem_ipa_data_trace=1` logs every packet.

## Another phone: the SoC addresses

The modem and IPA drivers carry the POCO X5 Pro's register and memory addresses as defaults and
take any that differ from boot-args `soc_*` (`hakios.mpss.c` `soc_override()`, `hakios.ipa.c`).
`tools/soc-args.py` reads a phone's own device tree (`devices/<name>/pull-soc.sh` pulls
`/sys/firmware/fdt` over adb) and prints the `soc_*` it needs; the run scripts append
`devices/<name>/soc.args`. For the first phone every generated value equals the default, so it
needs no arguments.

## Not yet

- Registration reaches full LTE service, but the data session is still refused until the packet
  domain attaches; the DPM/WDA/bind order above is the current attempt, untested on hardware since
  the phone went off USB.
- Calls, SMS, IPv6.

## How to run it

```sh
XL_SB_DELAY=8 tools/modem-cycle.sh "modem=1 modem_again=1 modem_buffer=0xa8000000" 600
```

`modem_step=N` runs the bring-up step by step and returns to the loader with the log;
`modem=1` brings the modem up beside the system. Everything is in
`drivers/modem/hakios.mpss.c` (the modem), `hakios.nas.c` (NAS), `hakios.qmi.c` (QMI).

## Kernel arguments

| Argument | Meaning |
|---|---|
| `modem=1` | the modem beside the system, on a thread of its own |
| `modem_step=N` | steps 1..N, then back to the loader (`modem_return=1`) |
| `modem_again=1` | start it although a kernel before this one left it shut down |
| `modem_buffer=0xa8000000` | the RMTFS buffer (below 4 GB) |
| `modem_nas_wait=S` | how long step 11 waits for registration |
| `modem_online=0` | do not put the modem online |
| `modem_ipa=3` (default) | the SMP2P entry `ipa`: valid, clock on |
| `modem_sim_switch_off=1` | do not point logical slot 1 at the slot with the ready card |
| `modem_sim_probe_off=1` | no SIM and network-selection queries |
| `modem_data_off=1` | no data session at all |
| `modem_dpm_off=1` / `modem_wda_off=1` | skip the DPM / WDA step |
| `modem_data_nobind=1` | the session without binding the client to the mux |
| `modem_apn=NAME` | the session to that APN instead of the SIM's default profile |
| `modem_ipa_data_off=1` | no IPA data path for this processor, no `en1` |
| `modem_ipa_data_trace=1` | a log line per packet, command and DHCP/ARP answer |

## Thyme (the Mi 10S): no shared-memory partition with the modem (2026-11-10)

The driver is in this tree and builds for thyme (`devices/thyme/soc.args`), but its step 1
fails on the phone: `mpss: 1: there is no partition for us and the modem (0 and 1)`
(`logs/201955-run.log`). The table at the top of the shared memory says why
(`logs/203058-run.log`, `show_toc()`): it has 11 entries — 0/2, 0/5, 0/3, 2/3, 0/8, 7/11,
0/10, 10/5, the global one and two table entries — and **host 1 never appears**. The range
0x1000-0x1ff000 is packed with no gap, so a kernel-side partition cannot simply be added
to the 2 MB region either. This phone's boot chain therefore does not give the modem the
shared-memory partition the reference phone's driver expects, and step 1 (and every step
after it) cannot run. What to read next: the phone's own device tree (`/sys/firmware/fdt`:
the `qcom,smem` partitions property, `qcom,glink/modem`, `qcom,smp2p-modem`) to learn
whether this platform carries the AP-modem IPC in shared memory at all or over MHI/IPA
only, and which part of the boot chain creates the partitions that are there.

### The phone's own device tree answers it (2026-11-10)

`/sys/firmware/fdt` has no `qcom,glink/modem` child (its `qcom,glink` node has npu, adsp,
dsps, cdsp and spss only) and no `qcom,smp2p-modem` either: the AP-modem IPC of this
platform is not in shared memory at all, so the SMEM/GLINK driver the reference phone
taught the code cannot start here, and creating a partition would not help. The modem is a
separate die on **PCIe2 over MHI**: `/soc/qcom,pcie@1c10000` (parf 0x1c10000, phy
0x1c16000, dm_core 0x64000000, elbi 0x64000f20, iatu 0x64001000, conf 0x64100000; its
root port `pcie2_rp` holds `qcom,mhi@0`: `esoc-names = "mdm"`, `mhi,max-channels = 0x6f`,
`mhi,buffer-len = 0x8000`, SFR support, channels LOOPBACK, SAHARA, ...). The IPA is the
same one the tree's driver already runs (`qcom,ipa@1e00000`, `qcom,rmnet-ipa`), and the
`mhi-netdev0` alias points at `.../qcom,mhi@0/mhi_devices/mhi_rmnet@0`.

So cellular on thyme is not the reference driver's problem of one missing item: it needs
the PCIe2 link (PHY, clocks, power, link training), the MHI host stack over it (registers,
event rings, channels, BHI/SFR), ESOC to power and boot the modem (`mdm`), QRTR over the
MHI QRTR channel for the 88 QMI services, and then the existing QMI/NAS/WDS code on that
transport, with the IPA data path unchanged. The reference driver (`hakios.mpss.c`, SMEM,
GLINK, SMP2P) stays as it is for that phone; thyme needs the PCIe/MHI bring-up beside it.

### What Android's own boot shows of it (2026-11-10)

While Android runs, the endpoint is enumerated and its stack is visible: `0002:00:00.0` is the
PCIe2 root complex and `0002:01:00.0` the modem; `dmesg` has `msm_pcie_link_train: PCIe RC2
link initialized` and `pci 0002:01:00.0: 15.752 Gb/s ... 8 GT/s x2`, then `mhi 0002:01:00.0:
BAR 0: assigned [mem 0x64300000-0x64300fff 64bit]` and `Falling back to syfs fallback for:
sdx55m/sbl1.mbn` — the modem is an **SDX55M**, its first-stage image (`sbl1.mbn`) comes from
the AP over **Sahara**, and the MHI channels it then brings up are `QMI0`, `QMI1`, `SAHARA`,
`DIAG`, `LOOPBACK`, `IPCR`, `IP_HW0`, `IP_HW0_RSC`, `RMNET_CTL`, `DUN`, `EFS`, `ADSP_0..3`,
`SLPI_0..3`, `TF`, `QDSS` (their sysfs names under /sys/bus/mhi/devices). The same tree has
the Wi-Fi's `0000:01:00.0` on PCIe0 with the `cnss` MHI, which is the code this kernel already
drives. So the modem port is: PCIe2 RC (the Wi-Fi driver's sequence at 0x1c10000 with this
phone's PHY data), the standard Qualcomm MHI host on BAR0 0x64300000, Sahara of `sdx55m/*.mbn`
from the root image, then QRTR on QMI0/QMI1 and the existing QMI/NAS/WDS; the IPA data path
stays as it is. The GCC clock/reset offsets for PCIe2 are not in this repository and the
network is unreachable; the vendor kernel image on the phone (boot_a) carries them and is the
next thing to read.

### PCIe2's GCC registers, recovered from the vendor kernel (2026-11-10)

The clocks are not in any local table and the network is unreachable, so they were read out
of the phone's own kernel: with `kptr_restrict=2` lowered to 0 as root (Magisk), Android's
`/proc/kallsyms` gives `_text` (0xffffff9f70880000 that boot) and every `gcc_pcie_*` static
symbol; a symbol's file offset in the `boot_a` kernel Image is its address minus `_text`.
Dumping the `clk_branch` structs there (halt/enable fields at their offsets, `hw.init` at
+40, the vote register and its bit at +64/+68) gives, for PCIe2 at GCC:

- CBCR/halt registers: `slv_q2a` 0x6010, `slv_axi` 0x6014, `mstr_axi` 0x601c, `cfg_ahb`
  0x6024, `aux` 0x6028, `pipe` 0x602c (the PCIe0 block is the same shape at 0x6b010-0x6b02c,
  which is what the Wi-Fi driver already polls).
- The branch votes are at **GCC+0x52010** (PCIe0's at 0x52008), with bits `slv_q2a` 1<<10,
  `slv_axi` 1<<11, `mstr_axi` 1<<12, `cfg_ahb` 1<<13, `aux` 1<<14, `pipe` 1<<15 (all six
  OR to 0xfc00; the older comment in `hakios.thyme_pcie.c` that "0x52008 takes all PCIe0
  votes" is the same mechanism).
- The reference clock source (`aux_clk_src`, the `source_xo` register) is **GCC+0x6038**
  (PCIe0's 0x6b038).
- By the PCIe0 block's shape, PCIe2's GDSC status should be at 0x6004 and its core reset at
  0x6000; the PHY reset, which PCIe0 takes at 0x6c01c bit 0, still needs its PCIe2 bit.
  That, and the PHY sequence from the phone's DT (`qcom,pcie@1c10000`), are what a PCIe2
  link-up diagnostic needs next; the DT sequence is already in the pulled `dts`.

### PCIe2: clocks come up, the link attempt hangs until the modem is powered (2026-11-10)

The first link diagnostic (`pcie2_test=1`, `devices/thyme/drivers/modem/hakios.thyme_pcie2.c`,
built with `CONFIG_HAKIOS_THYME_MODEM_PCIE`) runs on a thread three seconds after
`hakios_goodix_init`, on the Wi-Fi PCIe0 sequence with the registers read out of the vendor
kernel. The first run reached `pcie2: power and clocks ready (GDSC 0xf822f000)`
(`logs/213357-run.log`) and then hung inside link training: the loader reported "hung" and
restarted the phone. The second, with the waits shortened and a print at each stage,
produced no log at all (`logs/214315-run.log`). Touching PARF/PHY/DBI while the SDX55M is
not powered and booted can stall the AP's interconnect, so the link sequence cannot be tried
before the modem's own power and boot control: the DT's `qcom,ext-sdx55m` esoc node
(`phandle 0x60`: `ap2mdm-status` GPIO 0x38, `mdm2ap-status` 0x03, errfatal 0x39/0x01,
pinctrl mdm_active/mdm_suspend, `qcom,sfr-query`) is what Android's esoc driver drives
before the pcie-msm probe, and that sequence is the next thing to port (its code is in the
vendor kernel image, like the clock tables were). Until then `pcie2_test=1` should not be
used on this phone.

### TLMM probing is not the way to the modem's lines (2026-11-10)

The read-only GPIO survey (`pcie2_test=1 pcie2_survey=1`) returns config 0 and input 0
for gpio1/3/56/57 at TLMM 0x0f000000 (`logs/220923-run.log`) — inconclusive, since the
register layout inside Kona's 16 MB pinctrl aperture and the tile each pin lives in are
unverified (the Wi-Fi's known-good base 0x0f900000 is the north tile). Reading the offsets
between tiles (0x0f200000 and up) stopped the phone's bus cold in two runs
(`logs/221637-run.log`, `logs/222527-run.log`: no log at all, and the loader restarted the
phone), so the survey now reads only the first tile and the map is not guessed at. The
ext-sdx55m driver's own sequence — which lines it drives, with what pin configuration, and
the status handshake before the PCIe link — is in the vendor kernel image like the clock
tables were, and is the next thing to read out; blind GPIO writes would be as unwise as the
blind reads were.

### The vendor esoc driver's power-on sequence, read out of its code (2026-11-10)

With the live kernel's `_text` and the pulled `boot_a` Image, `sdx55m_setup_hw`,
`mdm4x_do_first_power_on` and the ops tables were disassembled (llvm-mc over the bytes at
symbol-offset). The driver's GPIO table (12 sixteen-byte entries, each the DT property name
and the byte offset in its state struct) reads:

| property | struct offset |
|---|---|
| qcom,ap2mdm-errfatal-gpio | 0x24 |
| qcom,mdm2ap-status-gpio | 0x14 |
| qcom,ap2mdm-status-gpio | 0x2c |
| qcom,mdm2ap-pblrdy-gpio | 0x04 |
| qcom,ap2mdm-wakeup-gpio | 0x28 |
| qcom,ap2mdm-chnlrdy-gpio | 0x00 |
| qcom,mdm2ap-wakeup-gpio | 0x10 |
| qcom,ap2mdm-vddmin-gpio | 0x20 |
| qcom,mdm2ap-vddmin-gpio | 0x18 |
| qcom,ap2mdm-pmic-pwr-en-gpio | 0x30 |
| qcom,mdm-link-detect-gpio | 0x1c |
| (0x0d) | (unnamed) |

`mdm4x_do_first_power_on` (once, when the clink's flag is clear): calls the PON ops'
`toggle_soft_reset(mdm, 0)`, sleeps 150 ms, sets the **pblrdy** line (struct+4) to output
high, polls the **ap2mdm-wakeup** line (struct+40) up to 20 times at 5-6 ms, sleeps 200 ms,
and queues `esoc_clink_queue_request(1)` — the POWER_ON request engine. `sdx55m_setup_hw`
requires four of these lines non-negative (ap2mdm-errfatal, mdm2ap-status, ap2mdm-status,
mdm2ap-pblrdy) or its probe fails. **The phone's own DT defines only the errfatal pair and
the status pair** — no pblrdy, wakeup, chnlrdy, vddmin or pmic-pwr-en — so the ext-sdx55m
driver as read cannot be the whole story on this phone; Android's dmesg instead shows
`subsys-restart: Changing subsys fw_name to esoc0`, the MHI Sahara load of `sdx55m/sbl1.mbn`
and the modem's SSCTL service coming up. The next thing to read is that path (`mdm_probe`,
the esoc clink request engine, and what `subsys-restart` does with `esoc0`), not to drive
the lines the DT does not have.
