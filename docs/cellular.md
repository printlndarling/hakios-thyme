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

> Corrected on 2026-11-10 (property map below): this paragraph's offsets were
> misread. Struct+4 is AP2MDM_STATUS (set high), +0x28 is MDM2AP_PBLRDY (polled,
> and absent on this phone, so the poll is skipped), and the DT's four lines are
> exactly the set `sdx55m_setup_hw` requires.

### The vendor esoc stack's shape, and the tool that reads it (2026-11-10)

`tools/kdisas.py` turns a symbol of the phone's kernel into annotated disassembly: with the
live `/proc/kallsyms` (kptr_restrict 0) and `_text`, a symbol's offset in the pulled `boot_a`
Image is its address minus `_text`, the bytes go to `llvm-mc`, and every call and adrp/add
data address is resolved to the nearest symbol (and to its string when it is one). The
esoc/ext-sdx55m driver's function map it gives: `mdm_probe`, `mdm_pinctrl_init`,
`mdm_configure_ipc`, `mdm_status_fn` / `mdm2ap_status_check` / `mdm_status_change` /
`mdm_pblrdy_change` (the line interrupts), `mdm_handle_clink_evt`, the ops `mdm_cmd_exe`,
`mdm_get_status`, `mdm_get_err_fatal`, `mdm_notify`, the PON ops `mdm4x_do_first_power_on`,
`sdx55m_toggle_soft_reset`, `sdx55m_power_down`, and the subsystem side `esoc_ssr_probe`,
`mdm_subsys_powerup`, `mdm_subsys_shutdown`, `mdm_subsys_ramdumps`, `mdm_crash_shutdown`.
Android's dmesg names `subsys-restart` and `esoc0`, so `mdm_subsys_powerup` (with the clink
event handler beside it) is the path that actually brings this phone's modem up, and is what
the next read should cover; the `mdm4x_do_first_power_on` sequence recorded above is the
clink side of it.

### The pinctrl's per-pin registers are not reached by guessing either (2026-11-10)

`msm_gpio_get` shows how the vendor driver reads a line: it takes the pin's descriptor from
a table at `[pinctrl+848]`+0x20, stride 0x50, reads `regs_base + desc[0x28]` and takes the
bit `desc[0x40] & 0x1f`. The per-SoC data it comes from is `kona_pinctrl` (in the Image:
pins pointer at +0 = 0x1db36c0, 184 entries; groups pointer at +0x20 = 0x1db6ad0, 184;
functions at +0x10); the 0x50-stride descriptor table itself was not found — a scan of the
whole Image for 184 entries whose in_reg field steps by 0x1000 found nothing, so the table
is either built at runtime or its offsets are not the plain tile+pin*0x1000 shape. Android's
debugfs is closed as well (`/sys/kernel/debug` does not exist and cannot be created as root),
so the live states of gpio 1/3/56/57 cannot be read off Android either. The safe next read
is the table through the soc_data pointers above (or the pinctrl-msm group tables); blind
TLMM probing stays out, after the two bus hangs it caused.

### The TLMM tiles, and why the first survey read holes (2026-11-10)

The vendor `pinctrl-kona.c`'s `PINGROUP` table answers the question the Image scans
could not: Kona's TLMM is one 16 MB aperture (`reg = <0xf000000 0x1000000>`) whose
pins are grouped in three tiles, and a pin's page is **tile base + pin * 0x1000**:

| tile | base | pins |
|---|---|---|
| WEST | `+0x100000` | 133-179 |
| SOUTH | `+0x500000` | 0-132 |
| NORTH | `+0x900000` | 4-124 |

Each page is the mainline `msm_pinctrl` shape: CTL at `+0`, IO at `+4` (input bit 0,
output bit 1), interrupt config at `+8`, status at `+0xc`; CTL has mux bits 3:2, pull
bits 1:0, drive bits 8:6 and output-enable bit 9. The Wi-Fi driver's known-good pins
79-81 are NORTH (`0x0f900000 + pin * 0x1000`), which is how the table was checked.

All four `qcom,ext-sdx55m` lines are **SOUTH** pins, so their pages are:

| line | pin | page |
|---|---|---|
| mdm2ap-errfatal | 1 | `0x0f501000` |
| mdm2ap-status | 3 | `0x0f503000` |
| ap2mdm-status | 56 | `0x0f538000` |
| ap2mdm-errfatal | 57 | `0x0f539000` |

The first read-only survey (`pcie2_test=1 pcie2_survey=1`) read
`0x0f000000 + pin * 0x1000`, i.e. `0x0f001000`-`0x0f003000`: those addresses are in
the hole **below** the WEST tile, so its `config 0, input 0` described nothing. The
two later reads that stopped the bus (`0x0f200000` and up) were in the hole between
WEST (`0x100000`-`0x1b2fff`) and SOUTH (`0x500000`-`0x584fff`) — unmapped addresses,
which is exactly the hazard the note warned about. The tile map is now in the driver
(`hakios.thyme_pcie2.c`, `TLMM_PAGE`) and its TLMM mapping covers the south tile;
`pcie2_survey=1` reads only the four SOUTH pages above.

### The south-tile survey on the phone, and the esoc driver's real property map (2026-11-10)

The corrected read-only survey ran on the phone (`logs/234257-run.log`, kernel
`9249aff1`, image `8f5bc2aa`, args `pcie2_test=1 pcie2_survey=1`) and the four
south pages answered without touching the link:

```text
pcie2: gpio1 at 0xf501000: config 0x1 (mux 0, pull 1, drive 0, oe 0), io 0 (in 0, out 0)
pcie2: gpio3 at 0xf503000: config 0x1 (mux 0, pull 1, drive 0, oe 0), io 0 (in 0, out 0)
pcie2: gpio56 at 0xf538000: config 0x1 (mux 0, pull 1, drive 0, oe 0), io 0 (in 0, out 0)
pcie2: gpio57 at 0xf539000: config 0x1 (mux 0, pull 1, drive 0, oe 0), io 0 (in 0, out 0)
```

All four pads are at the reset default (GPIO function, pull-down, 2 mA, input),
not at the DT's `mdm_active` state: the loader does not apply that pinctrl, so a
RAM boot sees the lines undriven and MDM2AP_STATUS low. The tile map is confirmed.
`logs/234257-run.log` SHA-256 `1031bd722da5718760d130a33c41020a0d4c9b62135f327d76292514ede1768e`.

The same disassembly also corrects the earlier property/offset table. Reading the
strings each `gpio_request` names gives `mdm_configure_ipc`'s real map:

| property | struct offset | direction |
|---|---|---|
| qcom,ap2mdm-wakeup-gpio | +0x00 | optional (skipped when absent) |
| qcom,ap2mdm-status-gpio | +0x04 | **output low** |
| qcom,ap2mdm-chnlrdy-gpio | +0x10 | optional, output low |
| qcom,ap2mdm-errfatal-gpio | +0x14 | **output low** |
| qcom,mdm2ap-errfatal-gpio | +0x24 | **input**, its IRQ ("mdm errfatal", wake) |
| qcom,mdm2ap-pblrdy-gpio | +0x28 | optional, polled |
| qcom,mdm2ap-status-gpio | +0x2c | **input** |

`sdx55m_setup_hw` requires exactly the four the phone's DT has (1, 3, 56, 57); the
earlier note that the DT was missing a required `pblrdy` came from a misread table.
`mdm4x_do_first_power_on` then reads: PMIC soft reset, 150 ms, AP2MDM_STATUS high,
poll MDM2AP_PBLRDY up to 20 times at 5-6 ms (the whole poll is skipped when the
property is absent, as here), 200 ms, then the POWER_ON request.

The soft reset is not a GPIO: `sdx55m_toggle_soft_reset` calls
`qpnp_pon_modem_pwr_off`, which writes the PMIC's PON block through regmap
(offset+99 = 0, a delay, offset+98 = value; and the 0x80 / 0xa5 arm-and-fire
pair at offset+99 / +100). That is an SPMI register path XNU does not have yet.
The next XNU experiment is therefore lines-only: apply the DT's `mdm_active` pad
configuration to 56/57 (mux 0, bias-disable, 16 mA) and 1/3 (mux 0, bias-disable,
8 mA), drive AP2MDM_STATUS high, read MDM2AP_STATUS, and only then consider the
PCIe2 link -- with the PMIC reset as the separate question it is.

### The lines-only wake: the pad writes take, the modem does not answer (2026-11-10)

`pcie2_power=1` applies the DT's `mdm_active` pad state and raises AP2MDM_STATUS
without touching any PCIe register (`logs/235616-run.log`, kernel `371a184e`, image
`8f5bc2aa`, SHA-256 `6738a4e16e683c3d077871f0abc21d6e9a383c4bed3667becc716b802cab8934`):

```text
pcie2: lines configured (56/57 out low); before the wake: status 0, errfatal 0
pcie2: AP2MDM_STATUS high; MDM2AP_STATUS stays low, MDM2AP_ERRFATAL 0, waited 0 ms
pcie2: after the pause: status 0, errfatal 0; gpio56 ctl 0x3c3 io 0x3, gpio57 ctl 0x3c3, gpio1 io 0, gpio3 io 0
```

The writes take: gpio56/57 read back `ctl 0x3c3` (mux gpio, bias off, 16 mA, output
enabled) and gpio56's IO reads `0x3` (latch and pad high). The modem does not answer:
MDM2AP_STATUS and MDM2AP_ERRFATAL stay low for the whole two-second poll. So the
status line alone does not wake the SDX55M; the vendor's sequence starts with the PMIC
PON modem power-off pulse (`qpnp_pon_modem_pwr_off`, register pair 0x62/0x63), which
XNU has no write path for yet. That is the next piece.

Test-setup note: a boot without the 2.5 GB resident system image panics in launchd
(`errno 2`) at about 18 s (`logs/235222-run.log`); it is not a modem effect. With the
image resident the kernel stayed up to 172 s.

### The pin-descriptor table search: what fits and what was a false positive (2026-11-10)

`msm_gpio_set` confirms the descriptor layout: `desc = table(gpio * 0x50)`, the register
offset at `desc+0x28` (address `regs_base + that`), and the bit in `desc+0x40` bits 9:5
(the 64-bit load at +0x3c shifted by 37). Two whole-Image scans for a 184-entry, 0x50-stride
table then produced candidates that do not survive checking: one run near 0x1d36628 gives
gpio56..80 offsets that step by 0x1000 (0x52900c, 0x52a00c, ... 0x54000c) but places
gpio79 at 0x540000 where the Wi-Fi driver's known-good TLMM write for pin 79 uses
0x0f900000 + 79*0x1000 = 0x0f927000 — a different page, so that table is something else;
another run at 0x1d3837c has one constant word across every entry, which no real descriptor
has. The true `msm_pin_data` table is therefore still unlocated. The way not to repeat this:
derive the table's layout from `msm_pinctrl_probe` (which fills it), or read the vendor
pinctrl-msm's group/function tables directly; pin-order guessing has now cost three attempts
and two bus hangs, and is stopped.

### The pin tables, finally: the addresses were off by 0x80000 (2026-11-11)

`msm_pinctrl_probe` reads `soc = pctrl+848` (its second argument) and then
`soc+0x10` / `soc+0x18` for a 0x18-stride function table, so the
`struct msm_pinctrl_soc_data` is `{pins, npins, functions, nfunctions, groups,
ngroups}`. In the phone's Image the symbol `kona_pinctrl` is at file 0x1d33668, and
its fields point at pins 0x1d336c0, functions 0x1d35770 (stride 0x18) and groups
0x1d36ad0 (stride 0x50, 184 entries). The earlier reads used 0x1db36c0/0x1d6ad0 --
the field values with an extra 0x80000 -- which is why nothing matched. The group
entries are the `struct msm_pingroup` that `msm_gpio_get/set` indexes by GPIO number:

```text
+0x00 name    +0x08 pins    +0x10 npins   +0x18 funcs   +0x20 nfuncs (10)
+0x24 ctl_reg +0x28 io_reg  +0x2c intr_cfg +0x30 intr_status +0x34 intr_target
+0x3c bitfields: mux 2 (3 bits at bit 2), pull 0 (bit 0), drive 6 (bit 6),
                 oe 9, in 0, out 1
```

The exact registers (TLMM base 0x0f000000, from the group entries, cross-checked
with `pinctrl-kona.c`: SOUTH=+0x500000, NORTH=+0x900000, ctl = base + pin*0x1000):

| line | GPIO | CTL | IO |
|---|---|---|---|
| MDM2AP_ERRFATAL | 1 | 0x0f501000 | 0x0f501004 |
| MDM2AP_STATUS | 3 | 0x0f503000 | 0x0f503004 |
| AP2MDM_STATUS | 56 | 0x0f538000 | 0x0f538004 |
| AP2MDM_ERRFATAL | 57 | 0x0f539000 | 0x0f539004 |
| PCIE2 PERST | 85 | 0x0f555000 | 0x0f555004 |
| PCIE2 WAKE | 87 | 0x0f557000 | 0x0f557004 |

(gpio84 is a NORTH pin at 0x0f954000; gpio85-88 are back in SOUTH. The earlier
"first survey" read 0x0f001000 etc., which is not any pin's page.)

### The vendor pci-msm source, and what the PCIe2 bring-up was missing (2026-11-11)

The CAF SM8250 tree (`UtsavBalar1231/kernel_xiaomi_sm8250`, android14-stable) has
`drivers/pci/controller/pci-msm.c`, the driver the phone's DT's `qcom,pci-msm`
binds. `qcom,boot-option = <1>` is `MSM_PCIE_NO_PROBE_ENUMERATION`: probe does not
train the link; something later (the sysfs `enumerate` file, or the client) does.
`msm_pcie_enable` is:

1. assert PERST -- `perst-gpio` GPIO 85, active low (the info table's on=0);
2. regulators (vreg-1p8, vreg-0p9), GDSC, bus vote, clocks, resets;
3. PARF: DEVICE_TYPE=4, PM_CTRL bit 5 cleared, PHY_CTRL bit 0 cleared,
   DBI_BASE_ADDR=0, SYS_CTRL=0x365E, MHI_CLOCK_RESET_CTRL bit 4 set,
   INT_ALL_MASK=0, SLV_ADDR_SPACE_SIZE (DT 0x4000000), AXI_MSTR_WR_ADDR_HALT bit
   31 set;
4. PHY: write the DT `qcom,phy-sequence`, 1 ms, enable the pipe clock, then poll
   `PHY + 0xa14` (qcom,phy-status-offset) bit 6 clear -- set means not ready;
5. wait `qcom,ep-latency` (10 ms), release PERST, 1 ms;
6. `msm_pcie_link_train`: 0x80c field 0x1f00=1, 0x8a8 |= 0x20, 0x890 bit 0 cleared,
   0x8bc bit 0 set, 0x154 = 0x55555555, 0x8bc bit 0 cleared, LNKCTL2 speed only if
   the DT says so, 0x78 field 0x7000 = 5 (TLP read size), PARF+0x1b0 bit 8 set;
   poll ELBI+0x08 (0x64000f28) bit 10 (XMLH_LINK_UP) 20 x 5 ms, then `dm_core+0x80`
   bit 29 and `dm_core+0` != 0xffffffff;
7. program iATU region 0 (at 0x64001000) for the CONF window to BDF 1:0.0, type
   0x04 (config type 0), enable with bit 31, then read the endpoint.

The clocks, from the phone kernel's `clk_branch` data: the per-branch enable is
GCC+0x52010 with the masks slv_q2a 0x400, slv_axi 0x800, mstr_axi 0x1000, cfg_ahb
0x2000, aux 0x4000, pipe 0x8000; the PHY refgen branch is GCC+0x6f034 bit 0 (its
halt bit is bit 31), its source GCC+0x6f014 (19.2 MHz XO); the DT's two resets are
GCC_PCIE_2_BCR = GCC+0x6000 (binding 14) and GCC_PCIE_2_PHY_BCR = GCC+0x1f01c
(binding 17), confirmed by `gcc_kona_resets` in the Image. The old driver was
missing PERST entirely, polled 0x814 bit 6 set instead of 0xa14 bit 6 clear, never
enabled the refgen or toggled the PHY reset, forced Gen2, and wrote 0x78/0x8a8 as if
the fields were whole-register values; `hakios.thyme_pcie2.c` now follows the vendor
sequence.

### The PCIe2 link trains, and the SDX55M answers on it (2026-11-11)

Kernel `983385a3` (log `logs/002609-run.log`) produced the first working PCIe2
session:

```text
pcie2: clocks: GDSC 0xf822f000, refgen 0x1, PHY reset 0
pcie2: PARF programmed; PARF+0x350 0, +0x358 0x4000000
pcie2: PHY ready (0xa6); releasing PERST
pcie2: link requests done; ELBI 0x1, link status 0x10110008
pcie2: the link is up; RC id 0x10b17cb, link status 0xb0230008, ELBI 0x11401
pcie2: bus numbers 0x10100
pcie2: the endpoint says id 0x30617cb (vendor 0x17cb, device 0x306), class 0xff000000, BAR0 0x4
pcie2: MHI: BAR0 0x64300004 (was 0x4, next 0), command 0x100002, reglen 0x100
pcie2: MHI: cfg 0x800080 (channels 128, events 128), chdb 0x300, erdb 0x700
pcie2: MHI: control 0, status 0x7025062f
```

Three pieces were missing. The PHY would not clear bit 6 of 0xa14 (`0x61`)
until the **MDM reference clock gate** was enabled: the DT's `pcie_mdm_clkref`
is `GCC_PCIE_MDM_CLKREF_EN` (binding 71), i.e. GCC+0x8c00c bit 0 -- the
vendor's RC2 clock table enables it (required) and it is the clock the modem's
side of the link runs from. With it on, the same sequence reported 0xa6 (bit
6 clear) within 100 ms. The other branches the vendor's table enables were
added with it: GCC+0x9000c (TBU), GCC+0x8d058 (DDRSS SF TBU), and the PHY
refgen source/branch at 0x6f014/0x6f034.

The endpoint's config space then still read all ones, and the reason is the
vendor's addressing, not the link: `msm_pcie_oper_conf` never uses an ECAM
address. `msm_pcie_cfg_bdf()` re-points iATU region 0 at the wanted BDF, so
the CONF resource's **first 4 KB are** that device's config space and a
register is read at `CONF + offset`. `CONF + (1 << 20)` is outside the 4 KB
iATU window and answers 0xffffffff. The driver now reads CONF + 0 / + 8 /
+ 0x10, and the RC's type-1 header gets its secondary bus number (1) written
at DBI+0x18 first, as Linux's `pci_scan_bridge` does.

The endpoint answers as `17cb:0306`, class `0xff000000` -- exactly Android's
`0002:01:00.0`. Its BAR0 is a 64-bit memory BAR (read 0x4, unassigned); the
driver gives it the phone's own 0x64300000, turns memory decoding on, and
identity-maps the window through iATU region 1. MHI then answers: MHICFG
0x800080 (128 channels, 128 events), CHDBOFF 0x300, ERDBOFF 0x700. MHICTRL
reads 0 (the host has not enabled MHI) and MHISTATUS 0x7025062f; the next
peek reads BHIOFF/BHIEOFF and the boot interface's EXECENV, which say whether
the SDX55M sits in its PBL waiting for sbl1.mbn (the image is on the phone at
/vendor/firmware_mnt/image/sdx55m/sbl1.mbn, 548,056 bytes).

### The BHI doorbell, and the bus master bit that unlocks it (2026-11-11)

The SDX55M's PBL answers at the MHI boot interface (BAR0 + BHIOFF 0x100):
version 1.2, BHI_EXECENV 0 -- with the identity EE table that is
`MHI_EE_PBL`, so the modem has no firmware and waits for `sdx55m/sbl1.mbn`
(the file is on the phone at /vendor/firmware_mnt/image/sdx55m/sbl1.mbn,
548,056 bytes, md5 d53e8d4a685e49be2f9a433501db5250; the CAF MHI driver's
table maps dev_id 0x306 to it). The scene root now carries a copy at
/redwood/fw/modem/sbl1.mbn (userland/root/fw/modem, packed with
tools/apfs/mkapfs.py into userland/root.apfs; both the firmware and the
images are git-ignored).

`pcie2_bhi=N` in hakios.thyme_pcie2.c reads that file into physically
contiguous kernel memory, flushes it from the cache, programs BHI_INTVEC,
BHI_STATUS = 0, BHI_IMGADDR (high and low), BHI_IMGSIZE, and rings
BHI_IMGTXDB, then polls BHI_STATUS's top two bits.

The first attempts looked like the PBL was ignoring the doorbell: the
registers read back exactly as written (IMGTXDB 0x1234567, image address,
size) but BHI_STATUS stayed 0. What was missing was the endpoint's **bus
master enable**. The driver had set only PCI command bit 1 (memory space);
the vendor's `pci_set_master` also sets bit 2, and the BHI engine's reads of
host memory are the endpoint's DMA. With command 0x100006 the very next run
produced a transfer attempt:

```text
pcie2: MHI: BAR0 0x64300004 (was 0x4, next 0), command 0x100006, reglen 0x100
pcie2: BHI: version 1.2, execenv 0, status 0, errcode 0, imgaddr 0/0, imgsize 0, numseg 0x3
pcie2: BHI: wrote imgaddr 0x1/0x94214000 size 0x85cd8; read back ..., txdb 0x1234567, status 0
pcie2: BHI: status 0xffffffff (result 3), errcode 0xef120700,
       dbg 0x94214000 0x1 0x85cd8, execenv 0, MHISTATUS 0 after 10 ms
```

Result 3 is BHI_STATUS_ERROR, and the debug registers hold the address and
size the engine was given. That image buffer was at physical 0x194214000 --
above 4 GB -- while the vendor hands the device IOVAs from the DT's
`qcom,iommu-dma-addr-pool` (0x20000000, 512 MB); the next attempt allocates
the image below 4 GB. (kmem_alloc_contig's arguments are
`(map, &addr, size, mask, max_pnum, pnum_mask, ...)`; putting the bound in
the pnum_mask slot asks for an alignment, not a limit, which is why the
first "below 4 GB" attempt returned KERN_NO_SPACE.)

### The modem's SID, the apps SMMU, and a doorbell that is answered but refuses (2026-11-11)

The BHI error was first blamed on the endpoint's DMA being refused. The apps
SMMU at 0x15000000, read only, says: SCR0 0x2d0406 (unmatched streams fault),
87 match registers, and SMR 0 -- stream 0, mask 0 -- with S2CR 0. The RC's
BDF-to-SID table (PARF+0x2000) was never programmed, so the endpoint's DMAs
leave with the default stream and the SMMU has no bypass for it.

Writing SMR 0's S2CR to change that took the phone down with no log at all
(two runs, not one line of kernel output); the boot chain's own match
register is not to be touched. What does work: filling the whole BDF-to-SID
table with BDF 1:0.0 (0x0100) -> the table's 8-bit offset 1 (the DT's
0x1d01 minus `qcom,smmu-sid-base` 0x1d00), so whichever slot the CRC hash
picks holds the right entry, and taking *free* match registers only (5 on
this boot) for the streams the table can produce.

That the table works is proven by pointing its stream at a context that
faults: with SMR 5 = stream 0x1d01 and S2CR 0 (translate into context 0,
which is off) the SMMU's context-0 fault register records FSR 0x400 -- a
translation fault -- so the endpoint's reads do arrive with the SID from the
table. (The faulting address in CB0 FAR does not match the image, so the
recorded fault is not certainly the BHI's.)

With the stream sent to a bypass SMR (S2CR 0x20000, the value the SLPI's
verified FastRPC bypass uses -- though the Arm SMMU-500's spec calls TYPE 2
a *fault*, and this SMMU reads back 2 for a written 1) and with the image
byte-perfect in host memory (word sum 0x92f1f39, the file's own), in the
SLPI heap at 0xc0000000 and in ordinary kernel pages above 4 GB alike, the
PBL still answers BHI_STATUS 0xffffffff with BHI_ERRCODE 0xef120700 within
10 ms, and the SMMU records no global fault. The next step is the path
Android actually uses: a stage-1 context of the apps SMMU with page tables
of ours -- the machinery `radio_space_on()` in drivers/wifi/hakios.wifi.c
already uses for the radio's stream (0x1c01) -- giving the modem an IOVA
from the DT's `qcom,iommu-dma-addr-pool` that translates to the image.

### What the probes after that settled, and what is still open (2026-11-11)

Three more runs narrowed it. A stage-1 context of the apps SMMU with page
tables of ours -- the Android path, the machinery `radio_space_on()` in
drivers/wifi/hakios.wifi.c uses for the radio -- maps IOVA 0x20000000 (the
DT's `qcom,iommu-dma-addr-pool`) to the image and sends the endpoint's two
possible streams through it: the BHI answer is unchanged (0xef120700), so a
translated IOVA is no better than a bypassed physical address or the SLPI
heap. A type-2 S2CR on the endpoint's stream, with the SMMU's global fault
register cleared just before the doorbell, leaves SGFSR at 0 -- whether that
type is this SMMU's bypass or a fault whose record we did not reach. And
shortening the post-PMIC-reset pauses to the vendor's own scale (150 ms to
AP2MDM_STATUS, then milliseconds) changed nothing either.

So the PBL answers the doorbell, records the image address and size it was
given, and refuses with 0xef120700 whatever memory, translation or timing
precedes it; the image is byte-perfect. What remains is a validation inside
the PBL: its own secure-boot check of the SBL, or a BHI protocol detail
(the BHI reports version 1.2 and BHI_NUMSEG 3; the SBL ELF has 17 program
headers, 14 of them loadable, so NUMSEG is not the ELF's segment count).
Both need a working reference -- the vendor kernel's own MHI debug log from
a successful SBL load would do -- which this phone's Android no longer
offers (no debugfs, no way to restart the modem from userspace). The open
question and every register-level fact above are here for the next attempt.

### The endpoint the bootloader left behind, and the PBL's trust registers (2026-11-11)

Two probes are prepared for the next modem run.

**`pcie2_keep=1`** leaves PERST alone (and suppresses the PMIC warm reset) so
the endpoint keeps whatever execution environment the phone's bootloader left
it in. The default path already says the modem was running under Android when
this kernel was booted, and the bootloader that boots this kernel may have
booted the modem too -- in which case the endpoint sits in AMSS with its MHI
ready, and asserting PERST (what the vendor's enable sequence does) sends it
back to the PBL for no reason. The survey's `BHI_EXECENV` and MHI status say
which environment it really is in. If it is AMSS, the host side needs MHI
bring-up, not the BHI load.

**The PBL's identity and trust registers** are now printed before the
doorbell: `BHI_SERIALNU` (0x40), `BHI_SBLANTIROLLVER` (0x44), `BHI_NUMSEG`
(0x48), the four `BHI_MSMHWID` words (0x4c-0x58) and the eight
`BHI_OEMPKHASH` words (0x64-0x80). The last is the PBL's root: an all-zero
hash is an unfused (open) part and would let any image through, while a
nonzero one is the key the SBL's certificate chain has to match. The phone's
own Android boots this same modem with this same `sbl1.mbn`, so on a fused
part the image *must* be able to pass -- if the fused hash is nonzero, the
0xef120700 refusal is not an image-authentication failure and the search
stays on the host side of the doorbell.

### The bootloader leaves the modem in PBL, and the part is fused (2026-11-11)

The keep-PERST probe ran (`logs/082114-run.log`, kernel `4258b7dc`, image
`4f3cf17e`, `pcie2_test=1` + the keep argument + `pcie2_bhi=1 pcie2_img=1`,
SHA-256 `79f559ddce5cc2b281d7368579c2f7a5ff4f5870af893c07487a732ee13ad9b0`).
With PERST and the PMIC reset suppressed the link still trains, so the endpoint
is alive without the vendor's reset; the endpoint answers `17cb:0306` again with
MHISTATUS 0. The BHI reads:

```text
pcie2: BHI: version 1.2, execenv 0, status 0, errcode 0, imgaddr 0/0, imgsize 0, numseg 0x3
pcie2: BHI: serial 0x9721197d, antirollback 0, numseg 0x3, HWID 0 0xe50e1 0 0
pcie2: BHI: OEM PK hash 0x56ee0ed4 0x654619f3 0xa3094157 0x4a726792 0x344194e7 0x76cb53cd 0x3c3d297e 0x55794940
```

`EXECENV 0` is PBL: the bootloader does **not** leave a running AMSS behind, so
the BHI load is the right path (there is no MHI bring-up to join). The OEM PK
hash is nonzero -- the part is fused -- and the SBL is the same image Android
boots, so authentication is not what 0xef120700 is about. The load attempt
fails exactly as before (dbg1 = the image address, dbg3 = its size).

The SMMU still records a context-0 translation fault (`CB0 FSR 0x400`) from the
boot chain's fault-type SMR, i.e. the doorbell's DMA is still not verifiably
reaching host memory; the next run sends a deliberately wrong image without
reading any file (`pcie2_bhi_bad=1`, from the SLPI heap) -- if the error code is
unchanged the PBL rejects the transfer before the image's content matters, and
if it changes the content is being read.

### The zero image is refused exactly like the real one (2026-11-11)

That run is in: `pcie2_bhi=2` + the bad-image flag + `pcie2_img=1`
(`logs/082821-run.log`, kernel `90ce0739`, image `4f3cf17e`, SHA-256
`52d290e7ba88c4ca6a153b69c3d1a4bb41333e79c97bd768f47c6e75f292e8df`). A 4 KB
zeroed image from the SLPI heap, with no file read at all, is refused in 10 ms
with **the same** `BHI_STATUS` 0xffffffff / result 3 and `BHI_ERRCODE`
0xef120700; only the debug registers follow the image (address `0xc0000000`,
size `0x1000`). So the refusal is not about the image's content: the PBL never
gets to read it.

The SMMU says why. In that run the boot chain's match register 0 is still
stream 0, mask 0, S2CR 0 -- translation into **context 0, which is switched
off** -- and the BDF-to-SID table at PARF+0x2000 was not programmed, so the
endpoint's reads leave as SID 0, match SMR 0, and fault there (CB0 FSR 0x400,
SGFSR 0 because the stream matched an SMR before the unmatched-stream policy
could apply). A doorbell whose DMA faults reads nothing, and the PBL reports a
transfer error whatever the image is.

The next runs program the table (`pcie2_sid=2`) and read it back, and then give
the stream a stage-1 context (`pcie2_sid=5`); the context's own FSR, TTBR0,
TCR and SCTLR are printed after the doorbell now, and a match register that
already carries one of the two streams is repointed at the context instead of
being duplicated (first match wins, so a duplicate would never be reached).

### The table and the context: no refusal and no fault, but no completion either (2026-11-11)

With the BDF-to-SID table written and read back 256/256 (`logs/083456-run.log`,
SHA-256 `917269e313244a0803e6e7eee331ecf8f0c680256d9d690be57feb247eb13872`) the
doorbell still answers 0xef120700 in 10 ms and SGFSR stays 0: no unmatched
stream ever reaches the SMMU, i.e. the reads still arrive as SID 0 (matched by
the boot chain's SMR 0 and faulting in its off context 0) or not at all.

With a stage-1 context behind the table as well (`pcie2_sid=5`, the zero image,
`logs/083705-run.log`, SHA-256 `07ccaa63020373bb3fd4ec5b1923e5b2be2b5199ad7685b47f1c80a245e41b8c`)
the doorbell **stops erroring at once**: BHI_STATUS stays 0 (no result) for the
whole two-second poll, and the context the stream now goes through reads:

```text
pcie2: SMMU: context 3 (page 0x1000, 128 pages) tables ..., SCTLR 0x127, streams 2
pcie2: BHI: wrote imgaddr 0/0x20000000 size 0x1000; ... status 0
pcie2: BHI: status 0 (result 0), errcode 0xef120700 (stale), dbg ... after 2000 ms
pcie2: SMMU: our context 3: FSR 0x400 FAR 0xfeeee4b9, ...
pcie2: apps SMMU: SMR 5 stream 0x1d01 mask 0 s2cr 0x3 (kind 0 context 3)
```

The real SBL through the same context (`logs/083903-run.log`, SHA-256
`389547238bb68fcc76a1ff5eba0e6060aaba9103698705ba216fdbc0f218c1f0`) repeats it
with SMR 5 repointed to context 5 (the new path): status 0 for two seconds, and
context 5's FSR 0x400.

The FSR is **not a fault**: in the SMMU-500's CBF FSR the fault bits are
`MULTI|SS|UUT|EF|PF|AFF|TF` = `0xc000011e`, and 0x400 is a FORMAT bit. The last
run cleared and re-read the context's FSR immediately before and 10 ms after
the doorbell and it did not change (`logs/084236-run.log`, SHA-256
`920a67cf74ea18b162bcda870312b901fe2c653fd53482c507821bf091ab5f95`). So the
table plus the context stop the instant refusal without introducing a fault:
the BHI leaves status 0, which is either a transfer still in progress after
two seconds or no transfer at all. The next probe polls for ten seconds and
logs the status, EXECENV and MHI status once a second, to tell those apart.

That run (`logs/084639-run.log`, SHA-256
`10ce4580019d2a217ea3bbfcea8f8f1ef6342af86744461b7b0e85cce2dd998a`) answered a
different question first: the **BHI registers still hold the previous run's
image**. The survey line at the start of the boot reads `imgaddr
0/0x20000000, imgsize 0x85cd8` and the previous errcode before this run wrote
anything. So neither the PMIC pulse (`qpnp_pon_modem_pwr_off`, PON 0x862/0x863)
nor PERST resets the modem's boot block, and the doorbell that follows is
issued into an engine still holding the last attempt: status stays 0 at 1, 2
and 5 s, EXECENV 0, MHISTATUS 0. A modem test therefore needs a cold modem
state -- the phone's reboot into Android, whose modem boot resets it -- or a
reset whose effect is actually observed, before the BHI results mean anything.
Verifying the PON pulse (read 0x862/0x863 back, watch MDM2AP_STATUS) is the
next step, and the doorbell's status semantics will be read again only after
that.

### A cold modem refuses the translated IOVA too -- and the MHI windows were the image's, not the pool (2026-11-11)

The first run after a reboot (a cold modem: the BHI survey reads `imgaddr 0/0,
status 0, errcode 0` before the driver writes) repeats the refusal with the
table and the stage-1 context in place (`logs/085402-run.log`, SHA-256
`4dedb066d54bb40388ef75eb33957e8452462428f3e5c1b85aab49424f1f6372`):

```text
pcie2: SMMU: context 3 (page 0x1000, 128 pages) tables ..., SCTLR 0x127, streams 2
pcie2: SMMU: before the doorbell: our context 3 FSR 0x400 (fault bits 0) ...
pcie2: BHI: wrote imgaddr 0/0x20000000 size 0x85cd8; ... status 0
pcie2: BHI: status 0xffffffff (result 3), errcode 0xef120700, dbg 0x20000000 0 0x85cd8, ... after 0 ms
pcie2: apps SMMU: SMR 5 stream 0x1d01 mask 0 s2cr 0x3 (kind 0 context 3)
```

Immediate (0 ms), the same code, and no SMMU fault. So the earlier status-0
runs were the stale state, and a fresh PBL refuses **every** image address --
physical below 4 GB, the SLPI heap, and an IOVA that a stage-1 context of ours
maps -- in the same way.

Comparing the driver with the vendor's `mhi_init_mmio` found one thing that was
not faithful: the four MHI window registers are the **DMA pool**, not the image
range. `MHICTRLBASE/LIMIT` (0x80/0x88 and 0x84/0x8c) and
`MHIDATABASE/LIMIT` (0x98/0xa0 and 0x9c/0xa4) take the DT's
`mhi_0_iommu_group` pool, `qcom,iommu-dma-addr-pool = <0x20000000 0x1fffffff>`
(i.e. 0x20000000..0x3ffffffe), and the image is placed inside it; the driver
had written the image's own [addr, addr+size-1]. It now programs the pool on
both windows and logs them, and the PON block's registers are read back right
after the modem pulse and 150 ms later, so the reset's effect (or its absence)
is visible instead of assumed.

That run is in too (`logs/090020-run.log`, kernel `90de7063`, cold modem,
SHA-256 `b421227e7e25147477263850942289942faa4260cbed8fb488c4b4b6744fb265`):
the windows read back as the pool (`base 0x20000000 stop 0x3ffffffe, image at
0x20000000 size 548056`) and the PBL still answers result 3 / `0xef120700` in
0 ms with the image inside them. The PON block reads all zeros right after the
pulse and 150 ms later -- the write either self-clears as a pulse or never
lands, so the reset's effect is still unproven.

What is left to try on the host side of the doorbell: the MHI context base
registers (`CCABAP_HIGHER/LOWER`, `ECABAP_*`, `CRCBAP_*`) which the vendor's
`mhi_init_mmio` writes before the SBL load and this driver never has, and an
explicit `MHICTRL` reset (`mhi_set_mhi_state(MHI_STATE_RESET)` then waiting for
the bit to clear) before the BHI registers are touched. Both are in the next
cold-modem run.

### The context bases and an explicit reset change nothing (2026-11-11)

That run (`logs/090737-run.log`, kernel `16883c86`, cold modem, SHA-256
`365dd4279473bc33e191023406f7ca5f6905cf638e444d24d9418e7b20130c68`): the six
context-base registers are written (the image's IOVA stands in for all three
until there are real MHI contexts), `MHICTRL`'s reset bit is set -- and **the
device never clears it** (`reset bit did not clear (control 0x2, status 0)`),
which is consistent with the vendor only issuing that reset in the SYSERR path.
The doorbell is then refused exactly as before: result 3 / `0xef120700` in
0 ms with the image at `0x20000000`.

So the host-side MHI register state is now as faithful as the vendor's up to
the SBL load, and the PBL still refuses. The remaining possibilities are that
the BHI engine's reads of host memory are being dropped before they are
translated (the RC's inbound path, or a SID the SMMU never sees), or a PBL-side
check that needs a reference the AP cannot provide. The modem work pauses here
with the facts recorded; the next hardware slot goes to the sensor bring-up's
prepared probe, and the modem returns with a new measurement (an RC-side
inbound/DMA trace) rather than another variation of the same doorbell.

### The SMMU's extended-SID bit was never set (2026-11-11)

One host-side difference remains between this kernel and Android, and it is in
the apps SMMU's global configuration: `SCR0` reads `0x2d0406`, and bit 3 --
`sCR0_EXIDENABLE`, the extended (16-bit) stream-ID mode -- is **clear**. The
boot chain leaves the SMMU in the 8-bit mode; Android's `arm-smmu` driver sets
the bit when it initializes the block, because this SMMU reports 16-bit SIDs
and the DT's streams (`0x1d01` for the modem, `0x1c01` for the Wi-Fi) need
them. XNU never programs it. The Wi-Fi's stream setup happens to work anyway
(its SMR's low byte matches after the truncation), which is why the radio
never showed the difference.

`pcie2_exid=1` now sets `SCR0` bit 3 before the match registers are
reprogrammed, so the driver uses the wide encodings: SMR = 16-bit ID and mask
(no bit-31 valid), S2CR = `EXIDVALID` (bit 10) | type | context, with the full
`0x1d01`/`1` streams. The next cold run is `pcie2_test=1 pcie2_bhi=2
pcie2_img=1 pcie2_sid=5 pcie2_exid=1`; a change in the doorbell's answer (or
in what the SMMU records) tells whether the truncation was the wall.

That run is in (`logs/095542-run.log`, kernel `3c861adf`, cold modem,
SHA-256 `29670876464b6af60a1b241020c1b049f5d04092674122f91110b36f3541187e`):
`EXIDENABLE set` was logged, but the register **reads back `0x2d0406` with bit
3 still clear** -- the global SMMU configuration is not writable from this
non-secure kernel (the Wi-Fi driver's comment already called the global
registers the hypervisor's to grant). The doorbell is unchanged: result 3 /
`0xef120700` in 0 ms with the context and the wide encodings written into
match register 5. So the stream-ID width is not something the AP can change,
and the wall is not there either.

### The root complex's AER view, and a probe that was running all along (2026-11-12)

The 9:12 run (`logs/091212-run.log`, kernel `22f39ebe`, `pcie2_test=1
pcie2_bhi=1 pcie2_img=1`) appeared to stop between the MHI survey and the
doorbell: no file load, no refusal, no SMMU line. The probe had in fact run --
the scene it booted carried no `/redwood/fw/modem/sbl1.mbn` (the 11-11 images
had it, a later root rebuild did not), so the driver sat in its 30 s retry
loop and then said so. That line fell into the one part of the kernel log a
long run cannot keep: the console log is a 256 KB linear head followed by a
1 MB ring (`osfmk/arm64/loader_return.c`), so a run longer than about a minute
returns the boot's start and only the most recent megabyte, and everything in
between is overwritten. `userland/build.sh` now stages
`../fw-modem-sdx55m/sbl1.mbn` into `root/fw/modem/` whenever it is present, so
a scene, not a hand copy, carries the firmware.

The rerun did run the load: scene `out/thyme-boot/root-modemprobe.apfs`
(`a85da21b`, the shipped job list), same kernel, `logs/093611-run.log`. The
image read back byte-identical (`first words 0x464c457f 0x10101, word sum
0x92f1f39`), the doorbell registers read back as written, and the PBL refused
again in 0 ms: `BHI_STATUS 0xffffffff` (result 3), `0xef120700`, dbg
`0xc0000000 0 0x85cd8`. The new reads:

```text
pcie2: PARF: int status 0x2000, status 0xb40, core errors 0
pcie2: AER: uncorr 0 corr 0x2000 root 0 src 0
pcie2: SMMU: SGFSR 0, SGFSYNR0 0, SGFSYNR1 0 (the stream), CB0 FSR 0x400 FAR 0xd55bee94
```

`PARF` shows link-up and nothing else. The correctable status's bit 13 is
*Advisory Non-Fatal Error* -- the bit the AER spec uses for an uncorrectable
error (unsupported request, completer abort, completion timeout) that is
masked and marked non-fatal -- while the uncorrectable status itself reads 0.
The registers were not cleared before the doorbell, though, and Android boots
this modem constantly, so the reading may be the boot chain's. The driver now
writes 1 to the three status registers (RW1C) before the doorbell, prints the
read-back, and after the doorbell prints the uncorrectable status with its
mask and severity, the correctable mask, and the header log -- so the next run
separates this transfer's errors from an earlier boot's.

That run is in (`logs/094256-run.log`, kernel `22e8487d`, same scene):

```text
pcie2: AER: before the doorbell: uncorr 0 corr 0 root 0
pcie2: BHI: status 0xffffffff (result 3), errcode 0xef120700, dbg 0xc0000000 0 0x85cd8, execenv 0, MHISTATUS 0 after 0 ms
pcie2: PARF: int status 0x2000, status 0xb40, core errors 0
pcie2: AER: uncorr 0 (mask 0x400000, severity 0x462030), corr 0x2000 (mask 0xe000), root 0 src 0
pcie2: AER: header log 0xa0 0 0 0
```

The clear landed -- the pre-doorbell read is zero -- and the same advisory bit
is set again after the doorbell: the event is this transfer's, not the boot
chain's. The uncorrectable status still reads 0 while the correctable
status's Advisory Non-Fatal bit stands, so the root complex saw an
uncorrectable-class error on a non-posted request and downgraded it; the
header log carries no usable TLP. The SMMU's global fault status stays 0 and
no context fault appears, and while that is not conclusive (a secure fault
need not be visible here) it points at the root complex's inbound path rather
than at a translation fault. The next probe (`pcie2_inb=1`) turns iATU region
2 into an inbound window over the image's range: the vendor driver programs
no inbound region, and that is the one host-side difference left between this
path and a working one.

That probe is in (`logs/094949-run.log`, kernel `c89211cb`, `pcie2_inb=1`):
region 2 went in as ctrl1 `0x1`, ctrl2 `0x80000000`, `0xc0000000..0xc07fffff ->
0xc0000000`, and the doorbell's answer did not move -- result 3 / `0xef120700`
in 0 ms, the AER advisory bit set again. So the inbound translation is not the
wall either. What no probe has read yet is the fault state of the contexts the
boot chain left behind: the modem's stream matches an SMR the boot chain made
(SMR 5, stream `0x1d01`, S2CR translate into context 3, whose tables are
Android's). A read translated there would fault in *that* context's FSR, not
in SGFSR and not in context 0's. The driver now prints every enabled
context's SCTLR, FSR and FAR after the doorbell; the next run reads them.

That run is in (`logs/095518-run.log`, kernel `1c3c98c2`): every context
reads `FSR 0x400` -- bit 10, outside the SMMU's fault bits (`FSR_FAULT` is
`0xc00001fe` in the vendor's own `drivers/iommu/arm-smmu-regs.h`) -- and only
context 3 is enabled (`SCTLR 0x127`): the one the boot chain left behind with
Android's tables. So no context records a fault for the refused read. The
BDF-to-SID entry format is explicit in the vendor's `msm_pcie_config_sid`
(`drivers/pci/controller/pci-msm.c`: `BDF [31:16] | SID [15:8] | NEXT [7:0]`),
so the boot chain's `0x01000100` is BDF 1:0.0 -> SID `0x1d00 + 1 = 0x1d01`,
and `0x1d01` matches SMR 5 -- the S2CR type is 0, translate, into context 3
(`S2CR_TYPE_TRANS` = 0 in the vendor's header, so the driver's old note that
SMR 0 was "type FAULT" misread the encoding). Whether context 3's tables
actually map the address the PBL is given is now a table walk: its `TTBR0` and
`TCR` are readable, and the next probe resolves the image's IOVA through them
-- if it is mapped, the read reached memory and what refuses is the PBL; if it
is not, the SMMU refused the translation without a record this kernel can see.

That walk is in (`logs/101633-run.log`, kernel `0fb8d134`): context 3 reads
`TTBR0 0x300019395c000`, `TCR 0x803519` (39-bit, 4 KB grain), and the level-1
entries for `0xc0000000` (entry 3) and `0x20000000` (entry 0) are both zero --
both addresses are unmapped, so a read translated by this context would fault.
No context's FSR records anything, and nor does the secure global fault
register. Together the two readings say the modem's read does not traverse
context 3 at all: the SMMU is not in this read's path, and the refusal is not
a translation fault. What the PBL reads at `0xc0000000` is then the image
itself (or nothing, if it never reads), and the remaining question narrows to
whether the doorbell's machinery touches host memory at all.

`pcie2_imgsz=N` was added to hand the PBL a size other than the file's, and a
run with N=0 answers that (`logs/102248-run.log`, kernel `c3d7cf78`): with
nothing to read the doorbell is answered in 0 ms with a *different* error --
`0xef0e0002` instead of `0xef120700`, dbg `0xc0000000 0 0` -- and the AER
correctable status stays **0**, no advisory event. The file's size is refused
with `0xef120700` and the advisory bit set. So the doorbell and its argument
check are accepted; the advisory event is the PBL's read of host memory
failing, and the two codes are two different refusals -- size 0 an argument
error, a real size the read.

The read fails at the root complex even though no SMMU context translates it,
so the question is now which addresses the endpoint can read at all.
`pcie2_addr=N` was added; the next run reads `0x80000000`, the start of DRAM,
with the same file. An advisory event there too would mean the endpoint's
reads fail wherever they go; none would mean the read completes elsewhere and
`0xc0000000`, the SLPI's carveout, is refused specifically.

That run is in (`logs/102908-run.log`, kernel `5ad487a9`,
`pcie2_addr=0x80000000`): the same `0xef120700` in 0 ms, the AER advisory bit
set again, and the walk shows `0x80000000` unmapped in context 3 as well
(level 2 entry 0). So when no SMMU context translates the read, the endpoint's
read fails wherever it is pointed -- the failure is in this path, not in the
SLPI's carveout. What has not been measured with the AER yet is the
*translated* read: `pcie2_sid=5`, where the driver builds a stage-1 context,
maps the image into it and hands the PBL an IOVA. The next run does that with
the walk and the AER read in place, which says both whether our context is the
one that matches and whether its translation makes the read complete.

That run (`logs/103343-run.log`, kernel `5ad487a9`, `pcie2_sid=5`) did report
the refusal -- `0xef120700` with IOVA `0x20000000`, the AER advisory bit set
again -- but stopped printing after the AER line: its diagnostics map the SMMU
window a second time, and this kernel's `io_map` allocates from a 32 MB
`io_submap` shared by every driver with `KMA_NOFAIL`, so a full submap blocks
in `kmem_alloc` instead of failing. The modem driver's own regions had taken
ten of those megabytes for the whole TLMM aperture (it touches five pin
pages), plus the SLPI heap's eight. The SMMU window is now mapped once
(`smmu_window()`) and the TLMM entries are the five pages, so the next sid=5
run can print the walk of our own context and the AER under its translation.

That run is in (`logs/104158-run.log`, kernel `e2728cce`): the driver's own
context 5 has `TTBR0 0x50001a8890000`, and the walk resolves `0x20000000` to
`physical 0xc0000000` (`page 0xc0000e43`) -- the translation it built is
exactly the image. The stream table now prints, and it is
`SMR 0 stream 0` (translate, context 0) and `SMR 6 stream 0x1d01` (translate,
context 5): our SMR is the one that matches the modem's stream, and the boot
chain left none of its own for it. And yet the doorbell is refused in 0 ms
with `0xef120700` and the AER advisory bit is set, with a valid translation
in place. So the read does not complete even when the SMMU maps it correctly:
what refuses the PBL's reads is on this PCIe2 path itself, not in the image's
address or its translation. The next run points the same doorbell at
`0xf8000000`, ordinary kernel DRAM below 4 GB and outside every carveout:
the advisory bit there would say the path refuses every read, and its absence
would say the read completes in normal DRAM and the carveouts are what this
master may not touch.

That run is in (`logs/104658-run.log`, kernel `e2728cce`,
`pcie2_addr=0xf8000000`): the advisory bit is set there too. So every read
fails -- in the SLPI's heap, at the start of DRAM, and in ordinary kernel DRAM
below 4 GB alike. With no translation in the path for any of them, what is
left is the apps SMMU's own policy: an unmatched stream is a fault (SCR0 bit
10), and the fault would be recorded -- but the driver has been reading the
*secure* global fault status. Qualcomm's split puts the non-secure aliases at
+0x400 (the vendor's `arm-smmu.c`: "nsCR0: 0x400, nsGFSR: 0x448,
nsGFSYNR0: 0x450"), so the record of an unmatched modem stream would have
been sitting in registers no run has read. The driver now clears nsGFSR
before the doorbell and prints it with nsGFSYNR0-2 after it; the next run says
whether the modem's traffic matches an SMR at all, and if it does not, names
the stream ID the RC actually attaches.

That run is in (`logs/105425-run.log`, kernel `077bdba3`): the non-secure
GFSR reads 0 and so do GFSYNR0-2 -- and the aliases are inactive here
anyway: SCR0 at 0x400 reads 0 while 0x0 reads 0x2d0406, so the ordinary
offsets the driver reads are the non-secure ones. There is no fault record
anywhere: not in a context, not in a global status. The SMMU never sees the
modem's read at all.

The wifi driver's own bring-up says why. Its `umi_pcie_enable_dma` notes that
"SM8250 defaults to BDF-to-SID bypass. A table alone does not route requests
through the owned context; explicitly enable translation" and clears bit 0 of
`PARF + 0x2c00`, then writes 6 to the root port's command register (`DBI + 4`,
"allow protected endpoint transactions upstream") before setting the
endpoint's bus master. The modem driver had neither write: no run ever
un-bypassed the BDF-to-SID table or enabled the root port's upstream path.
Both are now part of the sid path, with the configuration register printed
before and after, and the next run repeats `pcie2_sid=5` with them.

That run is in (`logs/110112-run.log`, kernel `ab052a6f`, `pcie2_sid=5`): the
root port command reads `0x100006` (memory and bus master) after the write,
the BDF-to-SID configuration reads 0 before the update, and the doorbell's
answer changes -- `0xef0e0003` after **30 ms**, dbg `0x20000000 0 0x85cd8`,
where every earlier run got `0xef120700` in 0 ms. The AER advisory bit is
**clear**: no error at the root complex. The endpoint's read is being
forwarded now; the refusal has moved from "the read does not complete" to a
different, later one -- `0xef0e` is the class of the size-0 argument refusal
(`0xef0e0002`), so the PBL is now judging what it received, not failing to
receive it. The next run repeats the doorbell without `pcie2_sid`, at the
physical address: whether it keeps the new answer says whether the
translation matters at all.

The plain run with the same fix is in (`logs/110531-run.log`): it keeps
`0xef120700`, but takes **80 ms** now and its AER advisory bit is clear too.
The physical read of the SLPI heap is forwarded and fails later, in the
fabric, without a port-level error. Two paths, two later refusals -- the
physical one keeps `0xef120700`, the translated one answers `0xef0e0003` --
so what the PBL reports depends on what it received. The next run places the
image in kernel memory (`pcie2_img=2`) and points the doorbell at its
physical address: if a plain DRAM read completes, its errcode says what a
successful read yields, and the heap's `0xef120700` is then about the heap
and not about the image.

That run is in (`logs/111100-run.log`, `pcie2_img=2`): the image sits at
`0x1_93eac000` in ordinary kernel DRAM and the doorbell is answered with the
same `0xef120700`, after 70 ms, no AER. So the physical reads fail the same
way in DRAM as in the heap -- what this master may not do is read host memory
untranslated at all -- and the translated read is the one that gets further.
The next run puts the image in kernel memory and sends it through the driver's
own context (`pcie2_img=2 pcie2_sid=5`): the Android shape of the transfer,
a DMA buffer behind the SMMU, none of it in a carveout.

That run is in (`logs/112011-run.log`, `pcie2_img=2 pcie2_sid=5`): the walk
resolves the IOVA to `0x193cac000`, the kernel buffer itself, and the doorbell
answers `0xef0e0003` after 30 ms with no AER -- the same answer the heap's
translated read gave. So the answer follows the *path*, not the memory:
translated reads, whatever they point at, get `0xef0e0003` in about 30 ms;
physical ones get `0xef120700` in 70-80 ms.

That pattern has a simple reading: the PBL's reads may bypass the SMMU
entirely, so `0x20000000` is read as a *physical* address -- ordinary DRAM
below 4 GB, readable, holding whatever was there and not the image -- and the
PBL then rejects the content with a code of the same class as the argument
refusal (`0xef0e...`), while a read of the heap or of DRAM above 4 GB fails to
deliver at all (`0xef12...`). The discriminating run points the doorbell at
`0x20000000` with no SMMU context: a bypassed DMA should repeat `0xef0e0003`,
and that settles both questions -- the SMMU is not in the modem's boot path,
and `0xef0e0003` is what the PBL says about data it has read.

It did: `logs/112602-run.log`, `pcie2_addr=0x20000000`, no SMMU context,
answers `0xef0e0003` in 30 ms with no AER -- the translated run's answer
exactly. The SMMU is not in the modem's boot path: the IOVA was read as a
physical address in ordinary, readable DRAM, and the PBL rejected what it
found. The pattern is now complete:

- readable physical DRAM below 4 GB -> a transfer happens, then a content
  refusal (`0xef0e0003`);
- the SLPI heap at `0xc0000000`, or DRAM above 4 GB -> no transfer
  (`0xef120700` after 70-80 ms).

So the image must sit in DRAM below 4 GB that this master can read.
`pcie2_img=3` allocates the buffer with `max_pnum` = `0x40000` (4 GB at 16 KB
pages, exclusive), and the next run points the doorbell at it.

That run got nothing: `kmem_alloc_contig` could not find 1 MB below 4 GB
("no 1 MB of contiguous kernel memory" twice, `logs/113653-run.log`), and
fell back to the heap. `pcie2_img=4` instead puts the image in 576 KB of the
kernel's own BSS: the kernel image is loaded below 4 GB, so that buffer's
physical address is too, and the loader has already shown the range is usable
DRAM. The next run points the doorbell at it -- the first time the PBL is
handed the image itself in memory this master may read.

That run (`logs/114320-run.log`, `pcie2_img=4`) answers `0xef120700` after
70 ms with no AER: the kernel's own BSS at `0xf7f94000` is readable to the AP
but not to the endpoint, so the readable window is narrower than "below
4 GB". `pcie2_img=5` copies the image into `0x20000000` itself, the one
physical window the PBL's reads have been seen to deliver from, through an
uncached mapping. Its first run (`logs/114942-run.log`) never reached the
doorbell -- the thread stopped between the MHI survey and the image copy while
the rest of the run went on. A log line now separates the mapping from the
file load, so the next run says which of the two stalls.

That run is in (`logs/115612-run.log`): the mapping succeeded ("0x20000000
mapped at 0x62b60000") and the file load into it hung -- an uncached write to
that address stalls the writing CPU while the rest of the system runs on. So
`0x20000000` answers the PBL's reads but does not take the AP's writes: a
read-only window, not a buffer, and the image cannot be placed there this
way. How far the readable window reaches is the next question; `0x80000000`
is being read with the root port enabled, which its earlier run predates.

It is not readable either (`logs/120100-run.log`: `0xef120700`, 80 ms, no
AER), so the window is not "DRAM below 4 GB" but the region around
`0x20000000` -- the DT's `qcom,iommu-dma-addr-pool` base, likely the one
window the boot chain's access control leaves open to this master. The AP's
plain IO write into it stalls (non-posted), so `pcie2_img=5` now maps it
write-combining: posted writes do not wait for the acknowledgement that never
comes, and if the window is DRAM behind the RC's access control the image
lands.

It does not (`logs/120618-run.log`): the load into `0x20000000` hangs the same
way. The DSB after the writes waits for an acknowledgement that never comes,
posted or not, so that window answers reads and not writes -- it is not a
place the image can be put. What remains for the next attempt: whether another
address inside the pool (`0x30000000`, say) answers reads and takes writes,
or a way to bring the apps SMMU into the path, since the translated read
bypassed it exactly as the physical ones do.

The inbound iATU window was also retried on the fixed root port
(`logs/123204-run.log`, `pcie2_inb=1`): region 2 enabled over
`0xc0000000..0xc07fffff` and the heap read still answers `0xef120700` after
80 ms. So the window the RC offers is not the mechanism either: the refusing
party is above both the iATU and the SMMU -- the SID-based access control the
boot chain leaves for this endpoint, which only lets it read the pool region
around `0x20000000`. Android's own MHI must reach memory through a
configuration this kernel has not reproduced; the next diagnostic is a
register-level comparison of the two RCs (PCIe0, whose endpoint's DMA works,
against PCIe2) rather than another variation on the doorbell.

That comparison is in (`logs/124133-run.log`): PCIe0's and PCIe2's routing
registers are identical -- SID configuration 0, AXI master write halt
`0x80000014`, slave address space `0x4000000`, root port command `0x100006` --
and the one difference is the BDF-to-SID table's entry for the endpoint's own
BDF. PCIe0's hash slot holds `0x01000100` (BDF 1:0.0, SID offset 1 of
`0x1c00`); PCIe2's holds `0x60040d47`, whose BDF field is `0x6004` -- a stale
entry the boot chain left, not this endpoint's. The SMMU dump names the other
side of the same fact: SMR 5 is stream `0x1c01` (the wifi's) with S2CR type
translate into context 3, and nothing matches the modem's `0x1d01` outside a
run that programs the table itself.

Both facts reshape the reading. The PBL's own window check -- its image must
lie inside the MHIDATA window this driver programs, `0x20000000`-`0x3ffffffe`
-- explains the two refusal classes without either proving the read
delivered: an address outside the window is `0xef120700`, one inside it is
`0xef0e0003`. `pcie2_img=5` now takes `pcie2_addr` as its target, so the
write test can move to `0x30000000`, inside the window and away from its
first page.

`0x30000000` stalls exactly as `0x20000000` does (`logs/124702-run.log`,
"loading the file into it" and nothing more): the pool window is not writable
from the AP at any offset, so it cannot carry the image. The discriminator
that remains is whether the PBL judges *content* at all: with the image
zeroed in place and the same address and size (`pcie2_bhi_bad=1=2`) through
the driver's own SMMU context, a different error code than the real image's
would mean the read delivers and the content is checked; the same code would
mean the refusal is about the transfer or the address.

That run (`logs/125116-run.log`) answers `0xef0e0003` for the zeroed image,
exactly the real image's answer: at that address the PBL does not distinguish
content, or the read never delivers. And the fault-type probe
(`pcie2_sid=6`) hung the kernel instead of answering
(`logs/125606-run.log.live`), twice, while plain runs on the same kernel pass.
The free-space counter says why it was *not* the shared `io_submap`: 24.5 MB
of it were free at the probe's start (`logs/131406-run.log`), so the block
was not there, and the loader's periodic fetch means the last logged line
does not place the hang. The heap window now maps `MODEM_IMAGE_MAP` (640 KB,
all the image needs) regardless, and the fault probe is rerun with the
free-space line so a third hang can be placed.

The content discriminator, run with the argument's real name
(`logs/133940-run.log`, `pcie2_bhi_bad=2 pcie2_sid=5`), changes the reading:
the zeroed image is refused with `0xef120700` in **0 ms**, while the real
image through the same translation answers `0xef0e0003` in 30 ms. Content
changes the answer -- so the BDF table and the driver's match register are in
effect, the SMMU translates this traffic, the PBL reads the image, and the
two codes mean different things: a fast `0xef120700` is "there is no image
here" (zeros where the ELF header should be), and `0xef0e0003` is the
verdict on the real one after reading it. The physical reads' 70-80 ms
`0xef120700` is that first code from the other direction -- no transfer at
all. (An earlier run with the wrong argument name had reported the zeroed
image answering like the real one; nothing had been zeroed.)

The wall has moved to the image's validation. The next probe flips one byte
at offset `0x8000` of the real image (`pcie2_bhi_bad=4`): if the verdict
stays `0xef0e0003`, that part of the image is not authenticated and the
refusal is about something else; if it changes, the PBL's signature check is
the wall and the file in the scene is not the image the phone's fuse
expects.

It changes: one flipped byte at `0x8000` makes the verdict `0xef0a0105` in
10 ms (`logs/134522-run.log`, `pcie2_bhi_bad=4 pcie2_sid=5`). Three codes now
name three stages -- a header that is not an image is `0xef120700` (0 ms), an
image whose body was altered is `0xef0a0105` (10 ms), and the phone's own
untouched image is `0xef0e0003` (30 ms). So the real image *passes* the PBL's
integrity check; the transfer, the SMMU translation and the signature all
work, and `0xef0e0003` is a refusal after them. What the PBL validates next
is the remaining question; the next run hands it the ELF's own extent
(`0x83f9c`, where the last program header ends) instead of the file's size
`0x85cd8`, in case the size is what it judges.

It does judge it, and the file's own size is the right one: `0x83f9c` is
refused with `0xef0e0002` in 0 ms (`logs/135000-run.log`), the same code a
zero size gets, while `0x85cd8` passes to the next stage. So the image, its
size and its signature are all as the PBL expects, and `0xef0e0003` is a
refusal *after* all three -- the last stage this kernel can vary by
changing the image is now closed, and what the PBL validates beyond it is
either the state it is handed (the MHI registers, the doorbell protocol) or
something inside the SBL's start that a host cannot see.

The MHICTRL reset was the next suspect: the driver's write left the reset bit
set (the device never cleared it), while the vendor's pre-load path never
touches MHICTRL. With the handshake skipped (`logs/140444-run.log`, MHICTRL
back to 0 as the boot chain left it) the verdict does not move: `0xef0e0003`
in 30 ms again. The other pre-load difference is next -- the driver also
writes the three context bases and the MHI windows before the doorbell, which
the vendor writes only after the SBL has run; `pcie2_mhiraw=1` leaves them
alone, so the doorbell is rung with the exact register state Android rings it
with.

That run too is in (`logs/141022-run.log`): the same `0xef0e0003` in 30 ms.
The phone's own device tree, carved out of the vendor_boot image that was
pulled before the install work
(`backups/thyme-20261103-before-install/vendor_boot_a.img`), confirms every
parameter this driver took from the reference: `qcom,smmu-sid-base = <0x1d00>`
with `iommu-map` BDF `0x0100` -> SID `0x1d01`, `qcom,iommu-dma = "fastmap"`
over the pool `0x20000000 0x1fffffff` with a coherent pagetable (so the MHI's
DMA *is* translated, as this driver now serves it), `qcom,boot-option = <1>`
(no probe-time enumeration), and **no sbl-size property at all**, so Android
hands the PBL the file's own size as well. Everything a non-secure host can
set now matches.

The cellular thread stops here for the moment: image, size, signature,
address, SMMU translation and the pre-load MHI state all match Android's, and
the refusal lies beyond them -- in the secure world the phone's own esoc
stack shares with TZ, or inside the SBL's start, neither of which this kernel
can observe. Picking it up again would mean reverse-engineering the secure
side of the modem's boot, a project of its own.

### The last unread function, and the closed surface (2026-11-20)

`mdm_subsys_powerup`, which the 2026-11-10 notes left as "what the next read
should cover", is orchestration only (0xffffff801119f640): it fetches the
clink data, calls the esoc ops' function pointer with the power-on state
(w0 = 1) -- the same `mdm4x_do_first_power_on` / `qpnp_pon_modem_pwr_off(1)`
sequence already replicated -- stores the ESOC state, and waits on
completions with retries.  Its sibling `mdm_cmd_exe` handles the clink
requests: cmd 3 selects the `mdm_active` pinctrl state, cmd 5 raises the
status line (struct +0x14) and sleeps, cmd 7 drops it; no code path there
writes any register, strap or line beyond the four DT GPIOs, their pinctrl
states and the PON block the port already drives.

With that, the AP-visible surface is closed by evidence on every side: the
stock kernel's only pre-doorbell register writes are the five BHI writes
(`mhi_init_mmio` runs only from `mhi_ready_state_transition`, i.e. after
READY; `mhi_init_dev_ctxt` allocates host rings only), the probe sends
exactly those (`logs/171914-run.log`) and the answer stays `0xef0e0003` in
30 ms.  The remaining judgement is the modem's own: its PBL authenticates
the phone's own `sbl1.mbn` and refuses the ELF placement, and no host state
reaches that decision.  The warm-handover side door does not exist either:
the modem reaches AMSS only under Android's kernel, and the fastboot/loader
path the port boots through resets it back to the PBL before the probe's
first read (`execenv 0` with no PON of ours, `logs/211817-run.log`).

### External evidence: the same modem boots under another XNU port (2026-11-20)

The register surface above is closed; the modem is not.  An independent HakiOS
port for the Xiaomi Mi 10 (umi) -- `github.com/xm6661885/hakios-xnu-kernel-umi-fixes`,
pushed 2026-11-16 -- carries a complete host-loaded SDX55M stack in
`devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_modem.c` and its
`umi_modem_{pcie,phy,mhi,power,dma,data,net,fat,selftest}.inc`: BHI upload of
the 548,056-byte `sdx55m/sbl1.mbn`, SBL's Sahara over MHI channels 2/3 serving
the 17-image map (`tools/modem-sahara.map`: apps 6, qdsp6sw 8, EFS 16/17/20,
sbl1 21, aop 23, tz 25, acdb 29, hyp 33, mdmddr 34, multi_image 36/37,
xbl_cfg 38, apdp 40, devcfg 41, sec 42; 114,843,743 bytes staged), then AMSS,
IPCR/QRTR node 3, mission EFS/TFTP and IP_HW0/QMAP.  Its `MODEM-MAINLINE.md`
records the recipe verified first on mainline Linux (Debian 13, ModemManager
1.24): registration, IPv4 and SMS send/receive.  `COMMCENTER-umi.md` (v25)
records the XNU run: LTE registered, data on `pdp_ip0` (tx 867 / rx 1079 in
91 s) and an MT SMS delivered to imagent; the SMS banner is a separate
userland issue (`MessagesBlastDoorService`, `NOTIFICATIONS-umi.md`).  None of
this is reproduced here, and the umi endpoint is the same `17cb:0306` with
BAR0 `0x64300000`.

Consequences for this phone.  The PBL accepts an SBL from a non-Apple host on
this modem family, so `0xef0e0003` is a fault in what our transfer delivered
or in its mapping, not a modem-wide refusal -- the "closed by evidence"
conclusion above holds for the register surface only.  Thyme's own constants
are in `github.com/yuweiyuan8/linux-xiaomi-thyme-mainline` (Linux 7.2):
`sm8250-xiaomi-thyme-modem.dtsi` gives `qcom,ext-sdx55m` with GPIOs 1/3/56/57
and the MHI iommu group (`qcom,iommu-dma-addr-pool = <0x20000000 0x1fffffff>`,
`"fastmap"`, coherent pagetable), and `drivers/bus/mhi/host/sahara.c` plus
`mhi_qcom_sdx55_fusion_info` (`fw sdx55m/sbl1.mbn`, `amss_image`) implement
the same BHI->Sahara flow for this device tree.  The next read is
`umi_modem_mhi.inc`'s BHI upload and SMMU setup against
`devices/thyme/drivers/modem/hakios.thyme_pcie2.c`.

### The umi stack against the thyme probe, line by line (2026-11-20)

That read is done, and it removes most of the difference surface.  What the
two implementations do *identically* -- so none of it can explain the
refusal:

- **The BHI write set.**  Linux's `mhi_fw_load_bhi`
  (`drivers/bus/mhi/host/boot.c:284-288`) writes BHI_STATUS=0,
  IMGADDR_HIGH, IMGADDR_LOW, IMGSIZE, IMGTXDB.  umi writes the same five plus
  BHI_INTVEC=0 (`umi_modem_mhi.inc:1150-1152`); the probe writes the same
  five (`hakios.thyme_pcie2.c:1740-1744`) and the INTVEC word too (:1546).
  The doorbell word is a *session id* in Linux
  (`MHI_RANDOM_U32_NONZERO(BHI_TXDB_SEQNUM_BMSK)`, `common.h:66`), not an
  image type: umi's literal 1 and the probe's `0x1234567` are both valid
  sessions.
- **The SMMU context.**  `modem_smmu_map` (:844-862) and umi's
  `md_dma_install` (`umi_modem_dma.inc:147-167`) write the same constants:
  CBAR `0x1f300`, CBA2R 1, TTBR0 = tables|CB<<48, TTBCR2 `0x38011`, TTBCR
  `0x803519`, MAIR0 `0xf404ff44`, page attribute `0xe43`, TLBIASID+TLBSYNC,
  SCTLR `0x1127`.  The probe's log reads SCTLR back as `0x127` because bit
  12 is RAZ on this SMMU; the boot chain's own context 3 also reads `0x127`
  (`logs/171914-run.log:2973,2977`).
- **The route and the board.**  BDF-to-SID entry `0x01000100` (the CRC8
  slot in umi, all 256 slots in the probe), PARF+0x2c00 bit 0 cleared
  (:1591-1595; `umi_modem_dma.inc:225-226`), GPIO 1/3/56/57 in the south
  tile, PERST 85, rails `ldoa5` 880 mV / `ldoa9` 1200 mV (the probe votes
  the same pair, `hakios.thyme_rpmh.c:468-469`), GDSC `0x6004`, AUX RCG
  `0x6038`, shared REFGEN `0x6f014=0x10b`, the branch set, the resets, the
  PHY table, DBI lane/width/speed, the iATU, ID `17cb:0306`, BAR0
  `0x64300000`, bus mastering only after a private SMMU domain exists.
- **The firmware.**  The same 548,056-byte `sbl1.mbn`; the PBL
  authenticates it (a byte flipped at 0x8000 changes the answer to the auth
  group `0xef0a0105` in 10 ms), and the size/length checks answer
  `0xef0e0002`/`0xef120700` in 0 ms.

What is still different, in descending order of suspicion:

1. **PERST# order, CLKREQ# and WAKE#.**  umi asserts PERST# first
   (`umi_modem_pcie.inc:53`), keeps it low through AP_STATUS, the rails, the
   clocks and the whole PHY sequence, and releases it only after PHY ready
   plus 100 ms (:139).  The probe asserts it inside `link_train()`, after
   the rails and clocks (:409-416), and releases it 10 ms after PHY ready
   (:467-472).  umi also programs CLKREQ# 86 (mux 1, pull-up) and WAKE# 87
   (input, pull-up) (:63-66); the probe never touches either pin.
2. **The endpoint's MSI and the GIC MBI alias.**  Before the doorbell umi
   finds the PCI MSI capability, leaves every vector masked, sets message
   address `0x17a10040` / data 864, enables MSI, and maps `0x17a10000`
   identity in the same SMMU domain (`umi_modem_dma.inc:94-96,196-216`).
   Their DTS notes say SBL bypasses the DWC's internal MSI receiver.  The
   probe configures no MSI and maps only the image window.
3. **The image's memory shape.**  umi: 16 MiB `kmem_alloc_contig` mapped
   `ml_io_map_unmappable(..., VM_WIMG_WCOMB)`, IOVA `0xb0000000`
   (`umi_modem_dma.inc:79-83`).  The probe has run the SLPI heap at
   `0xc0000000` behind its own context, contiguous kernel memory above
   4 GB, the DT pool `0x20000000` physical, and its own BSS -- all
   `0xef0e0003` in 30 ms.  umi's exact WCOMB/`0xb0000000` shape is the one
   variant not tried.
4. **SID ownership.**  umi allocates a free SMR for `0x1d01` and refuses
   with EBUSY if the SID is already owned (`umi_modem_dma.inc:122-125`);
   thyme's boot chain already owns it (context 3), and the probe repoints
   the match to its own context 5.  No fault appears and the PBL's reads
   complete, so this is noted, not implicated.

The power-reset choice is *not* a difference.  umi never writes the AP PON
(their `MODEM-MAINLINE.md` is explicit that the modem node has no reset
GPIO or regulator and that mainline relies on inherited power); the probe
has run with the PMXprairie warm reset, with it late, and without it
(`pcie2_keep=1`, `logs/211817-run.log`) -- the answer is the same
`0xef0e0003` in 30 ms.  So the state umi calls its prerequisite, "the boot
chain left it in PBL", is the state every one of those runs was in.

### The next observation is the modem's, not the host's (2026-11-20)

Everything the host controls that these runs varied -- the doorbell word,
the image's address, its size page, the SMMU, PON, PERST, keep -- leaves
`status 0xffffffff, errcode 0xef0e0003, dbg <addr> 0 <size>` unchanged.
The code's public meaning could not be established (a literal search
returns nothing usable; the GitHub token used for the repository survey is
no longer valid).  What the umi driver makes observable is the execenv
transition: after the BHI transfer it waits for `BHI_EXECENV==1` before it
touches MHI contexts (`umi_modem_mhi.inc:1156`), and later for 2 (:1170) --
the SBL sets 1 when it starts, AMSS sets 2.  Until this probe, the only
read of execenv came once, after the failure had already happened.

The decisive measurement, now implemented, is to sample BHI_EXECENV and
BHI_STATUS every millisecond for the first 500 ms after the doorbell:
**`pcie2_eetrace=1`** turns it on.  It reads only, writes nothing new, and
logs every change as
`pcie2: BHI trace: <ms> ms: execenv <v>, status <v> (result n), MHISTATUS <v>`,
with the errcode and the three debug registers on the line where result 3
first appears (the PBL could clear them later).  The window ends early only
on success; the normal 10 ms wait then follows unchanged, its elapsed count
continuing from the trace's.  It builds cleanly (`CC hakios.thyme_pcie2.o`,
kernel linked) and has run twice on the phone (the result is below).  The
outcome is read two ways:

- execenv leaves 0 (and for how long) -> the PBL jumped and the SBL ran;
  the refusal is inside the SBL's start, the DDR/power class, and the
  host-side port list below is secondary.
- execenv never leaves 0 -> the PBL itself refuses the placement, and the
  three umi-only host differences become the first-order suspects.

Then, one per run, in this order:

1. `pcie2_perst_early=1`: PERST# asserted before AP_STATUS and the rails,
   released after PHY ready + 100 ms, with GPIO 86/87 programmed as umi
   does.  Same PON choice as the canonical run.
2. `pcie2_msi=1`: the MSI capability and the `0x17a10000` page, all vectors
   left masked; polling unchanged.
3. `pcie2_img=6`: a 16 MiB contiguous allocation, WCOMB-mapped, IOVA
   `0xb0000000` -- umi's exact shape.
4. Only if those leave the answer unchanged: the single-entry BDF-to-SID
   table (clear all 256, write the CRC8 slot) instead of the probe's
   all-256 write.

Each is a boot-arg-gated experiment in the existing probe; none touches a
partition, a fuse, or the AP PMIC beyond the PON write the probe already
has.  The full boot-arg set stays mandatory, and a run needs the battery
above 30%.

### The trace answers it: the PBL never jumps (2026-11-20)

Two RAM boots of the same kernel with `pcie2_eetrace=1`:

- `pcie2_bhi=1 pcie2_img=1 pcie2_eetrace=1` (the script's defaults, so no
  `pcie2_sid`, no `pcie2_iova`): errcode `0xef120700` at 79 ms, dbg
  `0xc0000000 0 0x85cd8`; execenv 0 at 0 ms, 0 at 79 ms, 0 at the end of
  the window.  Without the SMMU context the physical address is the
  untranslated class the matrix already had (`153232`), and the trace says
  that class, too, never leaves the PBL.
- `pcie2_bhi=2 pcie2_img=1 pcie2_eetrace=1 pcie2_sid=5
  pmic_modem_pon_sid=8 pcie2_iova=0x20100000` (the canonical set): errcode
  `0xef0e0003` at 32 ms, dbg `0x20100000 0 0x85cd8`; execenv 0 at every
  sample and 0 at the 500 ms mark.  AER uncorrectable 0, PARF interrupt
  status 0x2000, the SMMU's stream fault registers 0.

In both failure classes BHI_EXECENV never leaves 0.  umi's working stack
sees 1 there before it touches MHI (`umi_modem_mhi.inc:1156`), so 0 for the
whole window means the SBL never got far enough to run: the PBL reports its
own failure and never hands control over.  The refusal is post-
authentication (the control matrix's flipped byte moves it to the
`0xef0a0105` auth group at 10 ms; zeros to `0xef120700` at 0 ms), so what
the PBL rejects is the placement of an image it has already read and
accepted.

That is the second branch above: the umi-only host-side differences are now
the first-order experiments, one run each, in the order listed.  The probe
costs nothing to leave in: without `pcie2_eetrace` the code path is the
same as before.

### Experiment 1, the PERST/CLKREQ/WAKE order: not it (2026-11-20)

`pcie2_perst_early=1` moves the power-on order to umi's: after the PON
pulse, GPIO 86 (CLKREQ, function 3 with its pull-up) and GPIO 87 (WAKE,
function 3) are programmed, PERST (GPIO 85) is asserted before
AP2MDM_STATUS goes high and before the rails, and it is held 100 ms past
PHY ready instead of the vendor's 10 ms.  `logs/194752-run.log` shows each
step in that order (PON, GPIO86/87, PERST asserted, AP2MDM_STATUS, rails,
clocks, PHY ready, release).

The outcome is the canonical one unchanged: errcode `0xef0e0003` at 30 ms,
dbg `0x20000000 0 0x85cd8`, execenv 0 at every sample through the 500 ms
window.  One host-side caveat: the loader cut the command line's tail, so
`pcie2_iova=0x20100000` did not reach the kernel and the image sat at the
default IOVA `0x20000000` (the kernel's boot-args line ends
"... pmic_modem_pon_sid=8 pc").  Both IOVAs are in the matrix with the same
error, so the comparison holds; future probes should free room by trimming
a daily argument (`BASE_ARGS`), the real limit being nearer 380 characters
than the script's 420.

So the order of PERST against AP_STATUS, the rails and the PHY is not what
the PBL refuses.  The next experiment is `pcie2_msi=1`.

### Experiment 2, the MSI path: the PBL accepts and the SBL runs (2026-11-20)

`pcie2_msi=1` is the MSI half of the vendor's md_enable_dma
(`umi_modem_dma.inc:191-216`), after the SMMU context is up: the
endpoint's capability list is walked (cap 1 at 0x40, cap 5 at 0x50, header
`0x18a7005` -- 64-bit address, per-vector masking and MME 5: the form the
vendor insists on), the control register goes `0x18a -> 0x1bb` (enabled,
every vector masked), the message address is the GIC MBI alias
`0x17a10040` with data 864, and the GIC's page `0x17a10000` is
identity-mapped in the same context with the vendor's own descriptor
`0x0060000000000e4b` (UXN/PXN, AttrIdx 2 = MAIR0's 0x04, Device-nGnRE).
The entry is added while the context is live, so `smmu_enter_page`
invalidates the context's TLB after the walk (CBnTLBIASID + TLBSYNC) --
a step modem_smmu_map's build-before-enable path never needed.

`logs/200530-run.log`: the canonical set with `pcie2_eetrace=1
pcie2_msi=1`, 366 characters -- `pcie2_iova` dropped for room, so the
image sits at the default IOVA -- and the kernel's boot-args line shows
all of it arrived, through `pmic_modem_pon_sid=8`.

- The doorbell answers **2**: `BHI_STATUS 0x81234567` (result 2 in the top
  bits, the doorbell value below), errcode 0, dbg `0x20000000 0 0x85cd8`,
  32 ms after the ring.  Every earlier run answered 3.
- execenv is still 0 at 32 ms (the PBL reports before it hands over); at
  198 ms MHISTATUS is `0x101` (READY, state READY) and **BHI_EXECENV is
  1: the SBL is running**.  BHIEOFF stays 0.
- SMMU: no fault (context 5 FSR 0x400, fault bits 0; NS GFSR 0, SGFSR 0);
  AER uncorrectable 0; PARF core errors 0.  The boot went on to launchd,
  the run ended at 90 s and the phone returned to Android.

The only variable against the last failure is the MSI code: the
perst_early run carried the same IOVA (0x20000000, its command line cut)
and still answered `0xef0e0003`.  The vendor's comment on the page -- it
covers "SBL's direct MSI writes" -- is the explanation: the PBL signals
over the GIC page, and with the page unmapped (and the capability's enable
bit clear) the boot aborts with `0xef0e0003` after reading and accepting
the image.  Which half is load-bearing is not isolated (one run); both are
the vendor's own steps.

Experiments 3 and 4 on the list above are moot: the PBL's refusal was
never about the image's placement.  The next stage is the vendor's own
after its "BHI SBL accepted" (`umi_modem_mhi.inc:1153-1162`):
`md_contexts()` hands the SBL real MHI context rings -- this driver used
to write the image address into CCABAP/ECABAP/CRCBAP -- and `md_sahara()`
speaks the Sahara protocol over MHI channel 1 to serve the rest of the
image set (`fw-modem-sdx55m/` already carries sbl1, tz, hyp, aop, qdsp6sw,
multi_image, devcfg, mdmddr), after which BHI_EXECENV reaches 2 (AMSS).
That port is now written (`pcie2_mhi=1`, Experiment 3); what the first run
showed is below.

### Experiment 3, the MHI engine and Sahara: built, and a run whose evidence the log lost (2026-11-20)

`pcie2_mhi=1` turns on the stage after BHI's result 2, ported field by
field from the vendor (`umi_modem_mhi.inc:114-676`,
`umi_modem_dma.inc:74-216`):

- A 1 MB `kmem_alloc_contig` pool (`KMA_KOBJECT|KMA_ZERO`, one
  `flush_dcache`, written only through the write-combining alias), mapped
  into the same SMMU context 5 at IOVA `smmu_iova + 0x100000` --
  `0x20100000` with the default image IOVA -- by `smmu_enter_range()`,
  which walks the existing L2 table, fills 256 PTEs with the vendor's
  `0x0e43` and invalidates the context's TLB.  The image at `0x20000000`
  (548,056 bytes) and the pool do not overlap.
- The twelve channel contexts the vendor fills (2, 3, 4, 5, 10, 11, 14,
  15, 20, 21, 100, 101), six event contexts with rings of
  {64, 128, 64, 64, 1024, 2048} entries, a command context and a 128x16
  command ring; `CCABAP/ECABAP/CRCBAP`, `MHICFG = (cfg & 0xffff) | 6<<16
  | 2<<24`, ERDB armed on rings 0-3 before M0, `MHICTRL`'s
  reset-cleared `0x200` request, waiting for `(MHISTATUS & 0xff04) ==
  0x200`.
- Channels 2 (TX) and 3 (RX) on event ring 1: a START_CHAN command entry
  (`word3 = id<<24 | 18<<16`), its completion checked as event 0x21 on
  ring 0 with code 1, then a 32 KB receive buffer posted on channel 3.
- A Sahara server: HELLO (echoing the transfer-pending flag), READ_DATA32
  and READ_DATA64, END_IMAGE (a non-zero status is an error), DONE (a
  status above 1 is an error), each served from a 32 KB packet over
  channel 2, with `EAGAIN` when BHI_EXECENV becomes 2 mid-stream.  The 16
  images are read from `/redwood/fw/modem/` or `/fw/modem/`; `build.sh
  SDX55=1` stages them into the system image's `/fw/modem` (the loader's
  64 MB cannot hold them with root.apfs and the kernel), and qdsp6sw.mbn
  is decmpfs -- the kernel's `apfs_inode_size` reports its uncompressed
  84,004,996 bytes, which is what `vn_rdwr` then reads.

`logs/204534-run.log`, the canonical set plus `pcie2_mhi=1` (378
characters, `pcie2_sid=5 pmic_modem_pon_sid=8` still last):

- BHI answers 2 again (`0x81234567` at 32 ms); at 200 ms MHISTATUS is
  `0x101` and BHI_EXECENV is 1 -- the SBL runs.
- The engine starts: `pcie2: MHI: pool 1024 KB, physical 0x1a7238000 at
  IOVA 0x20100000`, then the six event rings at `0x20105000`,
  `0x20106000`, `0x20107000`, `0x20108000`, `0x2010c000`, `0x20110000`.
- Nothing further is known from this run.  The loader's log keeps its
  first 256 KB and the last 1 MB; `logs/204534-run.log` carries the
  marker `[... the log between the first 256 KB and the last 1 MB was
  dropped ...]` right after the event-ring lines, and the `.live`
  snapshot taken near the end of the run also ends pcie2's output at
  event ring 5.  Either the next register access wedged or the M0,
  channel and Sahara lines fell into the dropped region -- the run does
  not say which.  **No claim is made that M0, the channels or Sahara
  were reached**; the probe script's `PASS` line only says its three
  presence checks matched.
- The SMMU recorded no fault during the stage (context 5 FSR 0x400, fault
  bits 0), and the boot went on to launchd.

The next run adds write-ahead lines around each MHI register step (so a
wedged access names itself) and follows the log every second with
`XL_FOLLOW_KEEP=1`, which keeps each snapshot and so cannot lose the
stage to the 1 MB ring.

### The M0 request is where the endpoint goes quiet (2026-11-20)

`logs/210742-run.log`, the canonical 378-character set plus
`pcie2_mhi=1`, with `XL_FOLLOW=2 XL_FOLLOW_KEEP=1`:

- The stage before M0 is healthy in this run, by its own lines: BHI
  answers result 2 at 33 ms (`0x81234567`, dbg `0x20000000 0 0x85cd8`),
  and at 197 ms the SBL is running -- `MHISTATUS 0x101, execenv 0x1`
  (`.live.16:2590,2902`).
- The engine's own write-ahead lines all land (`.live.16:2904-2915`):
  pool 1024 KB physical `0x1a7238000` at IOVA `0x20100000`, the six
  event rings, `CCABAP 0x20100000, ECABAP 0x20102000, CRCBAP
  0x20104000`, `MHICFG 0xa800b80 -> 0x2060b80`, the doorbells armed, and
  the M0 request at 21:07:53.5.
- The 20 s wait ends with `pcie2: MHI: no M0: status 0xffffffff` and
  `pcie2: MHI: M0 ends with 60: BHI_EXECENV 0xffffffff, MHISTATUS
  0xffffffff, BHI_STATUS 0xffffffff` (`.live.16:6842-6843`,
  21:08:15.796).  Every MHI and BHI register reads all ones -- the
  endpoint stopped answering its BAR at or during the M0 transition.
- `pcie2: SMMU: our context 5 FSR 0x400 (fault bits 0) FAR 0xb4e4a7e8`
  (`.live.16:6844`): the SMMU recorded no translation fault for the
  image or the pool, so the device did not trip over the mapping.
- The host is fine: launchd lines continue in the same ring at the same
  millisecond.  This is the device going quiet, not the host wedging.

The main log does not carry any of this: its ring keeps the first 256 KB
and the last 1 MB, and by the end of the run the M0 lines sat between
them (drop marker at `logs/210742-run.log:3505`; the first 256 KB end at
21:07:54, the tail resumes at 21:08:49).  The kept `.live` snapshot is
what holds the stage; the earlier uncertainty about the missing M0
result was this drop, not a wedged access.

#### Two host-side differences from both proven implementations (fixed)

Comparing the engine against the two implementations known to work on
this SoC -- the vendor's `md_contexts()`/`md_start_channel()`
(`umi_modem_mhi.inc:320-380,178-235`) and the Wi-Fi MHI on this phone
(`hakios.thyme_mhi.c`, `umi_mhi_prepare_ready`) -- leaves two concrete
discrepancies on our side, both feeding the SBL a malformed register
surface:

1. **CRCBAP pointed at the command ring, not the command context.**  The
   vendor writes `md_address(0x68, md_cmdctx.a)` and the Wi-Fi MHI
   writes `write_address(0x68, commands.address)` -- both the 44-byte
   context entry.  The kernel that ran 210742 wrote the ring's own
   address there (the run's log line printed the context address, so the
   log did not show it).  A device reading a ring entry as a context
   takes word 0 as its ring base -- zero before any command is posted.
   Fixed: 0x68 now gets `mhi_pool.iova + cmdctx`; the ring pointer stays
   in the context's words 3/7/9, where it already was.
2. **The doorbells were written 32 bits wide.**  The vendor's
   `md_address()` writes low then high, and the Wi-Fi MHI's
   `write_address()` does the same.  Our `wr()` wrote only the low word
   of CHDB/ERDB/CRDBAP and of the pre-M0 ERDB arming, leaving the upper
   half of a 64-bit ring address as BHI or the SBL left it.  Fixed with
   `mhi_doorbell()`, which writes both halves, at every doorbell site.

Both fixes built (`CC hakios.thyme_pcie2.o`, `make-kernel: built:
hakios-kernel`) and ran on the phone in 215200; the next section reports
what changed there.

#### What the next run will say that this one could not

The M0 wait now names its failure as it happens -- an all-ones status
read, a SYSERR bit, or a one-second heartbeat with MHICTRL -- and on
timeout dumps the host-side state the 210742 run never read: MHICTRL,
MHISTATUS and BHI_STATUS; the DBI link status (0x80) and ELBI; PARF's
interrupt status, status word and core errors (0x224/0x230/0x3c0); AER
uncorrectable/correctable/root; and `mmu_walk()` of the pool IOVA
through context 5's TTBR0/TCR, which shows the exact PTE the device
would walk.  Link down, a register file that no longer answers despite a
live link, and a bad PTE then read differently instead of all as
`0xffffffff`.

Run 215200 answered it: the wait passed and none of these dumps ran.

### Run 215200: M0 and the Sahara handshake, then a silent ENOTSUP (2026-11-20)

The canonical 378-character set plus `pcie2_mhi=1`, with the two fixes
above and `XL_FOLLOW=2 XL_FOLLOW_KEEP=1` (`logs/215200-run.log`):

- The stage before M0 repeats 204534 and 210742 exactly: BHI result 2 at
  33 ms, and at 200 ms `MHISTATUS 0x101, execenv 0x1` -- the SBL runs.
- The engine reaches **M0**: `pcie2: MHI: M0, six event rings, contexts
  at 0x20100000, doorbells 0x300/0x700` (`.log:2991`), after CRCBAP was
  written as the command context (`0x20103000`) and the doorbells with
  both halves.  Between 210742 and this run the only functional change
  is those two fixes; no other write to the modem changed.
- Channels 2 and 3 open on event ring 1 (`:2992-2995`), and the SBL's
  Sahara service works: `HELLO, image mode 0`, `READ_DATA id 34
  (mdmddr.mbn) offset 0 length 40 of 40`, 40 bytes in 1 ms, `image 34
  complete, status 0` (`:2996-2999`).
- About 3.3 s later the stage ends with ENOTSUP: `.live.9:5106` has
  `pcie2: MHI: Sahara ends with 45: BHI_EXECENV 0x1, MHISTATUS 0x201,
  BHI_STATUS 0x81234567`, with context 5's FSR still 0x400 and the fault
  bits clear (`:5107`).  The endpoint never left M0 and never left the
  SBL's execution environment: it waits in Sahara while the host has
  stopped.
- 45 is ENOTSUP.  Every receive, read, send and unknown-command failure
  in this code logs before returning; the one path that returns ENOTSUP
  without a line is the HELLO validation (the vendor's own checks: 48
  bytes, version >= 2, version_min <= 2, mode <= 1).  So the SBL sent a
  command 1 whose offending field this run did not print -- most
  plausibly the re-HELLO after mdmddr, the 3.3 s gap being DDR init.
  **Which field failed is not known from this run.**
- The main log lost the stage again: its head ends at 21:52:11.6
  (`image 34 complete`), its tail resumes 21:52:45, and the stage-end
  line at 21:52:14.93 sits between them (drop marker `:3503`).
  `.live.9`, taken while that line was still the ring's tail, holds it --
  this is what keeping the snapshots is for.

The build after this run logs the whole Sahara dialogue (`request N:
command %#x, N bytes`) and, when a HELLO is refused, its six header
words.  No claim is made past this point: the AMSS has not started
(BHI_EXECENV never read 2), no QMI/QRTR/TFTP stage has run, and the
only image served was mdmddr.mbn.

### Run 220739: the refused HELLO was a mode-3 re-handshake (2026-11-20)

Same 378-character set, the build that logs every Sahara request
(`logs/220739-run.log`, SHA-256 `18cdfc0a8c73...58ef3b35`).  The whole
dialogue survives:

- `:3036-3043`: request 0 HELLO (0x1, 48 bytes, mode 0) answered mode
  0; request 1 READ_DATA (0x3, 20 bytes) id 34, offset 0, length 40 of
  40, served in 1 ms; request 2 END_TRANSFER (0x4, 16 bytes), image 34,
  status 0; the host's DONE_REQ (5, 8 bytes); request 3 DONE_RSP (0x6,
  12 bytes, status 0).
- `.live.7:4311-4313`: request 4 is **HELLO again**, 48 bytes, header
  words `0x1 0x30 0x2 0x1 0x400 0x3` -- version 2, version_min 1, max
  packet 0x400, **mode 3**.  Mode 3 is `SAHARA_MODE_COMMAND`; the
  vendor's `mode <= 1` check (kept for the read-only policy) returned
  ENOTSUP, and the stage ended `Sahara ends with 45: BHI_EXECENV 0x1,
  MHISTATUS 0x201, BHI_STATUS 0x81234567`.
- So 215200's unknown field was the mode, and the packet itself was
  well-formed: after the 40-byte mdmddr stub the SBL acknowledges the
  DONE and restarts the handshake in command mode.  In the protocol the
  mode in HELLO_RSP is the **host's** to give (the reference host,
  edlclient, answers a device re-handshake with `IMAGE_TX_PENDING` and
  continues its loop; `sahara_defs.py` has the mode enum).  Refusing the
  packet was this host's mistake, not the device's.
- The main log dropped these lines again; `.live.7` and `.live.8`
  (4311-4313, 4318-4320) kept them.

The build after this run answers a re-handshake with image transfer to
keep the image list going, answers a second consecutive mode offer as
offered (sending no command in command mode -- the read-only policy),
and answers a device DONE_REQ with DONE_RSP.

### Run 222749: the whole image list, then the endpoint stops answering (2026-11-20)

Same 378-character set and that build (`logs/222749-run.log`):

- **All twelve images the SBL asks for were served, every END_TRANSFER
  status 0** (`image N complete, status 0`, kept in `.live.16`): mdmddr
  34 (40 B), apdp 40 (13,508), devcfg 41 (42,515), tz 25 (913,408), hyp
  33 (84,416), aop 23 (154,688), efs1 16 / efs2 17 / efs3 20 (512
  each), acdb 29 (131,112), qdsp6sw 8 (84,004,996), apps 6 (2,632,704)
  -- 87,978,923 bytes, qdsp6sw at 25 MB/s.
- The SBL re-handshakes after **every** image -- a HELLO follows each
  image's END_TRANSFER and DONE_RSP (requests 0, 4, 11, ...); the run
  ended at request 69 with apps' DONE_RSP.  The mode-3 offer happened
  exactly once, the first re-handshake after mdmddr (`.live.10:4297`);
  every later one offered mode 0/1.  Answering image transfer was
  accepted: the list continued.
- After the last image the endpoint goes quiet and then **stops
  answering the bus**: request 70 reads all ones and the stage ends
  `Sahara ends with 5: BHI_EXECENV 0xffffffff, MHISTATUS 0xffffffff,
  BHI_STATUS 0xffffffff` (`.live.16:6962`), about 17 s after apps
  completed.  BAR0 and the BHI block answering all ones means the
  modem's PCIe function is in reset or the link is down, not that the
  SBL crashed: the vendor's host expects the SBL to go quiet after the
  image list and waits up to 20 s for BHI_EXECENV 2.
- **No claim is made that the AMSS started.**  BHI_EXECENV was never
  read as 2; what the endpoint does next (comes back with the AMSS's
  MHI, or stays gone) is what the watch added after this run samples.

What this settles: the Sahara host is complete against this SBL -- every
image it asks for, at every offset, is served, the DONE exchange is
accepted, and the re-handshakes are answered.  The next unknown is on
the device side, where the image list hands over to the AMSS.

### Run 223539: the link is down, and 20 s do not bring it back (2026-11-20)

Same set, plus a watch after the Sahara stage that samples the
endpoint's config id, the root complex's link-up bit (DBI+0x80 bit 29),
BHI_EXECENV and MHISTATUS every 200 ms for 20 s
(`logs/223539-run.log`):

- The image list repeats 222749 exactly: twelve images, every
  END_TRANSFER status 0, then `fault at Sahara: status 0xffffffff,
  execenv 0xffffffff` and `no message (5) at request 70`
  (`.live.17:6812-6813`).
- The watch's first sample is already `endpoint id 0xffffffff, link 0,
  BHI_EXECENV 0xffffffff, MHISTATUS 0xffffffff` (`.live.16:6814`), and
  **no transition is logged in 20 s**.  So this is not the SBL merely
  going quiet on its rings: the modem's PCIe link is down at the root
  complex and its configuration space answers all ones.  The modem side
  reset its PCIe function after the image list, and neither side brought
  the link back inside the window.
- The watch did not yet sample the modem's status lines, so whether the
  modem itself is alive while the link is down is not known from this
  run; the build after it samples MDM2AP_STATUS (GPIO 3) and
  MDM2AP_ERRFATAL (GPIO 1) with the bus for 60 s, which separates "the
  modem is rebooting and the link will return" from "the modem is
  waiting for the AP".
- **Still no claim that the AMSS runs**: BHI_EXECENV was never read as
  2, and a down link cannot read it at all.

### Run 224136: the modem drops its own status line 1.8 s after the image list (2026-11-20)

Same 378-character set; the build whose watch also samples the modem's
status lines (MDM2AP_STATUS GPIO 3, MDM2AP_ERRFATAL GPIO 1) every
200 ms for 60 s, logging only transitions (`logs/224136-run.log`):

- The boot handshake read `mdm2ap status 0, errfatal 0` before the link
  (`:210`), and the Sahara list repeated 222749 exactly: twelve images,
  every END_TRANSFER status 0.
- The watch's first sample, at the end of the list: `endpoint id
  0xffffffff, link 0, BHI_EXECENV 0xffffffff, MHISTATUS 0xffffffff,
  MDM2AP_STATUS 1, MDM2AP_ERRFATAL 0` -- the modem's own status line is
  **high** even though its PCIe function is already gone
  (`.live.16:7496`).
- **At 1800 ms the status line falls**: `MDM2AP_STATUS 0`, nothing else
  changed (`.live.17:7691`).  No further transition is logged in the
  remaining ~58 s; the stage ends `Sahara ends with 5: BHI_EXECENV
  0xffffffff, ...`.
- So the modem was alive after the image list (it raised its status
  line the SBL left low), and 1.8 s later it deasserted it -- a state
  change on the modem side, not just the link.  ERRFATAL stayed low
  throughout.  Two readings fit: the modem reset itself (status is
  deasserted through a modem reset, and a reset lands it back in the
  PBL), or the AMSS takeover resets the PCIe function and the line with
  it.  The link never returning in 60 s rules out "the SBL merely goes
  quiet and the AMSS's MHI comes up on the same link" as the vendor's
  20 s window would have it: the **host** has to bring the link back.
- The main log dropped the whole watch again (ring); the `.live`
  snapshots around the stage-end line kept it (`.live.16`-`.live.21`).
- No claim that the AMSS runs: BHI_EXECENV was never read as 2.

The vendor's answer to a dead link is `md_bringup`'s restart: PERST
asserted and AP2MDM_STATUS pulled low, the lines re-applied, the rails,
the clocks and a fresh train (UMI's DT has no soft-reset GPIO, so no
PMIC PON), and it expects BHI_EXECENV 0 -- the PBL -- on the far side
before it reloads the images.  The build after this run does exactly
that once, reads BHI_EXECENV after the relink, and only if the PBL is
back with MHISTATUS.READY runs the image list a second time (bounded:
one restart, two rounds, no loop).

### Run 225537: the vendor's relink brings the PBL endpoint back (2026-11-20)

Same 378-character set and the build with the relink
(`logs/225537-run.log`):

- Cold boot as before: endpoint id **0x30617cb** -- the PBL (`:272`,
  the id the vendor's `md_pcie` expects, 0x030617cb) -- the BHI load's
  `result 2` after 33 ms, `after the SBL: MHISTATUS 0x101, execenv 0x1`
  (`:2888`), the twelve images, then the same drop (MDM2AP_STATUS 1,
  then 0 at 1.8 s, link down, 60 s of nothing).
- The relink ran once (`:12837`): AP2MDM_STATUS low, PERST, the rails,
  the clocks, a fresh train -- and **the link came up with endpoint id
  0x30617cb** (`:12901`), the PBL's own endpoint, the exact id the cold
  boot had read.  The vendor's restart shape works on this modem.
- After the relink: `BHI_EXECENV 0, MHISTATUS 0 (ready 0), BHI_STATUS
  0, BHIEOFF 0x100` (`:13189`) -- execution environment 0, the PBL.
- **The second round did not run, and the gate was the reason, not the
  state**: the code waited 1 s for MHISTATUS.READY, but the PBL takes
  the doorbell before it ever sets that bit -- the cold boot's own load
  ran with `MHISTATUS 0` (`BHI trace: 0 ms`, `:2543`) and the 0x101
  pattern appeared only after the SBL.  After the relink the endpoint is
  in the cold boot's exact pre-load state (`BHI_STATUS 0`, the same
  BHIEOFF 0x100, EE 0).
- No claim that the AMSS runs: BHI_EXECENV was never read as 2.

The build after this run gates the second image list on BHI_EXECENV 0
alone after a 200 ms settle, and if the PBL takes the doorbell again
the whole list is served a second time -- which is also the experiment
that says whether the post-list drop is deterministic.

### Run 230144: the PBL takes the doorbell and refuses sbl1 (2026-11-20)

Same 378-character set; the build whose second round is gated on
BHI_EXECENV 0 after a 200 ms settle (`logs/230144-run.log`):

- Cold boot as before: the PBL's id 0x30617cb, BHI `result 2` after
  32 ms (`:2545`), `after the SBL: MHISTATUS 0x101, execenv 0x1`,
  the same image list and the same post-list drop.
- This build ran the old 60 s watch first, so the relink began ~80 s
  after the list (`:12467`).  The status line had long since fallen
  (`MDM2AP_STATUS 0` at the relink, `:12524`), the train succeeded,
  and the endpoint came back as the PBL's id again (`:12532`) with
  `BHI_EXECENV 0, MHISTATUS 0 (ready 0), BHI_STATUS 0, BHIEOFF 0x100`
  (`:12539`).
- **The second round ran**: sbl1 kept its IOVA 0x20000000
  (`:12547`), the window was reprogrammed the same way, the doorbell
  was written, and **the PBL answered result 3 after 78 ms**
  (`:12558`): `errcode 0xef120700, dbg 0x20000000 0 0x85cd8` -- the
  same image the cold boot's PBL loads in 32 ms.
- The far side's own diagnostics, not a transport fault: PARF core
  errors 0, AER 0, the SMMU's FSR 0x400 in context 5 is FORMAT (this
  configuration's resting value), and the translation
  `0x20000000 -> 0xc0000000` is intact (`:12720`-`:12749`).
- Open then: why does the same PBL refuse sbl1 in the second round?
  The relink was the one difference, and it ran long after anything
  the modem had waited for.  The build after this one moved the
  relink inside that window.

### Run 232234: the prompt relink asserts PERST on a modem that has not reset (2026-11-20)

The build that relinks as soon as the link is gone
(`logs/232234-run.log`; the main log's ring dropped the stage, the
`.live` snapshots kept it):

- All twelve images served as before; the last (`apps.mbn`) completed
  just before 23:23:00, and the bus died at 23:23:08.9 -- ~9 s of SBL
  quiet this time (224136 showed ~17 s).  The fault surfaced inside
  the Sahara wait: `fault at Sahara: status 0xffffffff, execenv
  0xffffffff` (`.live.20:3989`).
- The prompt warm restart ran at once (`.live.20:3992`): AP2MDM_STATUS
  low, PERST, 150 ms, the rails, the clocks, a fresh train -- and
  **the modem still held MDM2AP_STATUS high** (`MDM2AP_STATUS 1`,
  `.live.20:4101`): its own reset had not finished when the PERST
  came.
- **The endpoint never trained**: `link requests done; ELBI 0x1,
  link status 0x10110008` -- the cold boot's own pre-training value
  (`:265`) -- LTSSM 0x1 polled for 3 s, `no link training done: 0x1`
  (`.live.20:4147`), `relink: no link`.
- The stage ended `BHI_EXECENV 0xffffffff` (`.live.20:4179`); the
  environment was never re-read and nothing was claimed.  The one
  warm restart that did train (230144) ran with `MDM2AP_STATUS 0`.

The build after this run waits for MDM2AP_STATUS to fall before the
warm restart (in 224136 it fell 1.8 s after the watch began), and a
line that never falls gets the cold restart -- the PMIC's modem reset,
the sequence every successful boot has used -- before BHI_EXECENV is
read, with the second image list still bounded to one.

### Run 232957: the SBL never says status 1, and asks for 40 bytes of each store (2026-11-20)

The build that waits for the modem's own reset instead of asserting
PERST on it, with the whole 378-character argument set
(`logs/232957-run.log`; the main log's ring kept the head and the tail,
the `.live.10`/`.live.20` snapshots the stage in between):

- The command line is at the ceiling: `WARNING: the kernel command
  line is TRUNCATED: 72 characters of the payload's cmdline did not
  fit in the 607` (`:13`), and the kernel saw 607 characters ending in
  `pmic_modem_pon_sid=` -- the PON reset's SID never reached it. The
  full set was kept anyway (the rule for real-device runs), so this
  run's probe arguments were cut, not the daily ones.
- Cold boot as every time: the PBL's BHI (`version 1.2, execenv 0,
  numseg 0x3`, `:277`), sbl1 548056 bytes at 0xfe2ec000 for physical
  0xc0000000 (`:2556`), `status 0x81234567 (result 2)` after 33 ms
  (`:2671`), then `after the SBL: MHISTATUS 0x101, execenv 0x1,
  BHIEOFF 0 after 195 ms` (`:2938`).
- The SBL then walks its image list, **one fresh HELLO before every
  image** (`HELLO, answering mode 0`; one of them `offers mode 3`,
  answered "image transfer to keep the image list"): 34 mdmddr, 40
  apdp, 41 devcfg, 25 tz, 33 hyp, 23 aop, 16 efs1, 17 efs2, 20 efs3,
  29 acdb, 8 qdsp6sw (84 MB), 6 apps (2.6 MB) -- twelve images, each
  ending `image N complete, status 0`, each answered by our DONE and
  by the SBL's `command 0x6, 12 bytes` DONE_RSP.
- What it asks of the four store IDs is 40 bytes each, not the whole
  placeholder: `READ_DATA id 16 (efs1.bin) offset 0 length 40 of 512`,
  the same for 17 and 20, `id 34 (mdmddr.mbn) offset 0 length 40 of
  40`. Forty bytes is a header it reads and goes on from; what it got
  was the first 40 bytes of "IMGEFS- DUMMY-N" and of the 40-byte
  mdmddr stub.
- The last image ends at `request 68: command 0x4 ... image 6
  complete, status 0` (`.live.10:5831`), and its DONE_RSP is
  `request 69: command 0x6, 12 bytes` (`.live.10:5832`) -- and **no
  `Sahara: complete` line follows, in this run or any other**.  The
  vendor's own host reads the same packet the same way: status 1 is
  the one that says the SBL is done and jumping (`md_sahara`,
  `/tmp/umi/umi_modem_mhi.inc`: `if (status==1) { "Sahara complete";
  return 0; }`, status 0 falls through and keeps serving).  The SBL
  accepted the image and asked for nothing more; it never handed
  over.
- It then goes quiet and resets itself: the Sahara wait faults
  (`fault at Sahara: status 0xffffffff, execenv 0xffffffff`,
  `.live.20:5311`), the endpoint leaves with the list and the build
  waits for the modem's own reset (`:5313`-`:5314`), and MDM2AP_STATUS
  falls 1800 ms after the link died (`.live.20:5913`).
- The relink runs as designed: the PMIC's modem warm reset is still
  `-19` (`:5987`, the PON block refuses as on the cold boot) but the
  modem's own reset had done the work, and the link trains and comes
  back as the PBL's endpoint (`:6088`): `BHI version 1.2, execenv 0,
  status 0, errcode 0, imgaddr 0/0, imgsize 0, numseg 0x3` (`:6094`).
- The second round (`:6735`) hands the PBL sbl1 again at 0xfed14000
  with its IOVA kept (`:6743`) and is refused exactly as in 230144:
  `BHI: status 0xffffffff (result 3), errcode 0xef120700, dbg
  0x20000000 0 0x85cd8 ... after 500 ms` (`:6889`); the stage ends
  `Sahara ends with 5: BHI_EXECENV 0, MHISTATUS 0, BHI_STATUS
  0xffffffff` (`:6919`).

The finding to act on: the four store IDs are answered from the
firmware package's placeholders, and the SBL reads a 40-byte header
out of each.  In stock the same conversation ends with status 1; here
it ends with status 0 and a self-reset.  The vendor's host serves
those four IDs from the phone's own partitions (`md_ufs_partition_for_id`
-> mdm1m9kefs1/2/3 and mdmddr, read only).  The next experiment is
to serve the real bytes and see whether the SBL's last answer becomes
status 1.

### Run 004759: the stores' real bytes, and why the first dump showed nothing (2026-11-21)

Run 003446 (`ufs=2 mdm_dump=1`) created the driver's 119 `/dev/ufs_NAME`
nodes -- the run's own `/dev` listing carries them -- but not one line
of the driver reached any log, neither the init line nor the dump's.
The reason is not boot timing, which was the first theory and put the
dump on a 5 s thread call without helping: **in this kernel a plain
`printf` does not reach the loader's log** (osfmk/arm64/loader_return.c,
the 256 KB head and 1 MB ring that `xl log` and `XL_FOLLOW` fetch).
The log keeps what is written into it with `loader_log_putc` -- how
`hakios.battguard.c`'s `say()` and `hakios.cpufreq.c`'s `tell()` do it,
which is why battguard's line is the first line of every run's kernel
log, and why drivers that `#define printf hakios_tl` appear there too.
`drivers/storage/hakios.ufs.c` now prints the dump through `mdm_say`
(loader_log_putc first, printf second); run 004759 carried all 55 lines.

What the dump read through the loader's block page (read only; sizes
from the partitions, first bytes printed):

| partition    | size    | first bytes |
|--------------|---------|-------------|
| mdm1m9kefs1  | 8 MB    | `10 00 00 00 03 00 00 00 d8 01 00 00 00 02 00 80 00 fe 1f 00 00 fe 1f 00 00 00 20 80 00 ... 49 4d 47 45 46 53 31` (`IMGEFS1`) |
| mdm1m9kefs2  | 8 MB    | `11 00 ... 00 02 20 80 ... 00 40 80 00 ... 49 4d 47 45 46 53 32` (`IMGEFS2`) |
| mdm1m9kefs3  | 8 MB    | `14 00 ... 00 02 40 80 ... 00 60 80 00 ... 49 4d 47 45 46 53 47` (`IMGEFSG`) |
| mdmddr       | 1 MB    | `22 00 00 00 00 00 00 00 00 00 00 00 28 50 8d 14 3c 06 00 00 ...` |

The placeholders are not random: the firmware package's `efs*.bin`
carry the real header's first 12 bytes (magic `10`/`11`/`14`, version
3, `d8 01`) and, for efs2/3, the address word at 12-15; everything
from the first address field on is zeroed, and `IMGEFS- DUMMY-N`
sits where the real store has its `IMGEFS{1,2,G}` item header and
checksum.  `mdmddr.mbn` differs from the phone's first 40 bytes in 7
of them (`03 00 00 00` at byte 4, `ff ff ff ff` at 12-15).  So the
placeholders can pass a size and magic check and carry none of the
identity the SBL may be looking for.

The real bytes now live in `fw-modem-sdx55m/efs{1,2,3}.bin` and
`mdmddr.mbn` (the directory is outside the repository's git, as the
firmware package is) and the system image was rebuilt with
`SDX55=1 SYSTEM=1`, which stages them into `/fw/modem`.  The next
probe run answers Sahara IDs 16/17/20/34 from the phone's own stores;
if the SBL's closing DONE_RSP still says status 0, the stores' content
was not what it was waiting for.

### Run 012201: the stores from the phone's own partitions (2026-11-21)

Kernel 6edfcec5 carries the partition branch of `mhi_sahara_read()`:
IDs 16/17/20/34 are served through `hakios_ufs_read_partition()` from
mdm1m9kefs1/2/3 and mdmddr (read only, `ufs=2`); the image's files
stay the fallback when `ufs=2` is absent.  This run is the first with
`pcie2_mhi=1` and `ufs=2` together and the whole probe argument set
reaching the kernel.

- The 607-character command line, measured from this run's echo: the
  loader writes 263 characters in front when `ufs=2` is among ours
  (`ufs_shared_pa=` is 33 of them), and `xl` appends
  `kernel_sha=… image_sha=… loader_sha=…` (70) behind the payload's
  own line -- so the probe's 335 arrive and the 62 characters cut
  are xl's hashes, not ours.  `modem-probe.sh` now says so
  (`note: xl's sha arguments will be cut, not ours`).
- mdmddr is read exactly as its header declares: `READ_DATA id 34
  (mdmddr, the phone's partition) offset 0 length 40 of 1048576`,
  then `offset 40 length 1596 of 1048576` -- 0x28 + 0x63c, no ERANGE
  (run 005621 stopped here against the 64-byte file).
- The list's order: mdmddr (34) first, then apdp (40), devcfg (41),
  tz.mbn (25, 913408 bytes), hyp (33), aop (23) from the image's
  files -- each `image N complete, status 0`, each followed by the
  SBL's `command 6` DONE_RSP and a new HELLO that the driver answers
  mode 0 -- and then the stores.
- The stores come last: `id 16 (mdm1m9kefs1, the phone's partition)`,
  17, 20; each is the first 2 MB of the partition in three reads
  (40 bytes at 0, 1048576 at 512, 1048064 at 1049088) at 22-23 MB/s.
  16 and 17 complete, status 0.
- This log's transcript ends at `request 54 … READ_DATA id 20 …
  offset 512 length 1048576 of 8388608`: the console's first 256 KB
  ended while that chunk was in flight (userland, SpringBoard, kept
  logging), and at 90 s the boot's ~1.5 MB exceeded the 256 KB + 1 MB
  the kernel keeps (osfmk/arm64/loader_return.c), so the list's end
  fell in the dropped middle.  `XL_SECS` is what composes the
  loader's `loader_return_after=` (tools/boot-args.py), so the next
  run ends at 60 s: ~1 MB logged, none dropped.

What this settles: the partition reads work from the probe's kernel
thread at full speed, and the SBL accepts every store and asks for
the next.  Whether it then jumps to AMSS (BHI_EXECENV 2) is not in
this log; the 60-second run is to see it.

### Run 014904: the list runs on past the stores, to the AMSS image (2026-11-21)

Same kernel, same image, ended at 60 s (`XL_SECS=60`).  The stores
were not the end of the list:

- After efs3 completes (`image 20 complete, status 0`), the SBL asks
  for `acdb` (29, 131112 bytes), then `qdsp6sw.mbn` -- ID 8, the
  AMSS image itself, 84004996 bytes.  The driver serves it in 8 MB
  chunks and logs progress every 8 MB: `id 8: 8388608 bytes in
  304 ms`, `16777216 in 603 ms`, `25165824 in 903 ms` (26 MB/s).
- Each image is still followed by the SBL's `command 6` DONE_RSP
  and a fresh HELLO that the driver answers mode 0, as with the
  stores.
- The 60-second log still overflowed the 256 KB + 1 MB the kernel
  keeps: the drop marker sits at line 4048, and the head ends
  mid-transfer of id 8 (`25165824 bytes in 903 ms`), so the list's
  end fell in the dropped middle again.
- Timing, from the loader's follow excerpts in the same log (bytes
  the kernel had logged): 227135 at 12 s, 556541 at 18 s, 622837 at
  22 s, 637839 at 24 s.  The head's 256 KB ended at ~13 s, which is
  where id 8 began; at 26 MB/s its remaining 60 MB take ~2.3 s, so
  the list should end around 16 s -- long before the 60-second
  return.  The next run returns at 45 s: ~1 MB of log, nothing
  dropped, and ~25 s of the AMSS watch inside.

Two tooling notes from the same day: `xl enter`'s `fastboot boot`
hung once for five minutes while the phone sat in its own bootloader
(the Windows `fastboot.exe` did not take the timeout's SIGTERM and
`xl` stayed in the read on its pipe); `xl`'s `fb()` now uses
`timeout -k 5` so the KILL always comes.  And `modem-probe.sh`'s own
last step (`ls -t logs/*-run.log | head -1`) died of SIGPIPE under
`pipefail` before printing the log's hash and the PASS/FAIL lines --
the run itself was unaffected; it now uses `sed -n 1p`.

### Run 015338 and the live snapshots: the list's end, and the AMSS (2026-11-21)

`XL_SECS=45` (run 015338) still dropped the middle: the head's 256 KB
ended at `id 8: 33554432 bytes in 1213 ms`, and the drop marker sat at
line 4042.  Shortening the run was losing a race against userland's
log rate -- once SpringBoard and `hakios-gl` start, the last 1 MB of
a 45-second boot covers only a few seconds.

The way around it was already in the run script: `XL_FOLLOW=2` and
`XL_FOLLOW_KEEP=1` make `xl run` fetch the log as it stands every two
seconds (`fastboot oem log`) and keep every snapshot as
`logs/<run>.log.live.N`.  Each snapshot is under the kernel's
256 KB + 1 MB, so up to ~1.1 MB of log there is complete.  Run
020046's snapshots 18 and 19 (1.05 and 1.14 MB, ~36 and ~38 s in)
carry the whole end of the exchange:

- The list has a twelfth image after qdsp6sw: `READ_DATA id 6
  (apps.mbn) offset 0 length 52 of 2632704`, served in four reads at
  25 MB/s, `image 6 complete, status 0`, then the SBL's `command 6`
  DONE_RSP (line 5447 of snapshot 19) -- and then nothing more.
- The driver waits on channel 3; the channel's watchdog logs `5000 ms
  without a packet`, then 10, 15 and 20 s, and at 20 s the read times
  out: `Sahara: no message (60) at request 77`.  (That 20-second
  silence timeout is why the stage's end lands ~36 s in, not ~16 s,
  where the list actually ends.)
- What the endpoint is by then: `after 12 images (94232091 bytes):
  endpoint id 0x30617cb, link 0x20000000, BHI_EXECENV 0x2, MHISTATUS
  0x201, MDM2AP_STATUS 1, MDM2AP_ERRFATAL 0` -- the link is still
  trained, the modem holds its status line high, and **BHI_EXECENV
  reads 2**: `pcie2: MHI: the AMSS has taken BHI over (BHI_EXECENV
  2)`.
- The stage's end line: `Sahara ends with 60: BHI_EXECENV 0x2,
  MHISTATUS 0x201, BHI_STATUS 0x81234567` -- BHI_STATUS still holds
  the answer to sbl1 (result 2).  No further pcie2 line follows in
  any snapshot.
- The SMMU records one translation fault against our stream's context
  while the AMSS comes up: `our context 5 FSR 0x400 (fault bits 0)
  FAR 0xa464a7e8` -- an address no mapping covers.  The AMSS starts
  DMA through SID 5 with its own expectations; that fault is the
  first thing the next stage (MHI with the AMSS) has to answer.

The driver change this run carried: `mhi_amss_watch` now prints a
5-second heartbeat (images served, bytes, BHI_EXECENV, MHISTATUS,
link, MDM2AP_STATUS) so a recent state line always sits in the log's
newest 1 MB -- runs 014904 and 015338 had lost exactly that.  This
run the watch exited at entry (`execenv == 2`), before the first
heartbeat; its entry line carried the result.

What this settles: the Sahara exchange completes -- twelve images,
94232091 bytes, the SBL acknowledges each one -- and the modem leaves
the SBL for the AMSS on its own, BHI_EXECENV 2, with the PCIe2 link
still up.  The external modem now runs its own firmware on the far
side of the link.  SIM, calls and SMS are still not started: before
anything can speak QMI, the host side has to speak the AMSS's MHI,
and the SMMU fault above has to be understood.

### Runs 021408 and 021924: the AMSS's MHI answers the host (2026-11-21)

The driver's next stage after the handover (`mhi_amss_start`, kernel
c5c955bd) does what Android's host does next: it re-reads the MHI
registers, and opens the QMI pair -- channel 2 out, channel 3 in --
with the same START_CHAN path the SBL's Sahara used.  Run 021408:

- The handover's registers: `MHICFG 0x2060b80, CHDBOFF 0x300,
  ERDBOFF 0x700, MHICTRL 0x200, MHISTATUS 0x201` -- the state is
  exactly what the host left the engine in before the SBL's list, so
  the AMSS inherited a live M0.
- `channel 2: START_CHAN at 0x20104020 ... channel 2 open on event
  ring 1`, and the same for channel 3: the AMSS's MHI accepted both
  commands (completion code 1).  The host is talking to the AMSS's
  engine, not the SBL's.
- Twelve seconds of listening on channel 3 produced nothing, which
  is what a silent QMI stack looks like: QMI is request and answer.
- The SMMU's translation fault repeats with the same low bits and a
  moving high part (`FAR 0xb4e4a7e8`, `0xb6a4a7e8`; 020046 saw
  `0xa464a7e8`) -- the AMSS's own DMA range, not the host's IOVA
  window at 0x20100000, and FSR 0x400 has no fault bits set in the
  driver's reading.  It does not stop the MHI exchange.

Run 021924 (kernel e7041365) sent a QMI control request,
GET_VERSION_INFO (7 bytes: `00 01 00 21 00 00 00`) on channel 2:

- The transfer was not drained: `channel 2: 5000 ms without a
  transfer completion`, `TX channel 2 stalls (last event ch 245
  code 1 bytes 0)`, and `QMI GET_VERSION sent: 60` (ETIMEDOUT).  The
  last event is still the START_CHAN completion, so the modem's QMI
  client was not listening ~25 s after its boot.
- The next run retries the request up to ten times, five seconds
  apart, and logs channel 2's read pointer on every try: if the
  engine drains the ring, the pointer moves even when the completion
  event is late.  The run lasts 130 s to give the modem's own boot
  (EFS especially) time to finish.

Still not started, and not claimed: SIM, calls and SMS.  The next
evidence to look for is a QMI answer on channel 3 -- type 0x02,
transaction 1, message 0x0021.

### Run 022525 and the vendor's channel table: QMI0 is 14/15 (2026-11-21)

Run 022525 (kernel 271264a1b08e) retried the request on the SBL's
SAHARA pair for ~50 s: channel 2's read pointer never moved
(`0x100000001` before and after every try), the last event stayed the
START_CHAN completion, no packet ever arrived on channel 3.  The
channel was simply the wrong one, and the vendor kernel's own sources
say which is right:

- `arch/arm64/boot/dts/vendor/qcom/kona-mhi.dtsi` (this phone's MHI
  channel table): `mhi_chan@2/3` is **SAHARA** with `mhi,ee = <0x2>` --
  the SBL's channels, gone after the handover.  `mhi_chan@14` /
  `@15` are **QMI0**, out on event ring 1, in on event ring 2,
  `mhi,ee = <0x4>`; `@16/@17` QMI1 (both er 3); `@20/@21` IPCR (er 2).
- The vendor's `net/qrtr/mhi.c` binds the **IPCR** channels, not
  QMI0/QMI1: QRTR rides 20/21 (kernel clients such as the ipc
  router).  The earlier note here that "QRTR runs on QMI0/QMI1" was
  wrong.
- The vendor's `drivers/bus/mhi/devices/mhi_uci.c` binds QMI0, QMI1,
  SAHARA, EFS, TF, DUN, LOOPBACK, and Android's own device nodes show
  the split: `/dev/mhi_0306_02.01.00_pipe_14` (QMI0) is owned by
  **radio** -- userspace QMI goes there -- while pipe_0/2/10/22 are
  root-only.  Android's dmesg puts MHI up at 13.68 s and the first
  QMI connection to the modem (`esoc0` SSCTL) at 15.8 s, about two
  seconds after the AMSS takes over.
- On QMI0 the wire format is the QMUX framing libqmi implements:
  `0x01`, a 16-bit length that counts everything after the marker,
  the QMUX flags/service/client bytes, then the QMI header (for the
  control service: type, transaction, message id, TLV length).  A
  bare QMI header, which 021924/022525 sent, is not what the modem's
  QMI client parses.

The driver's channel-context layout was checked against the vendor's
`struct mhi_chan_ctxt` and matches word for word: word 0 packs
chstate/brstmode (`0x201` = ENABLED + burst doorbell), word 1 is
chtype (1 out / 2 in), word 2 the event-ring index, then 64-bit
rbase, rlen, rp, wp at words 3/5/7/9.

`mhi_amss_start` now opens 14 (out, ring 1) and 15 (in, ring 2) and
sends the QMUX-framed CTL GET_VERSION_INFO, twelve bytes:
`01 0b 00 00 00 00 00 01 21 00 00 00`.  Eight tries, five seconds
apart; a transfer still untaken by the modem is re-announced to its
doorbell rather than doubled in the ring.  The evidence that would
matter next is any packet on channel 15 -- a QMUX CTL response
(type 0x02, transaction 1, message 0x0021) or, failing that, a
transfer completion on 14.

### Run 024351: QMI0 opens, and the AMSS still drains nothing (2026-11-21)

The right channels did not change the outcome.  Run 024351 (kernel
2fada913def5) opened 14 (out, event ring 1) and 15 (in, ring 2); both
START_CHAN commands completed with code 1, and the GET_VERSION was
posted and its doorbell re-announced on all eight tries over ~80 s.
Channel 14's read pointer read `0x100000001` before and after every
try -- not `rbase` (the pool sits at IOVA `0x20100000`, so ring
addresses are `0x2010....`), not anything the host had written -- and
neither a transfer completion nor a packet on 15 ever came.

What the same log says about the engine: command completions still
arrive on ER0, so the command ring and ECABAP/ER0 still work after
the handover; MHISTATUS 0x201 is READY + M0 with no SYSERR; MHICTRL
0x200 is M0.  MHICFG is `0x2060b80` -- NCH 0x80, NHWCH 0x0b, NER 6,
NHWER 2 -- the AMSS's own values, not the host's, which says the
AMSS re-initialized the MHI engine when it booted.  The vendor's
host knows this: `mhi_ready_state_transition` (mhi_pm.c) runs
*after* the device reports READY and before any channel is started.
It calls `mhi_init_mmio` again (the three context bases at 0x58-0x6c,
MHICTRLLIMIT/MHIDATALIMIT at 0x88-0x8c/0xa0-0xa4 from the host's
IOVA window, MHICFG's ring counts), resets every software event
ring's context to its last element and rings each event doorbell,
and only then sets M0.  This driver had done all of that once,
before the SBL -- never after the AMSS took the engine over.

Two implementation details checked against the vendor while reading
it: a channel doorbell's address is CHDBOFF + id*8 and the command
doorbell's is CRDB_LOWER, both written as two 32-bit halves
(`mhi_write_db`); and brstmode 2 (`MHI_BRSTMODE_DISABLE`) selects
`mhi_db_brstmode_disable`, which writes the doorbell on *every*
post, while 3 (ENABLE) rings once and then relies on the context's
wp.  The driver's `0x201` context state is ENABLED + brstmode 2, so
ringing every post is the vendor-matching behaviour.

The next kernel replays the ready transition after the handover --
context bases, windows, MHICFG, every event ring's pointers and
doorbells, M0 -- logs the register block the AMSS left behind and
channel 14's context word for word (state, type, er, rbase, rlen,
rp, wp), and, if the QMI sequence still gets no answer, asserts the
host's MHI reset once, waits for the READY pattern (`0x101`),
replays, and runs the sequence again.  SIM, calls and SMS remain
not started; the run's question is only whether the channel's read
pointer moves and channel 15 answers.

## Run 025442: the replay changed nothing, and the reset killed
## the register block

The kernel described above ran as 025442.  The AMSS stage survives
in the live snapshots -- the composed log drops its middle again,
but `logs/025442-run.log.live.22` holds the whole stretch, because
the loader keeps the first 256 KB plus a 1 MB ring and every
`.live.N` snapshot has the same retention.

What the dump taken after the handover showed:

- The AMSS did not touch the register side.  CCABAP `0x20100000`,
  ECABAP `0x20102000`, CRCBAP `0x20103000` and MHICFG `0x2060b80`
  are all still the host's own writes.  The earlier hypothesis that
  the AMSS re-initialized the engine is disproven; the replay
  rewrote the same values and changed nothing.
- The AMSS did write channel 14's context: state 2 (RUNNING) and
  rp `0x100000001` -- a value the host never wrote, so the AMSS's
  DMA to our context array works.  Its wp `0x2012a000` is still
  the host's last write; the AMSS never posted a TRE back, and no
  event ring's read pointer was ever observed to move.
- After the QMI sequence got no answer, the host's MHI reset
  (MHICTRL `0x2`) made MHICTRL, MHISTATUS and BHI_EXECENV read
  `0xffffffff`.  The AMSS does not survive a host reset, so the
  reset is not written again and `mhi_amss_reset()` is gone.

The open question left by 025442: is QMI0 simply not served by
this AMSS image (Android talks to the modem over QRTR/IPCR
instead), or is the data path itself down?  The next kernel starts
the IPCR pair (channels 20/21) and sends one QRTR HELLO -- one
transfer that completes tells QMI0-unserved from data-path-down,
and an answer would name the modem's node and announce its
services, UIM first among them.

## Run 030922: the data path is up, and the AMSS speaks QRTR v3

Kernel `b71e4be94b53` ran as 030922 and answered the question.
The QMI0 half reproduced 025442 exactly: channel 14's rp stayed
`0x100000001` through all three CTL GET_VERSION tries (`no answer
(try N: 60; TX rp 0x100000001 -> 0x100000001)`), and the last line
was `TX channel 14 stalls (last event ch 245 code 1 bytes 0)` --
the AMSS never drains that channel.  The dump after the QMI tries
showed every event ring still at its host-armed state; nothing had
been posted.

The IPCR half changed everything.  Channels 20/21 opened on
event ring 2, the HELLO transfer posted on channel 20 completed at
the host (`IPCR hello: TX completed`), and the AMSS advanced
channel 20's context rp to `0x200000001` -- it consumed the TRE.
Channel 21's rp then read `0x200000002`.  The answer arrived as 36
bytes:

```
03 02 00 00 14 00 00 00 02 00 fe ff ff ff fe ff 02 00 00 00
e3 1e 00 00 0a 00 00 00 00 00 00 00 00 00 00 00
```

That is not a v1 header.  The vendor's `qrtr_hdr_v2` (16 bytes,
`QRTR_PROTO_VER_2 = 3` in `qrtr.c`) parses it perfectly: version
3, type 2 (HELLO), size 20, source node 2, source port `0xfffe`
(the control port), destination node `0xffff` (broadcast),
destination port `0xfffe`.  The 20-byte control payload starts
`cmd 2` -- a HELLO -- announcing node 2 to the bus.  The first
version of the parser only knew v1 and printed the bytes as a
garbled packet; the layout match is what identified it.

After the IPCR stretch, event ring 2's rp had moved to
`0x20107020` (the AMSS had posted transfer events) and MHISTATUS
still read `0x201` -- the engine was healthy throughout.  Channel
14's rp was still `0x100000001`.

So: the MHI data path and the event path both work, and this AMSS
image simply does not serve QMI0's channels 14/15.  Android's
QMI reaches the modem over QRTR/IPCR (channels 20/21) -- which is
also what this phone's own Wi-Fi driver does -- and the modem's
node is 2, its control port `0xfffe`.  The next kernel uses the
v3 framing: it drains the control announcements, sends a
NEW_LOOKUP for the UIM service (`0x0b`) if no server is announced,
and then sends UIM GET_CARD_STATUS (`0x2f`) as a QRTR DATA frame
to the announced port -- the first real question about the SIM.

Note for the wire format: QMI over QRTR has no QMUX prefix.  The
DATA payload is the bare QMI message -- type `0x00` (request),
transaction id, message id, length, TLVs.

## Run 032040: the modem's whole service table, and UIM is not in it yet

Kernel `bb443f278ef7` sent the same version 1 hello and then drained
the announcements with a cap of eight, plus six more after a
NEW_LOOKUP.  Every announcement is a 36-byte version 3 frame from
`2:0xfffe` to `1:0xfffe`, and each NEW_SERVER names the node that
serves it -- **node 3**, not the nameservice's own node 2:

| service | port | | service | port |
|---------|------|-|---------|------|
| WDS `0x01` | `0x42` | | `0x24` | `0x11` |
| DMS `0x02` | `0x4b` | | `0x42` | `0x02` |
| NAS `0x03` | `0x3c` | | `0x44` | `0x4d` |
| QOS `0x04` | `0x43` | | `0xe4` | `0x33` |
| WMS `0x05` | `0x40` | | `0xffe4` | `0x08` |
| AUTH `0x07` | `0x51` | | | |
| CAT `0x08` | `0x45` | | | |
| `0x22` | `0x17` | | | |

Thirteen services, and **UIM (`0x0b`) is not among them** -- nor
voice (`0x0a`).  The lookup for UIM did not produce an announcement
either.  That is expected once you read the nameservice: since
Linux 5.10 `ctrl_cmd_new_lookup()` refuses any request whose source
node is not the nameservice's own local node (`from->sq_node !=
qrtr_ns.local_node`), so a lookup from node 1 is dropped by the
modem's ns, and no reply is owed.  What *does* work is the hello:
`ctrl_cmd_hello()` calls `announce_servers()`, which sends one
NEW_SERVER per registered server **directly to the node that said
hello**.  So a HELLO is a "give me the current table" request, and
repeating it over time is how a late-registering service is caught.

The table itself is a milestone: the data path carries both
directions, the modem's ns knows this host as node 1, its service
endpoints live at node 3, and SMS's own service (WMS `0x05`) is
already announced with a live port.  What is still missing is the
card stack.  The next kernel sends a hello every few seconds (up
to twelve rounds, the first in the version 1 shape that works,
the rest in version 3 straight at node 2), compares each round's
table against the last, and the moment UIM (`0x0b`) appears sends
it QMI GET_CARD_STATUS (`0x2f`) -- version 3 first, version 1
again if that goes unanswered -- the first real question about
the SIM.

## Run 033152: UIM is announced, and its answer queued behind the table

That kernel (e570372748cf) answered the open question first:
**UIM (`0x0b`) is served.**  It came as the 22nd frame of hello
round 0, `control cmd 4: service 0xb instance 1 node 3 port 0x3f`,
and a version 3 GET_CARD_STATUS went to `3:0x3f` with its TX
completed.  The modem's services are all there for the asking;
nothing is missing from the card stack's transport.

What went wrong is a drain cap, not the modem.  The query's read
loop stopped after eight frames, and all eight were still table
announcements -- the ns queues one NEW_SERVER per registered
server *per hello*, and this round's backlog ran past the eight:
`0x4c`:`0x31`, `0x0c`:`0x38`, `0x4d`:`0x45`, `0x4e`:`0x4f`, and
two endpoints on node 2 itself (`0xe` instance 513 port `0x5b`,
`0xf` port `0xf`) plus `0xf` on node 3 port `0x7`.  The QMI
answer sat behind them and was never read; no frame with QMI
message `0x2f` exists in the run or in any of its 87 live
snapshots.  The round-0 drain was also still stopping at UIM's
announcement (`if (uim_port != 0) break;`), which is the same
mistake one level up.

The next kernel drains by *kind*, not by count: a round reads up
to 48 frames and the query loop only breaks on a DATA frame
(`mhi_qrtr_type() == 1`), which is what the answer must be --
announcements are type 4.  The card-status parser was already
written (result/error, per-slot card state, per-application
type/state), so the next run either prints the card or prints
why the modem will not describe it.

## Run 034018: the drain was not the problem, the frame length was

That kernel (f4a7f12045fd) drained the whole table in round 0 --
47 frames, the modem's own HELLO first (`v3 type 2 size 20 from
2:0xfffe to 65535:0xfffe`, a broadcast reply), then one
NEW_SERVER per service, UIM `0x0b` among them at node 3 port
`0x3f`.  The round no longer stopped at UIM, the query drain was
raised to 48 frames and keyed on the DATA type, and it changed
nothing: the v3 request (23 bytes with its header) and the v1
request (40 bytes) both completed their TX and the modem sent
**not one frame** back.  Not the answer, not an error, not an
announcement -- `IPCR UIM: 0 answers`.

The frame was malformed, and upstream `net/qrtr/af_qrtr.c`
`qrtr_endpoint_post()` says how: before anything else it refuses
`len == 0 || len & 3` (the whole frame must be a multiple of
four), then requires `len == ALIGN(size, 4) + hdrlen`.  A QMI
GET_CARD_STATUS with no TLVs is seven bytes; with the v3 header
that is 23, which fails the first check, and with the v1 header
40 bytes -- but `size` said 7, so the vendor strict to the
second rule (or to plain equality) rejects it too.  The modem's
qrtr dropped both without a diagnostic.  The same source settles
two other questions: local delivery looks the socket up by
`dst_port` alone (`qrtr_port_lookup(cb->dst_port)`), and a
NEW_SERVER from a remote endpoint assigns `pkt->server.node` to
that endpoint's node -- a bridging node may announce services of
another node.  So `node 3` is the right address in the header
and the port was always the thing that counts.

The fix is the padding mpss.c's client always sent and this
probe did not: the request becomes eight bytes (a trailing zero
past the QMI message, whose own length stays 0), `size` says 8,
and both shapes satisfy both checks -- v3 is 24 bytes, v1 is 40,
both aligned and exactly `ALIGN(size, 4) + hdrlen`.  The TX
frames are logged now too (`IPCR UIM TX`), so the next run shows
the request it sent on the way to the card's answer.

## Run 035209: the modem answers, and says the card stack is not up yet

That kernel (2744d9440630) sent the padded request and the
modem's qrtr accepted it.  The TX frame went out exactly as
designed -- `03 01 00 00 08 00 00 00 01 00 01 10 03 00 3f 00 00
01 00 2f 00 00 00 00`: version 3, DATA, 24 bytes, from the host
`1:0x1001` to UIM at `3:0x3f`, QMI transaction `0x1000`,
GET_CARD_STATUS, eight bytes with the padding zero.  The answer
came back within the first receive window, `03 01 00 00 0e 00 00
00 03 00 3f 00 01 00 01 10 02 01 00 2f 00 07 00 02 04 00 01 00
02 00 00 00`: a QMI response (type 2, the request's transaction,
message `0x2f`), seven bytes of QMI body, one TLV -- type 2,
length 4, result 1, error 2.  The log said `IPCR UIM: 1 answers`
and `UIM card status: result 1, error 2`.

So the transport works end to end: the padded frame is delivered,
UIM receives it, and UIM's answer comes back with the host's
transaction id.  Error 2 is `QMI_ERR_NO_MEMORY` in the QMI error
table (`hakios.qmi.h`) -- the service's card stack was not ready
~60 s after the AMSS came up, which is what a UIM service says
while its card driver is still initializing, not a rejection of
the request.  A card-status answer would otherwise carry TLV
`0x10` with the slot and application table, and none came.

The next kernel asks twice as carefully: GET_VERSION (`0x21`)
first, which tells whether the UIM service is answering at all,
then GET_CARD_STATUS up to five times three seconds apart, each
with its own transaction id (`0x10` through `0x14`), and stops
as soon as an answer actually describes the card.  If the error
is simply time, one of the tries prints the card; if every try
says 1/2 while GET_VERSION succeeds, the card stack itself is
the next thing to look at (`mhi_uim_card_status()` already
prints slot and application state when TLV `0x10` arrives).

## Run 040057: NO_MEMORY is not about time, and not about the card

That kernel (85855a49b3a9) ruled both out.  GET_VERSION (`0x21`,
transaction `0x0002`) went to `3:0x3f` first and came back with
the same result TLV as everything else: **result 1, error 2**.
Then five GET_CARD_STATUS tries, transactions `0x0010` through
`0x0014`, three seconds apart: every one answered within the
first receive window, every one result 1, error 2, TLV `0x10`
never present -- `IPCR UIM: 6 answers`, all failures.  A service
answering FAILURE/NO_MEMORY to *Get Version*, which needs no
card, no slot and no state, is not a card stack that needs more
time; UIM's QMI dispatch itself refuses the request.

One wire-level detail seen in the retries: try 3's answer frame
had `flags` byte 1 (`03 01 01 00 ...`) -- the v3 header's
confirm_rx bit, the modem asking for flow-control confirmation.
The frame was still delivered and answered; the flag is not
required for the question to get through.

What is left is whether UIM alone is sick or every QMI service
refuses.  The next kernel asks the same GET_VERSION of WDS, DMS,
NAS and WMS at the ports the nameservice announced (the driver
now remembers service, node and port for the five of them), asks
UIM again in libqmi's exact framing -- size field 7, frame
padded to 24 bytes, which is what a real client sends and is one
byte different from what this probe has sent so far -- and once
from an ephemeral source port (`0x4001`) instead of `0x1001`.
The answers sort the possibilities: result 0 anywhere means the
QMI-over-QRTR path is healthy and UIM's own state is the
problem; result 1/2 everywhere, or only after the size-7 and
port change, points at the modem's QMI bridge rather than at
any single service.

## Run 041016: size 7, and the card describes itself

Kernel `92cd3ef6cbe0`.  The discriminator answered everything at
once:

- **The whole NO_MEMORY mystery was one byte of framing.**  The
  same GET_CARD_STATUS message (type 0, transaction `0x20`, id
  `0x2f`, length 0) went out as a 24-byte frame whose v3 size
  field said **7** -- the QMI message's own length, not the
  padded frame's -- and UIM answered with a full card status:
  `03 01 00 00 8c 00 00 00 03 00 3f 00 01 00 01 10 | 02 20 00
  2f 00 85 00 ...`, QMI type 2, the host's transaction, message
  `0x2f`, length `0x85`, result 0, error 0.  Every earlier size-8
  request (the pad byte counted in the size) had been answered
  result 1, error 2.  The transport was never sick; the size
  field was wrong.
- **The card status is phone #1's situation exactly**: primary
  GW application 255 (slot 255 -- `0xffff`, none bound), two
  slots; slot 1 `ERROR` (2), error 3, no applications; slot 2
  `PRESENT` (1) with one application, type 2 (USIM), state 7
  (`READY`), PIN1 state 3 (disabled), 3 retries.
- DMS GET_VERSION (node 3 port `0x4b`) answered result 0 with
  the 31-byte version TLV (`QUALCOMM INCORP...`): the
  QMI-over-QRTR path is healthy.  WDS (`3:0x41`) answered error
  9; NAS (`3:0x3c`) and WMS (`3:0x45`) did not answer within
  five seconds; UIM GET_VERSION -- in size-7 framing -- answered
  result 1, error 17, from both `0x1001` and `0x4001`, while
  GET_CARD_STATUS right beside it answered result 0.

So the card is there, READY, with a disabled PIN, and the only
thing missing is what phone #1 fixed in runs 31-33: logical slot
1 pointed at the slot with the card (UIM Switch Slot `0x46`:
TLV 1 the logical slot, TLV 2 the 32-bit physical one, both from
1), then the USIM bound to the primary GW session (UIM Change
Provisioning Session `0x38`: TLV 1 session type 0 and activate
1, TLV 0x10 logical slot 1 and the AID the card status carries
per application).  The next kernel replays both, reading the
card status again after each, and then asks DMS to take the
modem online (phone #1 boots offline; error 52 sends it the
offline -> low power -> online way) and NAS for the selection
preference, the registration (error 37 = UIM_UNINITIALIZED gets
ten seconds and a retry) and the system info.

## Run 042512: the switch and the provisioning are accepted

Kernel `f1dc9f37f185`.  Phone #1's sequence replayed exactly, and
its two UIM steps both worked:

- Card status: result 0, primary GW application 255 (slot 255),
  slot 1 `ERROR` (2) with no applications, slot 2 `PRESENT` (1)
  with one USIM, state 7 (`READY`), PIN1 disabled, AID 16 bytes
  `a0 00 00 00 87 10 02 ff 86 ff ff 89 ff ff ff ff`.
- **UIM Switch Slot** (`0x46`, logical 1 -> physical 2): result
  0, error 0.  The card status after it shows slot 1 `PRESENT`
  with the USIM (state 1, detected) and slot 2 `ABSENT` -- the
  card moved to the logical slot the modem watches.
- **UIM Change Provisioning Session** (`0x38`, session type 0,
  activate 1, logical slot 1, the USIM's AID): result 0, error 0.
  The card status after it: **primary GW application 0 (slot 0)**
  -- the USIM is bound to the primary subscription -- slot 1
  `PRESENT`, USIM state **7 `READY`**, PIN1 **disabled**, three
  retries.  `after the switch (1) and the provisioning (1):
  primary GW application 0`.

That is SIM recognition at the UIM level: the slot the modem
watches holds the card, the card is READY, and the subscription
points at it.  What is left is the modem's operating state:

- **DMS is OFFLINE** (mode 3, offline reason `0xffffffff`).
  Set Operating Mode ONLINE (0) answered result 1, error **52**
  (`QMI_PROTOCOL_ERROR_DEVICE_NOT_READY`) twice; Set Operating
  Mode LOW POWER (1) -- phone #1's intermediate -- answered
  error **60** (`INVALID_TRANSITION`).  The attempt was made
  eight seconds after the AMSS came up; phone #1's modem went
  online late in its own boot.
- NAS GET_SYSTEM_SELECTION_PREFERENCE (`0x34`) answered result
  0 with a 308-byte body (automatic selection, no preferred
  networks): unlike run 041016, NAS responds.  INITIATE_NETWORK_REGISTER
  (`0x22`, action 1) was not answered within five seconds, nor
  was GET_SYSTEM_INFO (`0x4d`) -- consistent with a modem that
  is still offline.

The next kernel keeps the UIM sequence and attacks DMS: it reads
the operating mode and asks for ONLINE every ten seconds for
seventy (the mode is logged each time, so a late device-ready or
a self-transition shows up), then, if still refused, tries
persistent low power (6) as the intermediate and ONLINE after
it.  The registration is sent again when it gets no answer at
all, not only on error 37.

## Run 043256: the third request takes the firmware with it

Kernel `25b90191ef19`.  The UIM path repeated run 042512 exactly --
slot 1 `ERROR`, slot 2 a READY USIM, Switch Slot (`0x46`) result 0,
Change Provisioning Session (`0x38`) result 0 with the AID,
primary GW application 0 -- and the AMSS answered UIM at
04:34:08, two seconds before the DMS block began.

DMS (04:34:11): mode 3 `OFFLINE`, offline reason `0xffffffff`.
Set Operating Mode ONLINE (`0x2e`, txn `0x90`) result 1, error
52; eleven seconds later txn `0x91`, same answer; then, at
04:34:32, txn `0x92` -- and from that request on the AMSS
answered nothing:

- GET (`0x2d`, txn `0x83`, 04:34:52): no answer.  Set ONLINE
  txn `0x93`: no answer, and while it waited (04:35:06.8) the
  log carried `MHI: fault at AMSS-MHI: status 0xffffffff,
  execenv 0xffffffff`.
- Every later frame -- GETs `0x84`-`0x87`, Set ONLINE
  `0x94`-`0x97`, persistent low power (6, txn `0xa0`), NAS
  selection / initiate register / initiate again / system info,
  and the final UIM card status -- failed in `mhi_send` with
  `TX no completion`.  The stage ended `MHISTATUS 0xffffffff,
  BHI_EXECENV 0xffffffff`: every MHI register read back all
  ones, the firmware's side of the engine gone.

Run 042512, by contrast, sent three mode requests -- ONLINE,
LOW POWER, ONLINE -- none repeated, and had faulted nothing
when it ended 27 seconds after the DMS block, `MHISTATUS 0x201,
BHI_EXECENV 0x2`.  The difference between the runs is the
repetition: in 043256 the third identical ONLINE request (21
seconds after the first) is the last request the firmware ever
answered, and the fault registers were read afterwards.

So repeated Set Operating Mode ONLINE is not a harmless retry
on this AMSS: a third one, ten seconds apart, is followed by
the firmware leaving the engine and the whole rest of the run
going nowhere.  The next kernel therefore: reads the mode
before the card is touched (so a refusal cannot be the
provisioning session's doing), watches it for a minute without
requesting anything (a late self-transition or a late
device-ready shows up in the log), then asks for ONLINE exactly
once, and reads the mode twice more to see whether it took and
whether the firmware survived the request.  The MHI registers
are logged immediately before that one request, so the fault,
if it comes, is bracketed.

**Correction (run 043256's frames, re-read at 10:5x).**  The
"repetition" reading above was wrong -- every one of the three
Set Operating Mode ONLINE requests in this run carried a
malformed message: `00 90 00 2e 00 04 00 01 01 00 00 00`, i.e.
QMI length **4** with the pad byte counted, which is the same
off-by-one that run 055654 was refused NO_MEMORY (2) for.  The
correct message is the 3-byte TLV `01 01 00` (length 3); umi's
proven host sends that form on this same modem roughly every two
seconds for sixty seconds with no fault, and run 062843 sent two
*clean* ONLINE requests on a healthy firmware (`MHISTATUS 0x201`
after them).  So what took the firmware was not the third
request as such -- it was the third **mis-framed** request, each
of which the AMSS had to parse as garbage before answering 52.
The retry policy now keeps the clean frame and repeats it; there
is no run on this modem that shows a clean repeat faulting it.

## Run 055654: the frame was refused before the modem looked at it

Kernel `d4e1eeab5a12`.  The TFTP announcement and the IPCR
lookups went out (`2 of the host's 2 services announced to node
2`, lookups `TX completed`), and **no TFTP request arrived** --
the modem never asked the host for a single file.  UIM worked
as before.  DMS was OFFLINE (mode 3, reason `0xffffffff`); Set
Operating Mode ONLINE was answered result 1, error **2**
(`NO_MEMORY`), before the card and again after it, and the run
ended with the firmware healthy (`MHISTATUS 0x201`).

Error 2 to a Set Operating Mode is not a state error -- the
frame could not be parsed.  The request sent was
`00 tt 00 2e 00 04 00 01 01 00 00 00`: message length **4**
where the message is 3 bytes (`01 01 00`), the same pad-byte
overcount the UIM notes record (run 041016: a size that counts
the pad byte is answered NO_MEMORY).  The modem echoes what it
read of the header (`message 0x042e`, length `0x0100` in the
run's own log line), which is how a shifted header presents.

## Run 060739: the length was still one byte off

Kernel `9b5c7191c3p0`.  Same announcements, same silence from
TFTP, same UIM path.  Set Operating Mode ONLINE was rebuilt to
`00 tt 00 2e 00 03 00 01 01 00` but the length field went out
as `04 01` little-endian = **0x0104**, length 260; the modem
answered result 1, error 2 again, before and after the card.
Firmware healthy at the end.  So the fix for 055654 was not
complete: the whole ten-byte frame has to be exactly
`00 tt | 00 2e | 00 03 | 00 | 01 01 00`.

## Run 062843: the right frame, a new refusal, and no TFTP client

Kernel `e7a0219b3c11`.  The frame above is right in this run --
the log shows `message 0x2e length 3`, and the modem **answers**
it, so the header parses.  The answers are result 1, error
**1** (`FAILURE`), before the card (txn `0x70`) and after it
(txn `0xa0`); the firmware stays healthy throughout
(`MHISTATUS 0x201, BHI_EXECENV 0x2` at the end).

Two facts point the same way:

- `2 of the host's 2 services announced to node 2` -- the host
  published **TFTP v1 only** (instance 1, port `0x3003`).
  tqftpserv publishes TFTP **twice, versions 1 and 3**, and the
  SDX55's client looks up **version 3**; the SIP-published
  versions umi's `md_tftp_announce` sends are `{4,4096,1,1}` and
  `{4,4096,3,1}`.  A v3 lookup against a host that never
  announced v3 finds nothing, which is exactly the run: the
  modem's nameservice never returned a server for 4096, the
  modem's TFTP client never sent an RRQ, and the radio stayed
  offline for want of its carrier configuration.
- The driver's own IPCR discovery lookup was sent as a
  version-1 hello addressed straight at node 2 rather than
  umi's transport-type-10 broadcast, so it could not have drawn
  the nameservice's answer either.

DMS's error 1 is consistent with the reference host's finding
that the radio only goes online after the modem has read its
carrier configuration over TFTP: a well-formed request on a
boot that has fetched nothing is answered FAILURE, not
DEVICE_NOT_READY, on this AMSS.  The next kernel publishes
TFTP as both versions, sends the discovery lookup in umi's
proven form, sources TFTP error replies from the port the
request was addressed to (v1 or v3, as tqftpserv does), and
keeps asking for ONLINE -- every refusal but success is
retried, because a boot that has not yet fetched its
configuration must not be taken as final.

## Run 105401: the modem's TFTP client arrives, and the OACK goes unanswered

The version 3 registration works: `3 of the host's 3 services
announced`, `IPCR TFTP lookup (v3): TX completed`, and for the
first time the modem's own TFTP client reaches the host.  The
log shows exactly **one** real request (the dozens of copies
in the rotating snapshots are overlapping windows, not
retransmits):

    tftp: the modem writes "/readwrite/server_check.txt": blocks of 7680, window 4

That is the SDX55's liveness write, the first TFTP act of a
stock boot.  Stock acknowledges it and discards it, and the
modem then reads its carrier configuration (`modem_pr/mcfg`)
before the radio can go online.  Here the request arrives,
the host logs it, builds an OACK -- and nothing follows: no
DATA block, no retransmission, no RRQ for the carrier
configuration.  DMS answers `message 0x2e, result 1, error 1`
throughout (3 before the card, all 12 after), and the
firmware stays healthy (`MHISTATUS 0x201`).

The OACK went out as a 54-byte frame.  Every other frame this
host sends the modem -- the QMI requests that *are* answered
-- is padded to a multiple of four (`mhi_qrtr_put3_sized`,
seven bytes of message in 24 bytes of frame), the modem's own
30-byte QMI answer arrives as 32, and umi pads every frame
`(32+size+3)&~3`.  Linux's qrtr-mhi refuses to transmit any
other length.  A 54-byte OACK is the one frame in the whole
exchange that breaks that rule, and it is the one frame that
draws no answer.  The next kernel sends every TFTP frame
through the same padded builder, and also answers the QRTR
flow-control **confirm bit** (flag bit 0) with RESUME_TX from
the receiving port, as Linux's `qrtr_send_resume_tx` and
umi's `md_qrtr_packet` do -- the modem's frames observed so
far carry flags 0, but a flow-controlled sender stalls until
the resume arrives.  The OACK's send result is now logged, so
a silent send failure can no longer hide.

## Run 112752: TFTP, the card and NAS all work; the ONLINE TLV was cut short

The padded frames fixed the TFTP handshake outright.  The
whole stock mcfg exchange runs: the modem writes
`server_check.txt` (5 bytes, kept), writes `mcfg.tmp`
(1 byte, kept) and reads it back, then reads
`modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig` and
`.../mcfg_sw/mbn_sw.dig` with windowed transfers, each
answered by an OACK whose send the log now confirms
(`the OACK for ...: 45 bytes of options, TX completed`).
The digest match against its EFS copy means the modem needs
no 40 MB download; the exchange completes.

The card stack comes alive behind it: `UIM slot 2: card
PRESENT (1)`, `application 0: type 2, state 7 (READY)`, the
AID `a0 00 00 00 87 10 02 ff 86 ff ff 89 ff ff ff ff` --
a USIM, read.  NAS answers `system info ... result 0, error
0`, its Initiate Register refused with error 37
(UIM_UNINITIALIZED), and the EFS session is offered and
acknowledged (1 region, 2 MB, discarded, as the reference
host does).  The firmware is healthy throughout
(`MHISTATUS 0x201, BHI_EXECENV 0x2`).

DMS alone still refuses: `set operating mode ONLINE` is
answered `result 1, error 1` (FAILURE), every time, before
and after the card.  The request is the defect, and the
modem's own answer proves it.  Its Get Operating Mode
response ends with the seven-byte result TLV followed by
**`01 01 00 03`** -- type 1, length 1, value 3 (OFFLINE),
inside an eleven-byte message whose header length counts
every byte.  Our Set request sent `01 01 00` as the whole
message: type 1, length 1, and **no value byte inside the
message** -- the mode falls outside it.  A modem that cannot
read the mode it was sent answers FAILURE, which is exactly
what every run since 062843 recorded.  The earlier reading
of run 043256 (`length 4` is the pad counted) was wrong:
length 4 is what the TLV needs; the pad is separate.  The
next kernel sends `00 tt 00 2e 00 04 00 01 01 00 00` --
eleven bytes, length 4, value 0, ONLINE.

## Run 114223: the request is whole, and the modem says DEVICE_NOT_READY

The eleven-byte set request changes the answer at once: `message
0x2e, result 1, error 52` -- `DEVICE_NOT_READY`, the reference
host's pre-online answer -- where every earlier run drew FAILURE
(1).  The modem now reads the mode it is sent; it is simply not
ready to take it.  Everything else the boot needs is in place in
this run: the whole TFTP exchange completes (no `NOT HERE` but
the expected read of `mcfg.tmp` before its write, no busy slot,
no failed send), the card is `PRESENT` and `READY` with its USIM
AID, the EFS session is offered, read (1 region, 2 MB,
discarded) and acknowledged, NAS `system info` answers result 0,
and the firmware ends healthy (`MHISTATUS 0x201`).

All fifteen ONLINE requests -- three before the card, twelve
after -- are answered 52.  The ordering explains why: the
modem's EFS session opens **after** the after-card verdict (line
8991 against 8970 in the same snapshot) and is acknowledged
seconds later, and nothing asks for ONLINE again once it is.
If the modem's boot holds `device ready` until that handshake
completes -- it is the last item of its boot the host takes part
in -- then no request before it could ever be accepted.  The
next kernel asks once more after the EFS sync, and registers NAS
again if the radio comes on (the register was refused error 37,
UIM_UNINITIALIZED, while the modem was still offline).

## Run 115721: the EFS handshake is not the gate; LOW POWER was never asked

The first kernel that asks for ONLINE after the EFS sync settles
the question run 114223 left open.  The EFS session opens,
offers and acknowledges its one region (2 MB, discarded) exactly
as before, and the eight ONLINE requests that follow it are
answered `error 52` like the fifteen before them: **the EFS
handshake is not what holds `device ready`**.  The rest of the
boot is clean -- TFTP serves `server_check.txt`, `mcfg.tmp`
(written, read back), `mbn_sw.dig` and `mbn_hw.dig` from the
read-only tree with no missing file; slot 2 is a `PRESENT`,
`READY` USIM; NAS `system info` answers result 0; the firmware
ends healthy (`MHISTATUS 0x201`, `BHI_EXECENV 0x2`).

The missing piece is in the reference driver
(`drivers/modem/hakios.mpss.c`, `dms_online`), whose comment is
about this same AMSS: *a modem that boots offline does not go
straight online; Qualcomm's DMS allows offline -> low power ->
online, and a direct ONLINE is refused error 52 -- ModemManager
and qmicli go the long way.*  Its `dms_online()` asks ONLINE,
and on 52 asks **LOW POWER (mode 1)**, waits two seconds, then
asks ONLINE again.  Every run since 112752 has only ever asked
ONLINE at mode 0; the mode-1 request has never been sent once.
umis `md_qmi_status` retries on exactly the same error 52 --
the same detour, stated as a retry.  The next kernel carries
`mhi_dms_online()`: ONLINE first, and on 52 the LOW POWER
request, a two-second pump, and ONLINE again, at each of the
three ask sites (before the card, after it, after the EFS sync).

## Run 121008: LOW POWER goes out (and is refused), and the carrier-config scan begins

The first kernel with `mhi_dms_online()`.  The detour now runs at
all three ask sites: after every ONLINE answered 52, the request
goes **LOW POWER (mode 1)**, two seconds pass in the pump, and
ONLINE is asked again.  Three sites, 3 + 12 + 8 ONLINE attempts,
and **every LOW POWER is answered `result 1, error 60`** -- the
one error this project has no table for; DMS refuses the mode
change with a code that is not 52.  ONLINE stays 52, and every
GET reads `operating mode 3 (OFFLINE), offline reason 0xffffffff`.

What did change is the modem's own work.  For the first time it
performs the **mcfg carrier-config load**: it reads `mbn_sw.txt`
and `oem_sw_j11.txt`, then works through
`mcfg_sw/generic/<region>/<carrier>/.../mcfg_sw.mbn` one file at
a time -- 23 distinct configs in the run, 22 to 65 KB each, every
one served with an OACK and `blocks gone` (AF/WE/EG, APAC/Idea,
APAC/DCM, APAC/KDDI, China/CT/Lab, China/CU/Lab, EU/A1, EU/DT...).
The scan is alphabetical because it follows `mbn_sw.txt`, which
lists **267 entries**; at the ~3 s per config the snapshots show,
the whole load is a 13-minute job.  It also writes its processed
configs back as `/readwrite/mcfg_sw<HASH>` (seven distinct names
in all, counting `server_check.txt` and `mcfg.tmp`).

Two host faults cut the experiment short.  The kept store was
four slots of 64 KB, so after the first four names every further
write was logged `NOWHERE` -- including
`mcfg_sw60C83F9DF640D4BAC0252FA2115C36F2`, 11350 bytes -- and a
name the modem wrote could not be read back.  And the eight
after-EFS ONLINE attempts ran out **while the scan was still
reading**: the last config was read ten lines before the
"still offline after 8 ONLINE requests" verdict, and the stage
ended there.  Asking stopped before the modem finished the very
load the reference says gates the radio.

The next kernel answers both: the kept store is eight slots of
128 KB, replacing the oldest name when they run out, and storing
nothing for the empty writes with which the modem asks whether a
name takes writes; and the after-EFS watch asks ONLINE for up to
240 rounds (each through the LOW POWER detour), stopping only
after the modem has been silent **and** refusing for five rounds
running.  `XL_SECS` goes to 1500 so the scan has time to end.

## Run 123149: the kept store holds, the scan ends, and the card is never asked again

The first run with both host fixes.  The kept store works: five
processed carrier configs are written back as `/readwrite/mcfg_sw<HASH>`
(5639 to 11350 bytes), every one logged `kept`, the empty probe writes
are logged `nothing, not stored`, no name is ever logged `NOWHERE`, and
a name the modem writes is read back afterwards (`mcfg.tmp`, the hash
configs).  The carrier-config load runs to its end this time: **60
distinct configs** served, the last read `SEA/YTL/Commercial` (entry
~262 of the 267 in `mbn_sw.txt`; only `Test/*` and `common/*` follow),
and the modem then goes quiet -- the scan is not the gate either.  The
EFS sync completes again (1 region, 2 MB, acknowledged).

The after-EFS ONLINE watch (240 rounds budgeted) ran **192 requests**
across the scan's end, into the quiet: every ONLINE is answered
`error 52`, every LOW POWER `result 1, error 60`, and every GET reads
`operating mode 3 (OFFLINE), offline reason 0xffffffff`.  The watch
ends itself -- five silent rounds in a row after the modem stops
talking.

The SIM tells a new story.  The switch goes out while the modem is
still reading its carrier configuration and is answered `result 0`; the
status two seconds later shows **both logical slots ABSENT** -- the
card powering down to initialize again -- and that single re-read is
the *last* card-status request before the NAS block's lone retry a
minute on, which also shows no application.  The provisioning never
runs (`provisioning (0)`), the primary GW application stays 65535, and
NAS refuses the registration `result 1, error 37` (UIM_UNINITIALIZED)
both times.  The reference host re-reads the status 6 s after the
switch and then every 10 s through its 90 s window, and provisions the
moment a READY (or only detected) application shows in logical slot 1;
this run asked twice and gave up before the card was back.

The next kernel asks the status every five seconds for six tries after
the switch, re-sending the switch (at most three in all) if the mapping
reverts to the card first found in another slot; it provisions and
confirms as soon as an application appears; and the after-EFS watch
probes the card one round in eight (about every 24 s, 30 probes over
the run), provisioning late when the AID shows up, confirming the
primary GW application, asking NAS to register again once it stands,
and switching again if the card comes back in slot 2 while logical
slot 1 is empty.

## Run 125953: the card binds, and a "busy" TFTP answer stops the config scan

The first run with the UIM polling and the late watch, and the card work
succeeds end to end.  The switch is answered `result 0`; five seconds
later both logical slots read ABSENT (the card initializing); ten seconds
later logical slot 1 describes **application 0: type 2, state 1
(detected)**, AID `a0 00 00 00 87 10 02 ff 86 ff ff 89 ff ff ff ff`.
The provisioning goes out at once, the confirmation 2 s later reads
**primary GW application 0 (slot 0)**, slot 1's application state 7
(READY), and the log says `THE PROVISIONING TOOK`.  It holds through the
EFS sync: `after the EFS sync the subscription stands on primary GW
application 0 (slot 0)`.  NAS's registration changes with it -- error 37
(UIM_UNINITIALIZED) becomes **error 3 (INTERNAL)** -- so the card is no
longer what refuses; the modem is.

Two host-side failures cut the run short.  The first is the TFTP slots.
The modem asks twice for one file (its client retries when the answer is
slow); in 123149 both copies always ended -- one with the last block's
ACK (`blocks gone`), the other with the modem's own EOT error, which the
ack handler frees **silently** (code 9 is not logged).  This run's copies
did not get that ending: `server_check.txt` and `mcfg.tmp` writes, a
1-byte `mcfg.tmp` read, two `oem_sw_j11.txt` RRQs and two `mbn_sw.dig`
RRQs sat in the four slots, and the second RRQ of `mbn_sw.txt` was
answered **`no free transfer` / TFTP ERROR "busy"**.  After that answer
the modem read **no carrier configuration at all** -- 4 distinct files,
zero `generic/` entries, against 123149's 229 reads and 60 distinct
configs.  The scan never starts, the modem has nothing to say, and the
after-EFS watch's five-silent-round exit ends the stage nine rounds in
(12 + 9 ONLINE requests; every ONLINE `error 52`, every LOW POWER
`error 60`).  Silence was not a fault: the modem only speaks when spoken
to, and 123149's watch only looked talkative because the scan was
running.

The next kernel (a) treats an RRQ/WRQ that repeats a live transfer's
address, name and direction as the same transfer, restarting and
answering it from its own slot instead of taking a second; (b) when
every slot is nevertheless out, makes way with the read that has moved
least (never a write) and answers, so no read is ever refused `busy`;
(c) carries eight transfer slots instead of four; (d) drops the watch's
silent-round exit -- its 240 rounds are the budget and the scene clock is
the only cut; and (e) asks NAS to register one round in sixteen during
the watch, logging the answer when it changes, so the error 37 -> 3
transition and whatever follows ONLINE are both on record.

## Run 133235: the TFTP slots hold, and the pump turns out to be the clock

The first run with the eight-slot TFTP bookkeeping (duplicate coalescing,
least-progressed eviction).  Every symptom 125953 named is gone: 172
reads served, not one `no free transfer`, not one `busy` answer, and
every `mcfg_sw<HASH>` read that answered HERE was written back and kept.
The card work still holds -- the switch is answered, application 0 state
7 (READY), the provisioning stands as **primary GW application 0 (slot
0)** through the EFS sync -- and NAS still answers **error 3** (INTERNAL)
to the registration request once the card is bound, against 37
(UIM_UNINITIALIZED) before it.  Every ONLINE in the after-EFS watch is
still **error 52**, every LOW POWER still **error 60**, and the stage ends
on the scene clock (MHISTATUS 0x201, BHI_EXECENV 0x2 at 13:39:01).

The scan ran this time, but only just: 35 of `mbn_sw.txt`'s 267 entries
were served in the watch's 240 rounds, and the last reads were still in
`China/`'s tail and `SEA/YTL` when the rounds ran out.  The cause was in
our own pump, not in the modem.  `mhi_qrtr_pump` counted *every* receive
attempt against its six-attempt budget, each attempt waiting up to 100 ms
on an empty channel -- so one pump call took about 600 ms whether or not
anything was queued, and the scan moved at a handful of frames a second.
A pump that is meant to drain a busy channel was pacing it.  The pump now
takes a queued frame at once and counts only the wait that finds the
channel empty (budget 3 s per call, 4000-frame cap as the flood guard).

The watch's exit was reworked with it.  The modem only speaks when asked,
so rounds with nothing heard are normal even when it is healthy; five of
them were written off as an exit once already (125953).  The watch now
needs **thirty consecutive quiet rounds** -- each one having sent its own
ONLINE request -- and forty rounds of history before silence may end it,
with 900 rounds as the backstop, and the scene clock raised to 2100 s.
If the scan is what the modem is waiting on, this run gives it the whole
scan inside one watch.

Nothing else in the QRTR traffic changed: the registry is asked about
`tms/pddump_disabled` (no domains) and `avs/audio` (one domain, answered),
no NEW_LOOKUP comes in, and the cmd-6 control frames from node 2's
nameservice name its own client (node 3, ports 0x5a-0x60) -- flow
control, not a gate.  Over forty services announce, among them the full
phone set on node 3 (WDS 0x1, DMS 0x2, NAS 0x3, WMS 0x5, UIM 0xb, PDC
0x24) and, on node 2, IPA 0x31 instance 257 at port 0x13, RMTFS 0xe
instance 513, and 0x138b.

The open lead against error 52: this modem has never asked the host for
any `/modem_pr/so/*.mbn` Hexagon overlay.  Those 40-plus files are
EM_HEXAGON (e_machine 0xa4) DSP modules, and on the reference phone the
modem read `/modem_pr/so/665_0_0.mbn` before its first accepted ONLINE.
The boot image does carry the 84 MB `qdsp6sw.mbn` through Sahara
(`READ_DATA id 8`), so the DSP itself is loaded; what is missing is the
overlay request.  Whether the modem only asks once the carrier config
lands (which this run finally lets complete) is what the drain pump is
meant to answer.

## Run 164316: the drain pump, and the carrier-config load never starts

The first run with the pump that takes a queued frame at once (133235's
fix), the NAS registration asked from inside the after-EFS watch, and
the scene clock at 2100 s.  The card end works: the switch is answered
`result 0`, the card comes back READY in logical slot 1, the
provisioning takes at tl 205.5 s (`primary GW application 0 (slot 0)`),
the subscription stands through the EFS sync, and NAS answers **error 3**
(INTERNAL) at tl 242.1 s -- the card is not what refuses.

The carrier-config load never begins.  The dig is served cleanly at tl
192.2 s -- `mbn_hw.dig` then `mbn_sw.dig`, `0 asked`, `32 asked`, both
ending `1 blocks gone` -- and after it the modem asks for **no config at
all**, not `oem_sw_j11.txt`, not one `generic/` entry, nothing for the
rest of the run.  The card status and the switch went out at tl 198.3 s,
**6.1 s after the dig**, because the before-card ONLINE loop's three
pump rounds now each wait out the full drain budget on an empty channel
(three get-mode rounds at 188.3, 194.9 and 198.3 s).  In the three runs
whose scan did start (121008, 123149, 133235) the status and the switch
went out **at** the dig, and the first config read followed 1.4 to 4.4 s
later.  Nothing in this run pointed at that boundary yet -- the pause
after the dig has been read as the modem's boot being over since 125953
-- so the mode-change order, not the card timing, was blamed.

DMS: the mode change now runs ONLINE first and LOW POWER only after a
52.  Before the card, 3 ONLINE requests, all refused -- 52 early, **60**
(INVALID_TRANSITION) after the card, and 60 again after the EFS sync;
41 ONLINE requests in all, every one through the detour, the last at tl
422.3 s.  The modem stays OFFLINE (mode 3, offline reason 0xffffffff)
from the first get to the last.

## Run 221518: the vendor pair, forty times, and not one answer changes

The first run that sends qcril's vendor 0x0073/0x0074 pair between LOW
POWER and ONLINE, with every attempt: forty LOW POWER requests, all
refused **60**, and the pair never moved anything -- the ONLINE after
the EFS sync is still **52**.  Only the tail of this run survives in
the ring snapshot (tl 384.9 to 562.4 s): the EFS sync is late here (tl
397.1-398.1 s, against roughly 230 s in every neighbouring run), the NAS
registration during the watch answers **error 3** at tl 408.1 s, and the
dig and the card block predate the surviving window, so this run says
nothing about the scan.  It is the run the warm-up was added after; the
pair stays for one more run and is then dropped.

## Run 233725: the warm-up and the pair, and the scan stays away

The first run with the eight-message DMS warm-up (the qcril sequence
captured from the stock socket: event report, vendor 0x0003, get
capabilities, vendor 0x005e, Set AP Version `MIUIOS3.0.10.0.WPACNXM`,
get mode, get manufacturer, vendor 0x0003 reports), run once per boot
before the first mode change; the vendor pair stays with every attempt.
Every LOW POWER is answered **60**, every ONLINE **52**, from the first
ask at tl 186.7 s to the verdict at tl 425.0 s after 41 ONLINE requests.
The warm-up changes no answer.

The card end works again (status tl 196.3 s, switch tl 196.6 s,
provisioning takes at tl 203.6 s, NAS **error 3** at tl 241.6 s, EFS
sync tl 228.0-228.8 s).  The dig completes at tl 190.0-190.5 s and the
scan never begins: the status and switch went out **6.0 s after the
dig**, because the warm-up plus the three 2.5 s pump rounds sit between
the get mode and the card block.  The pair is blamed for the missing
scan, and the next kernel drops it.

## Run 005928: the pair out, the warm-up in, and a watch that cuts itself off

The first run without the vendor pair (the warm-up stays).  The pair is
innocent: the scan still never begins.  Warm-up first at tl 186.45 s,
the dig served at tl 189.1 s (`mbn_hw.dig`) and tl 190.1 s
(`mbn_sw.dig`), both `1 blocks gone`; after it, silence -- no
`oem_sw_j11.txt`, no `mbn_sw.txt`, no config, ever.  The card status and
the switch went out at tl 195.1 s, **5.0 s after the dig**, the warm-up
and the pump rounds having pushed the card block back.  The card end
works (provisioning takes at tl 203.1 s, NAS **error 3** at tl 212.4 s,
EFS sync tl 231.1-232.2 s).

The 150 s of quiet after the EFS sync pass without a frame, and the
first paced ONLINE goes out at tl 400.6 s -- LOW POWER refused **60**,
ONLINE **52** -- and the watch exits at tl 404.9 s: *"the modem is still
offline after 1 ONLINE requests"*.  The quiet counter had been
accumulating through the grace, so the silence that is normal before
the asking began was read as the exit's thirty-round silence the moment
the first ask went out.  The next kernel resets the counter when the
asking starts and will not end the watch on silence until twenty paced
requests have gone out.

Line up the six runs by one number and the scan boundary is clean:

| run    | `mbn_sw.dig` done | UIM status sent | gap   | first config |
|--------|-------------------|-----------------|-------|--------------|
| 121008 | 191.09 s          | 191.09 s        | 0.0   | 192.84 s     |
| 123149 | 191.30 s          | 191.07 s        | -0.2  | 192.66 s     |
| 133235 | 190.74 s          | 190.74 s        | 0.0   | 195.14 s     |
| 164316 | 192.23 s          | 198.32 s        | +6.1  | never        |
| 233725 | 190.54 s          | 196.28 s        | +5.7  | never        |
| 005928 | 190.06 s          | 195.07 s        | +5.0  | never        |

That table read as a slot rule: the carrier-config load only ever starts
when logical slot 1 already points at the card's slot when the dig
finishes -- the status and the switch at the dig in every scanning run,
five to six seconds after it in every run that never read a config.  So
the kernel after 005928 sends the card status and, when the card sits in
another slot, the switch at the **top of the boot**, right after the
first DMS get mode, before the warm-up and the pump rounds that had been
delaying the card block, and leaves the rest of the card work where it
was.  The watch keeps its 150 s of quiet and the 30 s pacing, and its
exit now requires twenty paced requests as well as thirty quiet rounds.

## Run 015246: the switch at the top of the boot, and the scan still away

The first run with the UIM status and switch at the top of the boot and
with the watch exit fixed.  The switch goes out at tl 187.31 s, txn 113,
result 0, logical slot 1 to the card's slot 2 -- **0.94 s before** the
dig finishes (`mbn_hw.dig` and `mbn_sw.dig` both `1 blocks gone` at tl
188.26 s).  The slot rule predicts a scan.  There is none: no
`mbn_sw.txt`, no `oem_sw_j11.txt`, not one `generic/` config for the
whole run.  **The switch before the dig is not the trigger**; the card
being pointed at its slot does not make the modem load its carrier
configuration.

The card end is the cleanest yet: status at tl 196.27 s shows slot 1
`PRESENT`, application state 1 (`DETECTED`), USIM AID, slot 2 `ABSENT`;
the provisioning request (0x38, txn 75) is answered result 0 at tl
196.27 s; the confirmation at tl 198.28 s reads state 7 (`READY`) and
primary GW application 0 -- *"THE PROVISIONING TOOK: primary GW
application 0 (slot 0)"*.  NAS during the watch answers **error 3**,
the EFS sync opens at tl 245.0 s.

The watch fix holds: 151 s of quiet, the first mode change at tl
396.17 s, then LOW POWER / ONLINE every ~31 s, twenty requests in all,
and the verdict at tl 987.07 s -- *"after the EFS sync, the modem is
still offline after 20 ONLINE requests"*.  No early cut, and no answer
changes: ONLINE 52, LOW POWER 60, the mode stays 3.

Line up the **first Set Operating Mode request after the dig** -- not
the card's switch, not the total number of requests -- and the boundary
is clean again, and it survives the control:

| run    | `mbn_sw.dig` done | first Set ONLINE after it | first config |
|--------|-------------------|---------------------------|--------------|
| 133235 | 190.74 s          | 190.74 s (+0.00 s)        | 195.14 s     |
| 123149 | 191.30 s          | 192.66 s (+1.36 s)        | 192.66 s     |
| 121008 | 191.09 s          | 192.84 s (+1.75 s)        | 192.84 s     |
| 233725 | 190.54 s          | 193.55 s (+3.01 s)        | never        |
| 164316 | 192.23 s          | 195.33 s (+3.10 s)        | never        |
| 005928 | 190.06 s          | none until 400.6 s        | never        |
| 015246 | 188.26 s          | none until 396.2 s        | never        |

The earlier reading blamed the ONLINE storm as well, but no run had ever
put a Set request inside the two seconds after the dig: 164316's first
post-dig ONLINE went out 3.1 s after it and 233725's 3.0 s, and the
forty-one requests each of those runs sent came later, from the watch
after the EFS sync.  In the three scanning runs the modem reads
`mbn_sw.txt`, `oem_sw_j11.txt` and the first configs **while the Set
ONLINE request is pending** -- the read is logged in the same tl batch
as the request -- and the answer is still 52.  So the request does not
have to be accepted to start the load; it has to arrive in the window.

The next kernel flags the last ACK of `mbn_sw.dig` in the TFTP server
and runs the before-card pump in 250 ms slices; the moment the flag is
set it puts the same eleven-byte Set ONLINE frame (`00 58 00 2e 00 04
00 01 01 00 00`) on the wire -- within 0.25 s of the ACK -- then runs
the LOW POWER detour as the scanned runs did.  The early switch stays;
run 015246 shows it is harmless, and the card end it enables is the
best recorded.

## Run 021748: the Set ONLINE at the dig, and the switch-to-absent window

That kernel ran as 021748 and it closed the "the Set ONLINE request has
to arrive in the window" reading.  The dig ACK (`mbn_hw.dig` and
`mbn_sw.dig` both `1 blocks gone`) lands at tl 188.72 s; the eleven-byte
Set ONLINE (txn 0x58) goes out **0.375 s later** at tl 189.10 s, inside
the window by any measure.  The answer comes back in 28 ms: result 1,
**error 52** (`DEVICE_NOT_READY`).  The LOW POWER detour (0x59) is
refused 60 as always.  The modem then reads **no** config at all: no
`mbn_sw.txt`, no `oem_sw_j11.txt`, not one `generic/` file -- and the
status at tl 189.13 s reads mode 3, offline reason 0xffffffff.
**A Set ONLINE request placed inside the window is not sufficient.**
The scan is not started by the request, so the earlier table's boundary
was a coincidence of these two facts travelling together.

What every scanned run has in common instead, checked line by line at
the dig:

| run    | dig ACK   | card status #1 after the dig | switch 0x46 (logical 1 to slot 2) | next status | its answer | carrier load in that batch |
|--------|-----------|------------------------------|-----------------------------------|-------------|------------|----------------------------|
| 121008 | 191.09 s  | txn 64 at +0.00 s            | txn 67 at +0.00 s, result 0       | txn 68 at +1.75 s | slot 1 ABSENT, slot 2 ABSENT | `mbn_sw.txt` + `oem_sw_j11.txt` |
| 123149 | 191.30 s  | txn 64 at +0.00 s            | txn 67 at +0.00 s, result 0       | txn 68 at +1.36 s | slot 1 ABSENT, slot 2 ABSENT | `mbn_sw.txt` + `oem_sw_j11.txt` |
| 133235 | 190.74 s  | txn 64 at +0.00 s            | txn 67 at +0.00 s, result 0       | txn 68 at +4.40 s | slot 1 ABSENT, slot 2 ABSENT | `mbn_sw.txt` + `oem_sw_j11.txt` |

Card status #1 at dig+0 reads the physical slot ready -- *slot 1 card
ERROR (2), error 3; slot 2 card PRESENT (1), one application, state 7
(`READY`)* -- the switch moves logical slot 1 onto it, the card powers
down and initializes again, and the very next status, answered 1.4 to
4.4 s later, reads **both slots ABSENT**.  The modem reads the whole
carrier table in the same batch that status is answered.

The control runs miss one step or the timing:

| run    | switch 0x46 | next status | its answer | carrier load |
|--------|-------------|-------------|------------|--------------|
| 164316 | +6.15 s     | +10.92 s    | slot 1 PRESENT, 1 application | never |
| 233725 | +6.01 s     | +11.03 s    | slot 1 PRESENT, 1 application | never |
| 005928 | +5.01 s     | +10.02 s    | slot 1 PRESENT, 1 application | never |
| 015246 | -0.94 s     | +8.02 s     | slot 1 PRESENT, 1 application | never |
| 021748 | before dig  | +12.39 s    | slot 1 PRESENT, 1 application | never |

In those runs the switch and the status are 5 to 8 s after the dig, and
by the time the status is asked the card has already re-initialized:
slot 1 reads PRESENT.  **The modem loads the carrier table when a card
status is answered ABSENT right after the switch** -- a switch alone is
not enough (015246, 021748), a status alone is not enough (015246's card
block, every control run), and a Set ONLINE at the dig is not enough
(021748).  It is the switched-but-not-yet-visible card, seen by the
status query, that starts `mbn_sw.txt`, `oem_sw_j11.txt` and the
`generic/` configs.

The next kernel reproduces exactly that at the dig: status (0x70), the
switch (0x71) when slot 1 is not yet pointed at the ready slot, a 1.5 s
wait so the card is still initializing when the next status arrives,
that status (0x72), then the eleven-byte Set ONLINE and the LOW POWER
detour.  Run 021748's card end is untouched by its Set ONLINE at the
dig: the status at +12.39 s reads slot 1 PRESENT with one application,
and at +15.00 s the card block reports the provisioning done and
primary GW application 0, as in 015246.  Its watch runs its whole
course: 150 s of quiet after the EFS sync, the first paced ask at tl
401.96 s, the twenty LOW POWER requests all answered 60, and the
verdict `still offline after 20 ONLINE requests` at tl 988.72 s.

## Run 024138: the dig came after the window closed

The first run of the dig-window kernel came back a control, and the
cause is in its own timeline: the digit `mbn_hw.dig` ACK lands late at
tl 203.14 s, `mbn_sw.dig` at 204.14 s -- but the branch that waits for
it only pumped forty quarter-second slices, ten seconds.  The loop had
already walked into the get-mode retries and the card block when the
ACK arrived; the card block's own status (txn 64) and switch (txn 67)
are stamped with the sw-dig batch at 204.14 s, and the branch's
markers are absent: no status txn 0x70/0x72, no `at the dig` Set
ONLINE.  No config read followed; the EFS sync opens at 258.83 s.

So the run says nothing about the card sequence -- it is a fifth
control: switch and status after the dig, card visible (slot 2 ready at
the status), no load.  It was stopped once its dig window had passed,
before its watch could finish, since nothing more was expected of it.
It also shows the boot's own clock drifting
later (dig 204.1 s here against 188.2 to 192.2 s in the earlier runs),
so the budget cannot be a guess.  The next kernel waits ninety seconds
for the flag, breaks out of the pump the moment the sequence is sent so
the card block follows at once, remembers that it switched (the card
block then waits with status 0x45 for the card instead of switching a
second time), and keeps the 1.5 s gap between switch and status.

## Run 025336: the branch fired, but the card was already back

The dig-window kernel's branch fired exactly as designed -- and the
experiment still came back a control, because the fixed 1.5 s wait put
the second status on the wrong side of the card's power cycle.

The timings, all against the `mbn_sw.dig` ACK at tl 201.93 s:

| step | txn | tl | offset |
|------|-----|----|--------|
| status 0x2f | 112 | 202.46 s | dig+0.53 s |
| switch 0x46 | 113 | 202.46 s | dig+0.53 s, result 0 |
| status 0x2f | 114 | 203.46 s | dig+1.53 s |
| Set ONLINE 0x2e | 88 | 203.46 s | dig+1.53 s, result 1, error 52 |

The first status repeated the scanning runs' picture -- *slot 1 card
ERROR (2), error 3, no applications; slot 2 card PRESENT (1), one
application, state 7 (`READY`), PIN1 state 3, AID 16 bytes; primary GW
application 255 (slot 255)* -- and the guard switched logical slot 1
onto the ready slot 2.  But the second status, 1.53 s later, already
read the other way: *slot 1 card PRESENT (1), one application, state 1
(`detected`), PIN1 state 0, retries 0, AID 16 bytes; slot 2 card ABSENT
(0); primary GW application 255*.  The card had re-initialized and was
visible again; in the three scanning runs the same status, asked 1.4 to
4.4 s after the switch, still read both slots ABSENT and the modem
loaded the carrier table in that batch.

No `mbn_sw.txt`, no `oem_sw_j11.txt`, no `generic/` config read.  The
Set ONLINE at the dig answered 52 again and the LOW POWER detour 60.
The EFS sync opened at 234.80 s, the watch ran its quiet stretch
(its last line at tl 404.37 s), and the run was stopped there -- the
branch result was fully read out and nothing further was expected of
it.

So the switch sequence is right and the 1.5 s gap is wrong: the
ABSENT transient is not a fixed width, and an answer of PRESENT with
the application back in `detected` does not start the load.  The next
kernel polls instead: after the switch it asks the card status every
0.7 s, up to eight times (transactions 0x72 through 0x79), and breaks
the moment an answer reads no card in either slot (`card_present == 0`
and no logical AID); only then does the Set ONLINE go out.  If every
poll sees the card, the sequence ends after 5.6 s and the run is a
control with the polling data recorded.

## Run 030228: the card never leaves, so the polling is a control

The polling kernel flew and the experiment answered its question in
the negative: this boot the card never went absent, the poll never
broke early, and the Set ONLINE at the end of the 5.6 s answered 52
anyway -- with a `READY` card that the host then provisioned
successfully.

| step | txn | tl | result |
|------|-----|----|--------|
| status 0x2f | 112 | 201.42 s | slot 1 ERROR (2)/3; slot 2 PRESENT READY state 7 PIN1 3 AID 16; primary GW 255 |
| switch 0x46 | 113 | 201.42 s | result 0, logical slot 1 -> physical 2 |
| polls 0x2f | 114-121 | 202.56-207.38 s | every ~0.7 s: slot 1 PRESENT (1), state 1 (detected), PIN1 0, AID 16; slot 2 ABSENT |
| Set ONLINE 0x2e | 88 | 207.43 s | result 1, error 52 |
| Set LOW POWER 0x2e | 105 | 207.43 s | result 1, error 60 |
| Set ONLINE again | 121 | 207.43 s | result 1, error 52 |
| change provisioning 0x38 | 75 | 207.90 s | result 0; primary GW application 0 (slot 0) |

The card block then read the card `READY` (state 7, PIN1 3) at
209.44 s; a card status after the EFS sync (274.97 s) still reported
the subscription on primary GW application 0.  A NAS selection
preference went out at 219.00 s and `initiate register` was tried
from 220.05 s onward (error 37 each).  The watch's Set ONLINE
attempts (408.37, 439.29, 469.81, 500.52, 531.56, 562.62, 593.66 s)
all answered 52, every LOW POWER answered 60, every get-mode read 3
(`OFFLINE`, reason 0xffffffff).  No modem restart: the 45 services
were announced once, at 184.8 s, and the modem's only type-4 frame
all run was the DMS operating-mode event report at 187.38 s.

TFTP tells the other half: the modem read only `mbn_sw.dig` and
`mbn_hw.dig` (132 times each), found its own `/readwrite/mcfg.tmp`
NOT HERE 131 times, wrote a fresh 1-byte `mcfg.tmp` and a 5-byte
`server_check.txt`, and never asked for `mbn_sw.txt`,
`oem_sw_j11.txt` or any `generic/` config.  Whether the branch at
the dig ACK starved the pump, or the modem had nothing to select,
the carrier table never loaded -- and run 123149, whose scanner read
all 60 configs, still ended with 192 Set ONLINEs answering 52, so
the scan is not what turns the mode change from 52 to 0 either.

What this run settles: DMS readiness is not downstream of the card.
The modem refuses the mode change with a perfect, provisioned
`READY` card in the slot, just as it did with the card absent, with
the scan incomplete, and with the EFS synced.  The gate is on the
host half of the boot handshake, not the SIM.  Next: stop permuting
the card timing, and use the root shell the stock system allows to
capture what the stock RIL does around a successful DMS ONLINE.

## Run 044127: the hello answered inside the round, DMS met in SHUTTING_DOWN

The stock capture settled the reference timing first (radio_boot2.log,
one boot, Sahara complete at 15.9 s): qcril's first DMS answer comes
2.2 s later and reads mode 5, SHUTTING_DOWN; the LOW POWER request
goes out 4.4 s after that answer and is accepted; ONLINE follows and
is accepted 9.3 s after Sahara.  The transitions this firmware
accepts exist only while its boot is still settling, and stock's
whole sequence lives inside that settling window.

Run 044127 (kernel 8c257d5f42b4d3ce) carried the first three
host-side changes and moved this host into the same window:

- `mhi_qrtr_hello_heard` is set the moment a hello names the modem's
  node, and the host's announcement goes out from inside the drain,
  where stock's kernel answers the hello -- not after the round went
  quiet.  The log says so: `after the hello: 3 of the host's 3
  services announced to node 2`.
- The first DMS contact is made the frame its announcement is
  parsed (`DMS: first contact in hello round 3, before the card`),
  and `mhi_efs_service` runs in every drain iteration.

The first answer then read the state stock reads:

| event | stock | 030228 | 043045 | 044127 |
|-------|-------|--------|--------|--------|
| first DMS answer | 2.2 s | +55 s | +33 s | +16 s, round 3 |
| mode read | 5 (shutting down) | 3 | 3 | **5** |
| LOW POWER | 4.4 s, accepted | 60 | 60 | 60 |
| ONLINE | 9.3 s, accepted | 52 | 52 | 52 |

The contact timing is no longer the wall; two host-side gaps are:

1. **The drain dropped the modem's requests.**  Round 0 carried 51
   type-1 frames to the host's services -- the registry's `tms/pddump_disabled`
   and `avs/audio` lookups at 1:0x3002, TFTP's
   `/readwrite/server_check.txt` at 1:0x3004 -- and the drain merely
   logged each as `IPCR other`.  The round's cap was 48 frames, so
   the queue was not even emptied.  The modem does not publish DMS
   until its boot's needs are served: DMS appeared only at 15 s,
   after the post-contact pump finally answered the queue.
2. **The warm-up ate the SHUTTING_DOWN window.**  The first get read
   5, then `mhi_dms_warmup` spent its round trips on the client
   setup, its own get already read 3, and the LOW POWER that
   followed was refused 60.  Stock's setup happens before its
   radio-off, so the state it lowers from is still there.

The fixes: `mhi_host_request()` serves the registry's and TFTP's
requests from inside the hello rounds, exactly as the pump serves
them (the round cap is now 240, so a round takes the whole queue),
and `mhi_dms_early` sends LOW POWER the moment its first get reads
5, before the warm-up -- then the warm-up, then ONLINE, with the
old pair left as the fallback.  The DMS port is not hardcoded: it
was 0x4b in 043045 and 0x4a in 044127, by the boot.

## Run 045248: the mode change accepted, the card bound, NAS registers

Kernel 8d4ddd107735, the two fixes above.  The queue is served now:
round 0's log carries `the host's service registry asked; answering`
and a run of `the host's TFTP (v3) asked; answering` lines, then
`DMS: first contact in hello round 0, before the card` -- the modem
published DMS in the first round instead of the third, 044127's
+16 s became the first seconds of a boot that no longer waits on
its unanswered requests.

The first contact's get went unanswered (`no answer`), the warm-up's
get then read 5 (SHUTTING_DOWN), and `mhi_dms_online` was asked
from there.  Both answers are `result 0, error 0`:

| request | txn | answer |
|---------|-----|--------|
| LOW POWER (0x2e, TLV 1=1) | 0x31 | result 0, error 0 -- ACCEPTED |
| ONLINE (0x2e, TLV 1=0) | 0x41 | result 0, error 0 -- ACCEPTED |

`before the card the modem is ONLINE`, and after the card work the
get-mode still read 0 (ONLINE).  The host-side handshake that all
of runs 030228 through 044127 failed is now stock's own sequence;
the new shutdown-window branch in `mhi_dms_early` was not the one
that did it (the contact came earlier than the first get could
answer, and the warm-up's get found the window still open), but
the serving in the rounds is what opened the window at contact time.

The card followed: `the card is already ready in logical slot 1`,
`THE USIM OF LOGICAL SLOT 1 IS MADE THE PRIMARY GW APPLICATION
(16-byte AID)`, `THE PROVISIONING TOOK: primary GW application 0
(slot 0)`.  NAS then answered the selection preference (0x34), the
system info (0x4d) and `NAS initiate register (online)` (0x22) all
`result 0, error 0`.

Two things the run showed open:

1. **TFTP still did not scan the carrier table.**  The modem wrote
   `/readwrite/server_check.txt` (5 bytes, kept), read
   `/readwrite/mcfg.tmp` NOT HERE, wrote a fresh one -- the dig
   pair, not `mbn_sw.txt`.  What makes the firmware skip the
   config list is the next question for registration.
2. **The system's cellular UI reads sysctls this driver does not
   publish.**  With iOS up, SpringBoard's injected cellular hook
   logged `the kernel says up -1 registered -1 bars -1 dbm -1`:
   `userland/src/inject_sb.c` reads `kern.hakios.cellular_up`,
   `_registered`, `_bars`, `_dbm`, `_operator` (the legacy
   driver's names, drivers/modem/hakios.mpss.c), and this kernel
   has no such nodes.  It now does: kernel 294ba298de73ac0d makes
   `hakios.nas.c` a standard file, feeds it the NAS answers
   (GET_SERVING_SYSTEM 0x24 and GET_SIGNAL_INFO 0x4f every two
   seconds from a keep-alive thread that pumps between polls),
   and publishes the parser's state through the five sysctls.  The
   thread also keeps serving the modem's TFTP/registry/EFS traffic
   after the probe returns, which the old probe left unattended.

## Run 051024: REGISTERED WITH A NETWORK, and the zero-valued NR TLV

Kernel 294ba298de73, the bridge's first boot.  The keep-alive thread
started (`pcie2: cellular: the NAS state is polled every two seconds;
kern.hakios.cellular_* are live`) and its first state was the one
every earlier run never got to:

	pcie2: cellular: REGISTERED WITH A NETWORK, 0 bars, 0 dBm
	    (radio 0), operator "" (MCC 0 MNC 0)

**The modem is registered on the network.**  The GET_SERVING_SYSTEM
answers repeat it every poll (QMI length 83): TLV 1 is
`01 06 00 01 02 01 02 01 08` -- registration 1, CS 2, PS 1, network
2 (LTE), one radio, radio 8 (LTE).  No name-bearing PLMN TLV comes
in this answer (the 0x10 here has length 1), so the operator string
stays empty; the SIM card itself is READY and primary (the UIM
block again read `THE PROVISIONING TOOK: primary GW application 0
(slot 0)`).

The reading was there and the parser dropped it.  Each
GET_SIGNAL_INFO answer (QMI length 28) carries two TLVs:

	14 06 00 bd f1 9a ff 00 00     LTE: RSSI -67, RSRQ -15, RSRP -102 dBm, SNR 0
	17 04 00 00 00 00 00           NR:  all zeros

The old chain matched 0x17 first (`size >= 4`), took level 0 for an
NR reading and set `radio = 12`; the LTE branch's `radio != 12`
guard then skipped the real -102 dBm, and the final range check
threw the zero away -- `has_signal` stayed false and the state never
changed from "registered, no reading".  The parser now takes each
TLV as a candidate, keeps only values inside -150..-30 dBm, and
ranks NR > LTE > UMTS > GSM, so the zero-filled 5G placeholder no
longer shadows the LTE reading.

The UI side of the bridge did work on the same boot: the injected
hook read `up 1 registered 1 bars 0 dbm 0` and drew its floor of
one bar (`1 bars shown (item 4; kernel bars 0, 0 dBm, registered 1)`),
so once the parser hands it -102 dBm it derives three.

Two observations of the boot itself: the modem read `mbn_sw.dig`
inside the hello rounds (`1 blocks gone`, seconds into the boot) --
the serving in the rounds lets the config dig finish before the
card work -- but no carrier-config scan followed, consistent with
the table above (the dig alone does not start it; the switched-then-
ABSENT status does, and this boot's card was already primary GW,
so no switch was sent).  And the modem's nameservice sent
DEL_LOOKUP (control cmd 6) for service 0x3 instances 94-96 with
node 0 port 0: some modem-side client looked for a NAS service on
the *host's* nameservice, which the host does not provide, and
cancelled.  The modem's own NAS answers fine; noted, not blocking.

## Run 051934: the reading lands -- three bars, -103 dBm, status bar too

Kernel c3bd176211c5, the signal-TLV fix.  battguard stopped the
boot at 23% (`at or below battery_stop=25 three looks in a row`)
after 254 s, but the bridge had already published:

	pcie2: cellular: REGISTERED WITH A NETWORK, 0 bars, 0 dBm (radio 0), operator "" (MCC 0 MNC 0)
	pcie2: cellular: REGISTERED WITH A NETWORK, 3 bars, -103 dBm (radio 8), operator "" (MCC 0 MNC 0)

The first is the serving-system half of the poll cycle, the second
the signal half the next message carries (RSRP -103 this boot,
-102 in 051024).  SpringBoard's hook read the sysctls and drew
them:

	hakios: cellular: the kernel says up 1 registered 1 bars 3 dbm -103
	hakios: cellular: 3 bars shown (item 4; kernel bars 3, -103 dBm, registered 1)

SIM -> registration -> signal reading -> status bar is closed end
to end on the device.  The operator name is still empty: the
serving-system answer carries no name-bearing PLMN TLV, so the
name has to come from the card (EF_SPN) or a later NAS
indication.  Next: the card's own number (EF_MSISDN) and SMSC,
then WMS -- send to self and receive.

## The SMS milestone: the card's own number, WMS, a message to self

Kernel e50c63e933123bf1 adds the next milestone to the cellular
thread (`mhi_sms_milestone`, one step per cycle after the NAS
poll).  After registration the thread:

1. Reads EF_MSISDN with UIM Read Record (0x0021) the way libqmi
   asks it for qmicli --uim-read-record: session TLV 1 {primary
   GW with no AID}, file TLV 2 {file id 0x6f40, path 7fff
   (ADF_USIM) -- and, failing that, path 7f20 (DF_GSM)}, record
   TLV 3 {record 1, length 28}.  A card answer of 6c XX (wrong
   length) names the record's real length in SW2 and the read is
   sent again with it.  The record is parsed from its tail -- the
   number's length byte lands exactly on the record's end -- and
   the TON/NPI's international bit adds the '+'.  (The first
   attempt carried the card's own AID in the session TLV and the
   stack answered result 1, error 3 with no card result at all;
   see run 054035.)
2. Turns WMS Set Event Report (0x0001) on: TLV 0x10, the new-MT
   message report, a single byte 1.  From then on the modem sends
   an Event Report indication for every message delivered.
3. Builds a GSM 03.40 SMS-SUBMIT -- SMSC field empty (the modem's
   own), first octet 0x01, DA type 0x91, PID 0, DCS 0x08 (UCS2),
   the text "HakiOS SMS test" -- and submits it with WMS Raw Send
   (0x0020), TLV 1 {format 6 (GSM/WCDMA point-to-point), u16
   length, PDU}.  The answer's TLV 1 is the modem's message id,
   TLV 0x12 would carry the RP/TP causes of a refused send.
4. Watches for the delivery back: a WMS Event Report indication
   (type 4, message 0x0001) carries TLV 0x10 {storage type u8,
   memory index u32} and often TLV 0x11 {ack, txn, format, u16
   length, raw PDU} too.  mhi_sms_note takes down where the
   message is stored -- the indications are seen both by the pump
   between polls and by mhi_qrtr_answer while another request
   waits -- and an inline PDU is decoded at once; a message that
   only names a location is read on the next cycle with WMS Raw
   Read (0x0022), TLV 1 {storage, index}, TLV 0x10 mode 1, and
   decoded from the answer's TLV 1 {tag, format, u16 length, PDU}.
5. Decodes SMS-DELIVER: the SMSC field, the sender (BCD with the
   international '+', or GSM 7-bit alphanumeric), the DCS and the
   text -- UCS2 when (DCS & 0x0c) == 0x08, GSM 7-bit through the
   default alphabet otherwise; a status report or other
   non-deliver PDU is named in the log, not misread.

The evidence the log must carry is `THE CARD'S OWN NUMBER IS
+86...`, `the SMS-SUBMIT ... WAS TAKEN (message N)`, then `THE
MODEM REPORTS A NEW MESSAGE ...` and `AN MT MESSAGE FROM
+86...: "HakiOS SMS test"`.  The number and the last message are
published as kern.hakios.cellular_own_number and
kern.hakios.cellular_sms for the status bar to draw later.

## Run 054035: WMS answers, the SMSC arrives, EF_MSISDN refused the AID

Kernel e50c63e93312, the SMS milestone's first boot.
Registration and the signal reading came as before (3 bars,
-102 dBm, radio 8), and the milestone ran on the cellular
thread's next cycle:

	pcie2: cellular: SMS: THE MILESTONE STARTS: the card's own number, the new-message report, a message to self
	pcie2: cellular: SMS: read EF_MSISDN: result 1, error 3, card 00 00, 0-byte record
	pcie2: cellular: SMS: the modem's new-message indication is on
	pcie2: cellular: SMS: no own number was read; the send-to-self test is skipped

WMS Set Event Report was answered result 0, error 0, and the
modem at once sent its SMSC Address indication (QMI 0x0046,
TLV 1: type "145", digits +8619202900500) -- WMS is live and its
indications reach this host.

The EF_MSISDN read did not: result 1, error 3, and no card
result TLV at all, so the request never reached the card.  It had
carried the session TLV {primary GW, the card's 16-byte AID},
the identity the provisioning used.  libqmi's read-record builder
(qmicli 1.38 sources, read from Debian) passes QMI_UIM_SESSION_
TYPE_PRIMARY_GW_PROVISIONING with an *empty* AID and marks the
AID argument ignored; the fix sends session TLV 1 {0, no AID}
and, when the ADF_USIM file is not there, tries EF_MSISDN under
DF_GSM (path 7f20) too.  Kernel 763ef5f275c7937b carries it.
battguard ended the run at 25% after the evidence had landed.

## Run 060227: the AID gone, and the same refusal

Kernel 763ef5f275c7937b, the session TLV without an AID (libqmi's
shape).  The read went out for both paths and the stack answered the
same way as with the AID:

	pcie2: cellular: SMS: read EF_MSISDN under ADF_USIM: result 1, error 3, card 00 00, 0-byte record
	pcie2: cellular: SMS: read EF_MSISDN under DF_GSM: result 1, error 3, card 00 00, 0-byte record
	pcie2: cellular: SMS: no own number was read; the send-to-self test is skipped

So the AID was not what the stack refused.  The next suspect was the
path bytes themselves: qmicli's file spec writes each 16-bit id low
byte first ("7fff" -> ff 7f), and the driver had sent 7f ff.

## Run 062146: little-endian paths, a pathless read that answers, DMS NOT_PROVISIONED

Kernel 23ad2fed1c9c9096 rewritten to qmicli's byte order and widened:
paths 7fff, 7f10 (DF_TELECOM), 7fff/7f20, and no path at all; sessions
0, 3 and 6 on the first path; Get File Attributes (0x0024) for the
record length.  Every path-based request -- read or attributes, any
session -- came back result 1, error 3, with no card result TLV:

	pcie2: cellular: SMS: file 6F40 under under ADF_USIM 7fff: result 1, error 3, card 00 00, file 0 bytes (id 0000 type 0), record 0 bytes x 0
	pcie2: cellular: SMS: read EF_MSISDN under ADF_USIM 7fff, session 0: result 1, error 3, card 00 00, 0-byte record
	(sessions 3 and 6, DF_TELECOM 7f10, 7fff 7f20 -- all the same)

The request with *no* path, though, reached the card:

	pcie2: cellular: SMS: read EF_MSISDN with no path, session 0: result 0, error 0, card 90 00, 28-byte record
	pcie2: cellular: SMS: EF_MSISDN holds no readable number; 28 bytes: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff

Card-level success (90 00) with a 28-byte record that is entirely
0xff: either the card's EF_MSISDN is blank, or the pathless request
selected another file and read its record 1.  Kernel 28c0eb1d14385952
asks Get File Attributes with no path as well -- its answer names the
file and its record size -- and reads records 2..4 while it is at it.

The modem's own DMS was asked the same question (Get MSISDN 0x0024,
no input TLVs), and refused:

	pcie2: cellular: SMS: DMS get MSISDN: result 1, error 16, 0 characters

Error 16 is QMI_PROTOCOL_ERROR_NOT_PROVISIONED (libqmi 1.38.0-1,
src/libqmi-glib/qmi-errors.h, read from Debian sources): the modem
does not consider the MSISDN provisioned on this SIM either.

## Where the self-send target comes from now

Neither the card nor the modem holds this line's number, so the
self-send target is taken from the one authority that has it: Android
on the same phone, same SIM, answered +86192991300141 through
`service call iphonesubinfo 16/17` (2026-11-21; codes 12/13 throw
SecurityException, 15 returns null, and `content query
content://telephony/siminfo` is refused to non-system UIDs).  The
kernel takes it from an out-of-tree header -- `bsd/dev/hakios_sms_target.h`
in the kernel tree, beside the synced driver, defining
HAKIOS_SMS_TARGET -- which `mhi_sms_milestone` falls back to only when
both the card read and DMS stayed silent.  The header is never a
repository file, so the number stays out of the source, and a tree
without it builds exactly as before.  Kernel 28c0eb1d14385952 (2026-11-22
06:31) carries the record 2..4 read, the pathless attributes call and
the fallback; its run's log should carry `THE CARD'S OWN NUMBER IS
+86192991300141 (the build's out-of-tree target)` when the card stays
blank, then the submit and, back from the network, the delivered
message.

## Run 064223: the card's file named, both records empty, the run cut at the submit

Kernel 28c0eb1d14385952, its first boot.  The pathless Get File
Attributes finally named what the pathless read selects:

	pcie2: cellular: SMS: file 6F40 under with no path: result 0, error 0, card 90 00, file 56 bytes (id 6f40 type 2), record 28 bytes x 2
	pcie2: cellular: SMS: read EF_MSISDN with no path, session 0, record 1: result 0, error 0, card 90 00, 28-byte record
	pcie2: cellular: SMS: read EF_MSISDN with no path, session 0, record 2: result 0, error 0, card 90 00, 28-byte record
	pcie2: cellular: SMS: read EF_MSISDN with no path, session 0, record 3: result 1, error 80, card 6a 83, 0-byte record
	pcie2: cellular: SMS: read EF_MSISDN with no path, session 0, record 4: result 1, error 80, card 6a 83, 0-byte record

EF_MSISDN (6F40, linear fixed, 28 bytes, two records) is really
there, both records are blank (0xff), and 6a 83 is the card's
"record not found" for the rest.  The pathless read had been reading
the right file all along; the path-based requests are simply refused
by this AMSS (result 1, error 3).

The fallback came through -- and the run ended at the submit:

	pcie2: cellular: SMS: THE CARD'S OWN NUMBER IS +86192991300141 (the build's out-of-tree target)
	pcie2: cellular: SMS: the modem's new-message indication is on
	--- battguard: the charge is 25%, ...: back to the loader ---

Android had said 31% at the start, the kernel's own gauge 28%, and
the modem load brought it to 25: the guard cut the run one step
short of the submit.  The log also showed the DMS Event Report --
message 0x0001 like WMS's -- being taken for a new message, its
TLV 0x10 (power state) read as a message location.  Kernel
c5304d7d4dc389cc fixes both: mhi_qrtr_from() lets the new-message
handler hear only the WMS port, the Event Report's TLV 0x11 is
parsed by its real layout (ack u8, transaction u32, format u8,
u16 length, PDU; the old offsets were four bytes early), and the
submit logs a line before the call, so a cut and a refusal read
differently.  The next boot starts at 38% (Android's gauge).

## Run 072221: the submit answered, temporarily failed, the answer mislaid

Kernel c5304d7d4dc389cc, Android's gauge 38% at the start.  The
whole milestone ran -- the pathless file, both blank records, the
DMS refusal, the out-of-tree number, the event report -- and this
time the submit left:

	pcie2: cellular: SMS: THE CARD'S OWN NUMBER IS +86192991300141 (the build's out-of-tree target)
	pcie2: cellular: SMS: the modem's new-message indication is on
	pcie2: cellular: SMS: the SMS-SUBMIT to +86192991300141 goes out (45 bytes of PDU)
	pcie2: AMSS: IPCR WMS raw send (...): QMI type 0 transaction 179 message 0x20 length 51
	pcie2: AMSS: IPCR WMS raw send (...): the modem's own indication 0x46 (28 bytes) while it waited
	--- battguard: the charge is 25%, ...: back to the loader ---

The raw send carried a well-formed SMS-SUBMIT: no SMSC (the modem's
own), FO 0x01, +86192991300141 as TON 0x91, PID 0, DCS 0x08, 15
UCS2 characters, 45 bytes.  The 0x46 indication beside it is the
WMS SMSC Address report, and its Address TLV is {string type (3
chars) "145", length-prefixed string digits "+8619202900500"} --
the modem knows which SMSC to use.  But "no answer" followed, and
for a while it looked like the modem had ignored the request.

It had not.  Grepping every later snapshot for the frame found
the answer 9 s out, taken by the next NAS poll because the waiting
caller only checked the destination port, not the transaction:

	pcie2: AMSS: IPCR NAS serving system (cellular state): 40 bytes: 03 01 00 00 17 00 00 00 03 00 3a 00 01 00 01 10 02 b3 00 20 00 10 00 02 04 00 01 00 38 00 01 02 00 0b 00 13 01 00 00 00
	pcie2: AMSS: IPCR NAS serving system (cellular state): QMI type 2 transaction 179 message 0x20 length 16
	pcie2: AMSS: IPCR NAS serving system (cellular state): message 0x20, result 1, error 56

Decoded, that is the raw send's answer: result 1, error 56 (WMS
message delivery failure), message id TLV 0x01 = 11, and TLV 0x13
= 0 -- the failure type, temporary.  The modem accepted the
submission and the delivery failed transiently; a retry is the
remedy, and a permanent failure (0x13 = 1) is what it would report
when it is not.  The plain call waits only 7 s of silence, and the
answer came at about 9 s.

Kernel 307cfd6b3fe2dca3 fixes the three things this taught:

* mhi_qrtr_answer() now matches the answer's transaction and
  message id, not just the port; a late answer is logged as
  "IPCR a late answer elsewhere" and left for the call it belongs
  to (no other call can take it for its own, as the NAS poll did).
* mhi_qmi_call_slow() gives the raw send 40 s of silence budget,
  enough for the modem's network round trip.
* mhi_sms_send() retries a temporary failure up to three times,
  8 s apart, each attempt with a transaction of its own
  (0xb3, 0xb4, 0xb5) so attempts cannot be confused; a permanent
  failure is not repeated.  The per-attempt line names the causes
  and the failure type, and the final line says what became of
  the submission.

The next boot wants Android's gauge at 38% or more: the retries can
add up to 2.5 minutes to a phase that starts about 90 s into the
kernel run, and the guard ends the run at 25%.
## Run 081226: three temporary failures, and Android shows who carries the SMS

Kernel 307cfd6b3fe2dca3, Android's gauge 37% at the start.  The
milestone ran as in run 072221, and this time the transaction match
kept every attempt honest: each of the three submits was answered
late (the modem took 40 s or more), the late frames logged as
"IPCR a late answer elsewhere", and each says the same thing:

	02 b3 00 20 00 10 00 02 04 00 01 00 38 00 01 02 00 0c 00 13 01 00 00 00

result 1, error 56 (WMS message delivery failure), message id 12,
and TLV 0x13 = 0: the failure type is temporary, not permanent.
The final line says what became of it, as it should:

	pcie2: cellular: SMS: the SMS-SUBMIT to +86192991300141 FAILED after 3 attempts: result 4294967295, error 4294967295, RP cause 0, TP cause 0

(4294967295 -- ~0u -- is the no-answer sentinel of the call that
gave up before the modem's 40 s answer came; the frame above is the
real answer, and the next kernel waits 40 s of silence for it.)

While the phone recharged, Android itself answered the question the
logs could not: which transport this network gives SMS.  With the
same SIM in Android, `dumpsys telephony.registry` says the serving
system is NR SA on 460/15 (中国广电), and its CS domain registration
advertises availableServices=[VOICE,SMS,VIDEO] -- IMS services, not
SGs.  The vendor IMS stack (org.codeaurora.ims) logs

	QImsService: ImsSenderRxr : [0024]< REQUEST_IMS_REGISTRATION_STATE ImsRegistrationInfo state: 1 errorCode: 0 ...
	QImsService: ImsServiceSub : [1] VOLTE ims registered, WFC change to Not registered

state 1 = registered.  The carrier configuration's default enables
`imssms.sms_over_ims_supported_bool = true`, and the APN database has
460/15's two rows: default `cbnet` and IMS `apn=ims, protocol=IPV4V6`.
So there is no CS domain for the SMS to ride: Android's SMS on this
SIM is SMS over IMS.

HakiOS had none of that transport.  The modem ran its own IMS stack
all along, but the host never brought the "ims" PDN up and never
turned the IMS settings on, so WMS had nothing to submit over and
answered "temporary" until it gave up.  Kernel f3f2b0abc2f50a55
gives the milestone the host's part of what qcril does -- all over
QMI, no SIP in the host:

* WDS (service 0x01, its own QRTR port 0x1002): Bind Subscription
  0x00af to the primary subscription, then Start Network 0x0020 with
  TLV 0x14 the APN "ims", 0x16 no authentication, 0x19 IPv4; then
  Get Packet Service Status 0x0022 until TLV 0x01 says connected (1),
  30 s at most.  Refusals keep the call end reason (TLV 0x10).
* IMS (service 0x11, port 0x1003): Bind 0x0098, then Set IMS
  Services Enabled Setting 0x008f with TLV 0x10 Voice over LTE, 0x18
  IMS service and 0x1a IMS SMS all on; Get IMS Services Enabled
  Setting 0x0090 reads the SMS flag back.
* IMSA (service 0x21, port 0x1004): Bind 0x0033, then Get IMS
  Registration Status 0x0020 (TLV 0x12 the state, 0x14 the
  technology) and Get IMS Services Status 0x0021 (TLV 0x10 SMS, 0x11
  voice, 0x13 the SMS technology) every 15 s until the registration
  is registered (2) and SMS available (2), 90 s at most.

The milestone first gives the transport 30 s to come by itself and
asks WMS Get Transport Network Registration Status (0x4a) for it;
when it has not, the three steps above run, the transport is asked
again, and the submit goes out with its three attempts either way.
The QRTR ports 0x1002/0x1003/0x1004 keep the three new clients
apart from WMS's 0x1001, the way Android's qrtr sockets do.

The next boot wants Android's gauge at 38% or more: the IMS steps can
add four to five minutes to a phase that starts about 90 s into the
kernel run, the submit's retries up to two minutes more, and the
battery guard ends the run at 25%.  What to read, in order: the
transport line before the bring-up, "THE IMS PDN WAS STARTED", the
packet service line, "THE IMS, VOLTE AND IMS SMS SETTINGS SAY", the
IMSA registration line, the transport line after the bring-up (4 =
full service), and then the submit's own line.

Run 091313 (kernel c1752bf0f8a0) answered a different question than it
was sent to ask.  Its NAS registered as before, but the SMS milestone
never started: the run logged

	pcie2: cellular: SMS: the modem has not announced WMS yet

6718 times and no IMS line at all.  Run 081226 had WMS at 3:0x3a by
the time the milestone first looked; 091313's name table held 210
"IPCR service" announcements against 081226's 324, and WMS was not
among them.  The modem's name service announces its table in answer
to a QRTR hello, and the host's hello rounds all happen in the boot
probe; a service whose registration lands after those rounds was
never learned, because the cellular pump only handled type-1 data
frames and dropped the type-4 NEW_SERVER announcements.  The SMS
milestone also returned at its WMS gate before it could reach the IMS
block, so the run carried no IMS evidence either.

Two fixes, landed in that order:

* Kernel 8254f635172eb37b: the cellular pump parses type-4
  announcements and remembers the service through the same path the
  boot probe uses, so a registration that lands at any time is
  learned.  While WMS is missing the milestone also re-sends the
  boot's hello frame every ten cycles (about 20 s), which makes the
  modem's name service announce its whole table again.
* Kernel 0392cee67f1c0173: the IMS envelope no longer lives behind
  the WMS gate.  When WMS is still missing a few cycles after
  registration, a standalone stage looks at IMSA once; if the
  registration is not up it starts the WDS "ims" PDN, turns the IMS
  settings on and watches IMSA for 90 s, then logs where the transport
  stands.  It runs once per boot, and the SMS stage, if WMS shows up
  later, reuses what it saw instead of starting the PDN twice.

The next run's lines, in order: "WMS is still not announced; the
modem's name service was asked for its table again" or the late
"IPCR service" lines naming WMS; then, if WMS stays away, "WMS is
late; the IMS path is brought up without it", "THE IMS PDN WAS
STARTED", "the packet service says", "THE IMS, VOLTE AND IMS SMS
SETTINGS SAY", the IMSA registration line and "the WMS-less bring-up
ends with the registration …".  When WMS is there, the earlier order
applies unchanged, ending with "the transport before the submit …".

A later look at 091313's raw announcement frames sharpened the
diagnosis: the modem did announce service 0x05 (WMS), twice, along
with 0x01 (WDS), 0x0b (UIM), 0x11 (IMS) and 0x21 (IMSA).  The table
dump arrived while the cellular pump was draining type-1 frames, so
it was logged and dropped -- not a late registration but an
unlearned announcement.  The pump fix is what turns those same frames
into remembered services, and the IMS services the decoupled stage
needs were all in that dump.

## Run 113605: the card is recognized; the PDN is refused with InvalidOperation (2026-11-22)

Kernel 01972da982fd9dc2, Android's gauge 82% at the start.  This run
is the first whose whole chain is readable line by line:

* The card is fully recognized: EF_MSISDN returns the line's own
  number +86192991300141, the card answers 90 00 to the long-form
  reads, and the NAS line says "up 1 registered 1 bars 3 dbm -102".
* NAS serving system TLV 0x01: registration 1 (registered), CS
  attach 2 (detached), PS attach 1 (attached), selected network 2,
  radio 8 (LTE).  PS is attached; this network has no CS domain.
* WDS Bind Subscription 0x00af answers result 0.
* WDS Start Network 0x0020 was refused in all five forms the kernel
  tried -- by the created profile with family 8, by it with no
  family, and by the APN with families 8, 6 and 4.  Every answer is
  result 1, error 70 (InvalidOperation), with no call end reason TLV
  (that TLV is only sent with error 14, Call Failed).  Create Profile
  0x0027 answered with profile "7"; a later profile list never showed
  it.
* IMS (0x11): Bind 0x0098 and Set/Get 0x008f/0x0090 all answer error
  57 (WMS_INVALID_MESSAGE_ID, the shared code space): this firmware
  does not implement those IMS QMI messages.
* IMSA (0x21): Bind 0x0033 answers 0; Get Registration Status 0x0020
  and Get Services Status 0x0021 answer error 70.
* WMS: Set Event Report 0x0001 answers 0; Get Transport Network
  Registration Status 0x004a answers error 52 (DeviceNotReady); Raw
  Send 0x0020 was never answered (40 s of silence) -- what a submit
  does with no transport under it.

Reading the profile list byte for byte found two of the kernel's own
bugs.  First: Get Profile List 0x002a's output TLV 0x01 is a u8
element count followed by that many {type u8, index u8, name
lstring} (libqmi's qmi-service-wds.json and wwand's wire schema
agree).  The kernel parsed the count as the first profile, so every
entry was labelled one field off and profile 1 -- the profile the
modem's own attach uses -- was never surveyed.  The list actually
holds eight profiles: 1 (no name), 2, 3, 38 "cv2x", 39 "cv2x", 4, 5,
6 "qdp_profile_ia".  Second: the packet service status was read
against the wrong table.  QmiWdsConnectionStatus (qmi-enums-wds.h,
libqmi 1.38) is 0 unknown, 1 disconnected, 2 connected, 3 suspended,
4 authenticating; the kernel treated 1 as connected, so a wait for
"connected" stopped on "disconnected".

Kernel 0d9d8e745571a4c7 fixes both and adds the evidence and the
dial shapes wwand's hardware-verified bring-up uses (ddimension/wwand,
Quectel RG520N over PCIe MHI/QRTR):

* The profile survey reads the count, walks exactly that many
  entries, logs every profile settings answer (result, error, the APN
  it carries or the TLV's absence) and now includes profile 1.
* Create Profile's answer is verified with Get Profile Settings
  0x002b before it is dialled; a create that does not persist is
  named as such.
* Every start sends Set IP Family 0x004d first, as the reference
  does; Start Network carries TLV 0x19 only when 0x004d itself was
  refused, always carries TLV 0x35 Call Type = 1 (embedded, the way
  qcril dials a phone's IMS PDN), and waits with the 40 s answer
  budget instead of the plain call's 5 s.
* New evidence before the first start: WDS Get Supported Messages
  0x001e (u16-counted id list, read for 0x0020/0x0022/0x0027/0x002a/
  0x002b/0x0049/0x004d/0x0085), Get Packet Service Status 0x0022,
  Get Default Profile Number 0x0049 ({3GPP, embedded}) and Get LTE
  Attach Parameters 0x0085 (the APN the network settled on, the IP
  type, whether an attach ran).
* Two more dial shapes: profile + inline APN together (the reference
  sends both when a profile-modify is configured), and a start by the
  modem's own default profile -- the one its attach uses even when no
  listed profile carries an APN.
* After a refused start the packet service is read again: a session
  that is connected anyway (autoconnect, or a start answered "no
  effect") is adopted instead of being reported as a failure.
* A started call's own settings are read back (Get Current Settings
  0x002d: the APN, the IP family, whether an IPv4 address is there),
  and the packet service is polled for 60 s against the enum's real
  values.

What to read, in order: "the profile list names 8 profiles" and the
eight profile lines with their APNs; the created profile's "reads
back with the APN"; "WDS lists N supported messages" (whether 0x0020
is in the firmware's own set); "before the start the packet service
says"; "the modem's default 3GPP profile is N"; "the LTE attach is
attached ..."; each "set-ip-family ..." and start line; then either
"THE IMS PDN WAS STARTED" with "the packet service says 2
(connected)" and "the started call carries the APN", or the refusal
line with its after-the-fact packet status.  The IMS 0x11 answers
(57) and IMSA 0x21 answers (70) are only meaningful once a PDN is
up: with no PDN under it, IMSA's 70 is not yet evidence that the
message is unimplemented.

## Run 121242: every Start Network refused 70; 0x11 is SAR, not IMS

Kernel 0d9d8e745571a4c7 carried the milestone through to the end.
The card is fully recognised (logical slot 1 maps to physical slot
2, USIM ready with its AID), NAS registered (3 bars, -102 dBm, LTE
radio 8), DMS ONLINE, the profile list parsed correctly (8 profiles:
1-5, 38, 39 "cv2x", 6 "qdp_profile_ia"), the default 3GPP profile is
6, and the LTE attach itself is done with APN "ims", attached, ip
type 1.  Set IP Family 4 and 6 are accepted and 8 is refused with
error 48 -- 8 is UNSPECIFIED in QmiWdsIpFamily, not IPv4v6 (that is
the profile's PDP type); run 121242 had read 8 as "IPv4v6".

All seven Start Network shapes were then refused the same way:
result 1, error 70 (InvalidOperation), call end reason 0xffffffff,
and the packet service stayed 1 (disconnected) before and after.
This is byte for byte what the reference phone's session answered
while its data port had never been opened (the DPM-skip cycles: WDA
InvalidOperation, mux bind Internal, Start InvalidOperation).  The
thyme driver had no DPM/WDA step at all, and the modem does announce
DPM (0x2f) and WDA (0x1a) at node 3.

The IMS settings had gone to service 0x11, where every message was
refused error 57 (invalid message id).  libqmi's
qmi-service-ims.json puts Bind 0x0098, Set/Get IMS Services Enabled
Setting 0x008f/0x0090 and Policy Manager 0x0047-0x0049 on service
0x12 -- and the modem announced 0x12 instance 1 at node 3 port 0x49.
0x11 is SAR.  (This supersedes the paragraph above: the 57s were the
wrong service, not a missing PDN.)

Every "profile settings went unanswered" was the milestone's
512-byte answer buffer: the real answers are 696-752 bytes, and
mhi_qrtr_answer refuses to truncate (it logs "a N-byte answer, not
the M there was room for" and returns nothing).  Get Supported
Messages 0x001e is refused by this firmware (result 1, error 71);
that is the firmware's answer, not a driver bug.

## Kernel d9163aae6017db4e: the data port, the right IMS service

* mhi_qrtr_name: 0x11 SAR, 0x12 IMS, 0x1a WDA, 0x2f DPM; the
  remember list takes 0x12/0x1a/0x2f (array 8 -> 16) and drops 0x11.
* mhi_ims_enable sends Bind/Set/Get to 0x12.
* New mhi_wds_data_port: DPM Open Port 0x0020 with the hardware
  data ports TLV 0x11 (u8 count 1, then {type, interface, rx, tx} as
  four u32s) and the plausible pairs probed in order -- (EMBEDDED 4,
  iface 1, endpoints 2/16: the reference phone's), (PCIE 3, 1, 2/16),
  (4, 1, 22/23), (3, 1, 22/23), (3, 0, 2/16), (4, 0, 0/0) -- first
  result 0 wins and is logged "THE DATA PORT IS OPEN".
* New mhi_wds_data_path, on that port: WDA Set Data Format 0x0020
  (TLV 0x11 link 2 raw IP, 0x12/0x13 aggregation 5 QMAP, 0x15 16
  datagrams, 0x16 0x3800 bytes, 0x17 {type, iface}), then WDS Bind
  Mux Data Port 0x00a2 (TLV 0x10 {type, iface}, 0x11 mux) on mux 1
  and then mux 0 -- the reference phone's exact shapes.
* mhi_wds_start_try takes call_type and inline_family parameters.
  The first attempt is now the reference phone's exact form (no
  profile, no APN, TLV 0x19 = 4 inline, no call type, txn 0xe8);
  then the qcril shapes with family 4 (e1 profile, e2 profile no
  family, e7 profile+APN, e6 default profile, e3 APN), one ims
  profile+APN form without the call type (e9, to isolate TLV 0x35),
  and APN family 6 (e4).  Family 8 is gone.
* APN TLVs are plain strings (libqmi's "WDS APN Name"): the attach
  parameters, the profile survey, the created-profile read-back and
  the started-call settings all parsed a length prefix that is not
  there ("ims" had printed as "ms").
* The milestone's answer buffer is 1536 bytes, so profile settings
  answers fit and read back.

What to read in the next run, in order: "the modem announced DPM
(0x2f)" and "the modem announced IMS (0x12)"; each "DPM open port
(type ...) says result ..." until "THE DATA PORT IS OPEN"; "WDA set
data format ..." and "the mux bind to mux ..."; then the start lines
-- the reference form first; then either "THE IMS PDN WAS STARTED"
with "the packet service says 2 (connected)" and "the started call
carries the APN", or "the ims PDN was refused in every form" with
its call end reason and the packet status after the refusals.

## Run 124850: the data port opens, Start Network walks from 70 to 14 CALL_FAILED (2026-11-22)

Kernel d9163aae6017db4e.  The user, on the live system: the status
bar drew the cellular item (the kernel's NAS registration, forced on
by userland/src/inject_sb.c), Settings had no SIM pane (the iOS
telephony stack, CommCenter, is not in this userland), and dialling
10099 failed (this network has no CS domain; voice needs IMS, and
the ims PDN is refused below).

* DPM Open Port 0x0020 accepted the first probe -- EMBEDDED type 4,
  interface 1, endpoints 2/16: "THE DATA PORT IS OPEN".  WDA Set
  Data Format (raw IP in QMAP) result 0, error 0.  WDS Bind Mux
  Data Port to mux 1 result 0, error 0.  The reference phone's
  order was the fix.
* Every Start Network form now says result 1, error 14 CALL_FAILED
  (was 70 InvalidOperation without the port): the session reaches
  the network and the network refuses it.  Call end reason TLV 0x10
  is 0x3 (GENERIC_CALL_FAILED); the verbose reason TLV 0x11 is
  type 3, reason 1042, which is not in libqmi's
  QmiWdsVerboseCallEndReason3gpp enum.  The raw bytes were not
  captured: the frame hex dump caps at ~50 bytes of the 52-byte
  answer, so the next kernel logs the TLV bytes explicitly.
* Set IP Family 4 and 6 accepted before the starts; the packet
  service stays 1 (disconnected) before and after.
* The LTE attach is attached 1, ip type 1, APN "ims" -- the attach
  already carries the ims APN, which is either why a second ims
  PDN is refused as a duplicate or a stale reading; the control
  start below separates that question from a broken data path.
* Profile survey: 8 profiles; only profile 1 ("cbnet") carried an
  APN.  Profile 6 is "qdp_profile_ia", the initial-attach profile.
  Create Profile 0x0027 returned {type 0, index 6} -- the write
  landed on the initial-attach slot, and its read-back shows the
  APN "ims" (run 121242 had returned index 7).  This is a store
  Android also provisions, so the driver now never creates
  profiles (write_profile = 0) and the survey logs the TLV 0x10
  name of each profile: the next run's survey shows whether
  Android restored profile 6 after its boot.
* IMS settings 0x12: the WMS-less bring-up reached mhi_ims_enable
  before the modem had announced 0x12, so Bind/Set/Get were skipped
  ("the modem has not announced the IMS settings service"); the
  announcement came later.  IMSA 0x21 was likewise announced only
  after imsa_wait had run, so the registration was never read (the
  bind line never appeared; every state was "not given").

Kernel 869c8109e525102e (built 13:08) carries the next round:

* mhi_ims_enable asks the name service (hello) and looks for 0x12
  for up to 12 s before skipping; it returns 1 once the settings
  were sent.
* The SMS milestone retries the settings and the PDN once when
  0x12 appears after the WMS-less stage.
* No Create Profile; the survey logs each profile's name.
* After the ims refusals, a control start by the survey's first
  non-ims profile (profile 1, "cbnet", family 4, no APN, no call
  type): success logs "THE DATA PROFILE STARTED ... the data path
  works, so the ims refusals above are the ims PDN itself", which
  tells a broken data path from an ims-specific refusal.
* The verbose call end reason logs its four raw bytes and the TLV
  size.

What to read in the next run, in order: "the modem announced IMS
(0x12)"; "the IMS settings service is not known yet" followed by
"the IMS bind says result 0" and "THE IMS, VOLTE AND IMS SMS
SETTINGS SAY result ..."; the profile survey lines with names;
"the ims profile is ...; the data profile for the control is ...";
the start lines and either "THE IMS PDN WAS STARTED" or the
control's "THE DATA PROFILE STARTED"; the verbose reason's raw
bytes; and (if 0x12 came late) "the IMS settings service appeared
late" with a second try.  The profile survey's "profile 6 ..."
line answers whether the modem store came back from Android clean.

## Run 132112: the modem's own CM access list refuses every Start Network (2026-11-22)

Kernel 869c8109e525102e (built 13:08); the scene image was rebuilt at
13:17 to carry `/redwood/bin/sleep_hold 650 &`, and the run stayed
awake for its whole ten minutes ("sleep_hold: idle sleep and display
dim held off") and returned cleanly at 13:31.  The IMS settings (0x12)
and IMSA (0x21) were again announced only after the WMS-less stage, so
the settings were skipped; the IMSA bind itself later answered result
0, but the registration, SMS and voice reads stayed "not given", and
the SMS milestone's late retry did not fire in this run.

* Every Start Network form says result 1, error 14, and the verbose
  call end reason now has its raw bytes: `03 00 12 04`, type 3,
  reason 0x0412 = 1042.  libqmi's QmiWdsVerboseCallEndReasonType is
  MIP 1, INTERNAL 2, CM 3, 3GPP 6, ...; 1042 is
  QMI_WDS_VERBOSE_CALL_END_REASON_CM_ACL_FAILURE.  The refusal is the
  modem's own Call Manager access control list -- not the network's,
  and not an ims-specific rule.  The previous section read type 3 as
  3GPP, which is wrong.
* The control start (profile 1, "qdp_profile_ia", APN "cbnet",
  family 4, no call type) is refused exactly like the ims forms:
  reference form, ims APN family 4, ims APN family 6 and the
  data-profile control all say result 1, error 14.  The data path's
  plumbing is fine -- DPM open port (type 4, interface 1, endpoints
  2/16), WDA raw-IP QMAP format, mux 1 and Set IP Family 4 and 6 all
  answer result 0 -- so the CM refuses every PDN before a bearer is
  set up.
* WDS Bind Subscription (0x00af, value 1 PRIMARY) still gets no
  answer (result and error both print 4294967295), and Get Supported
  Messages (0x001e) does not answer either.  QmiSubscriptionType is
  1-based (DEFAULT 0, PRIMARY 1, SECONDARY 2, ANY 0xff), and
  Android's working SIM sits in logical slot 1 (subId 1), so the
  binding value is plausible but unverified; the QRTR name service
  announces only instance 1 for WDS/NAS/DMS/UIM (the driver drops the
  instance word).
* The profile store after Android's boot names 6 profiles; profile 1
  is "qdp_profile_ia" and carries the APN "cbnet"; no profile carries
  "ims".  Android reprovisioned the store between runs (run 124850's
  profile-6 "ims" is gone), so the driver keeps write_profile = 0.
* The packet service stays 1 (disconnected) before and after, and the
  LTE attach is attached 1, ip type 1, APN "ims".

The same phone under Android (read-only adb, Android 16) is the
control for all of this: the SIM is in slot index 1 / subId 1, China
Broadnet 46015, NR SA (ARFCN 504990), HOME, CS [VOICE, SMS]; IMS is
registered (status 3); an IMS PDN is CONNECTED (network{102}
MOBILE[NR], rmnet_data1, IPv6 MTU 1410, subId 1) with the APNs cbnet
(default,supl,xcap IPV4V6) and ims (ims IPV4V6).  qcril's radio log
shows Xiaomi's PDC machinery activating software MBNs at boot (99
configs, "pdc_get_config_lists_by_type ret_val 0").  So the modem,
the card and the network start PDNs on this hardware; what differs in
a HakiOS boot is the per-boot modem configuration and policy -- the
CM ACL.

The next kernel probes, read-only first: WDS Get Bind Subscription
(0x00b0, output TLV 0x10) to read which subscription the modem
believes it is bound to; a Bind Subscription (0x00af) with value 2
(SECONDARY) logging its answer; PDC Get Selected Config (0x22) on
service 0x24 at node 3 port 0x11 for config types 0 and 1 (Active Id
TLV 0x11, Pending Id TLV 0x12); and, after a bind that answers, one
more Start Network (reference form) to see whether the ACL refusal
follows the binding.

## Run 141245: the subscription binding is not the refusal, and the profile survey only ever read profile 1 (2026-11-22)

Kernel 016380da2ae5 (build 14:07:59) with the same modem-probe scene
and the full argument set; the run returned to Android cleanly after
its 600 s.

* Get Bind Subscription (0x00b0) went unanswered as the first WDS
  message of the boot (txn 0xc6) and again after the primary bind
  (0xc7) -- both are lost-first-message behaviour, not refusals.  The
  third ask, after the retried primary bind (txn 0xcc), answered
  result 0 with TLV 0x10 = 1: "the client is on subscription 1
  (primary)".  Bind Subscription (0x00af) itself went unanswered both
  times it was sent (tlv 0x01 u32 1); this firmware also refuses Get
  Supported Messages with error 71.  Because the read-back names the
  primary, the secondary bind was skipped by design and no start ran
  under another subscription.
* Every Start Network is still refused: result 1, error 14, and the
  verbose reason raw `03 00 12 04` (type 3 CM, 1042 CM_ACL_FAILURE) in
  the reference form, by the modem's own profile 1 with the embedded
  call type, by the "ims" APN in family 4 and family 6, and by the
  data-profile control.  The wrong-subscription hypothesis is dead:
  the modem itself says primary, and the refusal survives it.
* Profile 1 is "qdp_profile_ia" with APN "cbnet" and APN type mask
  0x431 (default, supl, hipri and xcap -- libqmi's
  QmiWdsApnTypeMask bit layout; the driver's older decode read bit 1
  as mms and bit 5 as ims, which is why this entry first called it
  "default, hipri, ims, xcap").  The LTE attach is attached 1, ip
  type 1, APN "ims".
* The profile survey has a real bug, now fixed: the profile-list TLV
  (Get Profile List 0x002a, output TLV 0x01) was parsed from `answer`,
  and every per-profile Get Profile Settings call overwrites that same
  buffer.  Entry 1 was logged before the overwrite; the loop then
  parsed the settings answer as if it were the list and broke.  So
  only profile 1 was ever surveyed, ims_profile was always -1, and
  none of the ims-profile starts ever ran.  Android's own radio log
  shows six 3GPP profiles (ids 1..7; masks 1073 on 1 and 7, 2 on 2,
  512 on 5) plus a 3GPP2 profile at 100.  The survey now copies the
  list to a local 160-byte buffer before the settings calls and logs
  the list bytes.
* The survey also reads each profile's APN enabled (TLV 0xa9) and APN
  class (TLV 0xab).  The firmware's ACL check is
  ds707_data_session_call_allowed; qdsp6sw.mbn carries the deny
  strings "APN not enabled in profile for index i %d,%s" and "APN not
  enabled in UICC card for index i %d,%s", so a 1042 can be told from
  an unprovisioned/disabled APN once those values are on record.

The next run surveys all six profiles and starts by the real ims
profile index (which the broken survey never tried).

## Run 142914: the fixed survey never got to speak -- the PCIe link died before the first WDS frame (2026-11-22)

Kernel 120c5ae97e2b (build 14:26:07, the fixed survey) with the same
scene and arguments.  modem-probe PASSes (BHI sbl1/status/AER) and the
phone returned to Android on its own.  But every WDS message went
unanswered: 144 frames from 1:0x1002 to 3:0x34 and not one reply
(run 141245 on the same link: 285 out, 286 back).  The control
lookup had announced the service at that same node and port
("service 0x1 instance 1 node 3 port 0x34"), so the destination was
right.  Get Profile List (0x002a), Bind Subscription (0x00af), Get
Bind Subscription (0x00b0), Supported Messages (0x001e), Packet
Service (0x0022), LTE Attach Parameters (0x0085), Set IP Family
(0x004d) and Start Network (0x0020) all went unanswered, twice through
the bring-up; the log's own summary is "the ims PDN was refused in
every form (last result 4294967295, error 4294967295)".  Other
services answered normally before that (NAS selection preference
0x0034 result 0 error 0; UIM/SMS/DMS all returned results), and
Android afterwards registers on 中国广电 NR SA.  The survey returned at
its very first call ("the profile list says result 4294967295"),
before the fixed parse could run, so this run carries no profile
evidence either way.

The cause sits below QMI, and it is not the WDS service: the modem's
PCIe endpoint vanished about two and a half minutes into the boot.
The driver's narrations give the order -- the third WMS transport
status query (0x004a, result 1, error 52) at tick 225877450, then
"the SMS transport is not up by itself; the IMS path is brought up"
and "the IMS settings service is not known yet; the name service is
asked and looked for again" at 235877445.  That narration starts the
mhi_ims_enable wait (source line 7402): four rounds of a QRTR hello
plus a 3000000-tick delay, 12000000 ticks in all.  No heartbeat or
stall line accompanies any of the hellos, so their transfers
completed; the fourth found the endpoint gone:

    pcie2: MHI: fault at AMSS-MHI: status 0xffffffff, execenv 0xffffffff
    pcie2: MHI: fault diag: MDM_ERRFATAL 0, MDM_STATUS 1, LTSSM 0x5: link not L0, config not read

at tick 244888523, 9010000 ticks (three completed waits) after the
narration.  The MHI state read all ones and the LTSSM left L0 while
the modem held ERR_FATAL low: the endpoint went away rather than
reporting a crash.  Run 144447 reproduced it frame for frame at tick
242577416, same narration, same register values.

Every send after that dies before the ring.  mhi_send (line 1771)
posts the transfer and mhi_poll's fault check (line 1664) returns
EIO; tx_pending stays set, so mhi_post refuses with EBUSY from then
on and mhi_send returns at once.  That is why the tail of run 144447
has 15507 "TX no completion" lines and zero "without a transfer
completion" heartbeats, zero "TX channel stalls" and zero "erratic
transfer event" lines.  The whole post-fault retry cascade runs
inside one display frame (every line carries the same tick
245577411) -- a free spin, not a hang, and it explains the run-long
flood of unanswered frames: they were logged, never transmitted.
The 0-reply WDS statistics say nothing about the WDS service; the
dead-WDS reading was wrong.

The two bad runs were launched minutes after the previous run ended;
the clean run 141245 followed a long idle and never logged "link not
L0" in any part.  Boot context: 141245 45%, 34-35 C; 142914 42%,
37 C; 144447 38%, 39 C.  That is a correlation, not a cause -- the
modem's own thermal state is not in these logs.  The next run was
launched after ~20 minutes back in Android at 45% and 28.5 C; it
faulted at the same point (next entry), which set the temperature and
power reading aside and moved the probe to the order of the stages.
A link watchdog that stamps LTSSM transitions with the display tick
and a PCIe/MHI reset-and-reinit path stay queued for the case where
the link is lost after the survey has already run.

## Run 151919: the stage order decides it -- an early WMS lets the milestone kill the link (2026-11-22)

Kernel 120c5ae97e2b, same scene and arguments, launched at 15:17 after
21 minutes back in Android (45%, 28.5 C) to test the heat/power reading
of run 142914.  The boot faulted exactly like the two before it: the
third WMS transport status (0x004a, result 1, error 52) at tick
225488553, the "SMS transport is not up by itself" and "name service is
asked and looked for again" pair at 235488588, and the fault at
244499636 -- 9010000 ticks (three 3 s hellos) later, same all-ones
state, same LTSSM 0x5, same ERR_FATAL 0.  Three boots on this kernel,
three identical faults; the cooldown changed nothing.

The narration timelines of the clean boot and the faulting ones differ
in the order of the stages, not their content.  In run 141245 WMS was
not announced until between 238 s and 240 s: the milestone kept
deferring ("the modem has not announced WMS yet" until 235.9 s), and
the WMS-less IMS stage (mhi_ims_standalone) ran first at 210-223 s --
the survey read the six profiles, the starts were refused, and the
milestone only started at 240.3 s with ims_state.brought already set,
after which the run kept its link to 600 s.  In runs 142914, 144447 and
151919 WMS was announced at boot (3:0x39), so the milestone ran first:
its three 0x4a probes at 205/215/225 s, the IMS path narration at
235 s, and the endpoint was gone during the hello wait -- WDS was never
touched.  Whether WMS is announced when the cellular cycle arrives
varies per boot; both orders exist in both kernel builds.

The fix is therefore an ordering one (kernel build after this entry):
mhi_sms_milestone runs the IMS/WDS stage before the milestone whenever
WMS is already announced and WDS is known, setting ims_state.brought
and calling mhi_ims_enable + mhi_wds_ims_up -- the enable/WDS tail of
mhi_ims_standalone without its initial registration look; when WDS is
not announced the old order stands.  The services arrived in the boot
hello in every run seen so far (WDS 3:0x34, WMS 3:0x39 in 151919), so
the survey should now land at ~205-220 s, twenty or more seconds before
the point where the last three boots died.  If the link still dies
after the survey, its evidence is already in the log.

One number for the next probe: in all three faulting boots the death
follows the third 0x4a probe by 19.01-19.02 s (225488553 -> 244499636,
225877450 -> 244888523, 223555617 -> 242577416) -- the hello loop's own
9 s plus one 10 s probe interval.  That anchor is worth watching when
the milestones and the probes are reordered again.

## Run 154032: the reordered bring-up keeps the link, and the attach PDN list becomes the suspect (2026-11-22)

The ordering-fix kernel booted at 15:40.  It carried zero MHI faults,
no "link not L0" and no lost transfer, and ran to the end of its log
(735 s of ticks, "END OF LOG"): the death that took runs 142914,
144447 and 151919 at ~245 s is gone, and the WDS stage ran before it.
What the log holds:

* The six-profile survey finished at 218.54 s: profiles 1, 2, 3, 4,
  5 and 7.  Profile 1 is "qdp_profile_ia" with APN "cbnet" and mask
  0x431; profile 2 carries "ims" with mask 0x2 -- the ims bit alone.
  Under libqmi's QmiWdsApnTypeMask, 0x431 is default + supl + hipri +
  xcap (bit 8, initial-attach, is not set; the name
  "qdp_profile_ia" is what marks profile 1 as the attach profile).
  Profile 3 is 0x31 (default + supl + hipri), 4 is 0, 5 is 0x200
  (emergency).  Every profile's APN-enabled TLV (0xa9) is absent, so
  this firmware does not answer that flag.
* The bind story is unchanged: Bind Subscription 0x00af went
  unanswered twice, Get Bind Subscription says 1 (primary), and the
  client stayed on the primary.
* The LTE attach is attached 1, ip type 1 (IPv6), APN "ims": the
  modem attached from default 3GPP profile 6, whose APN run 124850's
  Create Profile had overwritten with "ims".
* All nine Start Network forms are refused identically: result 1,
  error 14 (CallFailed), call end reason 0x3, verbose `03 00 12 04`
  (type 3 CM, reason 1042 CM_ACL_FAILURE).  The matrix covers the
  reference form, the default profile, the ims profile with and
  without family and call type, the ims APN inline in families 4 and
  6, the ims profile plus APN, and the data-profile control -- not
  one form is accepted, so the refusal is not IMS-specific: every
  data call is denied.
* The SMS milestone ran to its own gate: own number
  +86192991300141, new-message indication on, IMSA not announced,
  transport status error 52, and the SMS-SUBMIT failed its three
  attempts with no answer -- consistent with the data path, not a
  second fault.

The refusal is therefore a policy decision on every start, and the
one known configuration difference from Android is the attach: qcril
sets the modem's LTE attach PDN list to profile 1 -- the cbnet
initial-attach profile -- at every boot; HakiOS never has, so the
modem attached from the polluted default profile 6.  Whether that
list is what CM's access list consults is the question this build
asks.

The next kernel reads the attach list (Get LTE Attach PDN List
0x0094, current and pending; Get Max 0x0092) right after the attach
parameters, sets it to profile 1 when profile 1 is not already named
(Set LTE Attach PDN List 0x0093, list {count 1, u16 1}, action 0x01
none; a refused set is retried with action 0x02
detach-or-PDN-disconnect), and re-reads the attach parameters and the
list just before the start matrix, after the survey's minutes of
waiting.  That is runtime state the modem rebuilds on its own; the
profile store stays read-only.  The survey now also logs the
APN-disabled flag (0x2f) and roaming-disallowed flag (0x3e), and the
mask decode prints the libqmi bit names.

What to read in the new log: "the LTE attach names", "the attach PDN
list is set to profile 1: result", "before the start the LTE attach
is attached", and then whether any start form is accepted.

## Run 165605: the attach PDN list is set to profile 1 and every start is still refused (2026-11-22)

The attach-list kernel booted at 16:56, again with zero MHI faults and
a complete 735 s log, and the phone went back to Android by itself.
What it says:

* At boot the LTE attach ran from profile 6 only: "the LTE attach
  names 1 profiles (the max is 56)", "attach profile 1 of 1 is 6".
  The list the previous run set is not persisted -- the modem rebuilds
  it at every boot -- so setting it belongs to every HakiOS bring-up,
  which is what the driver now does.
* Set LTE Attach PDN List (0x0093, list {count 1, u16 1}, action
  0x01) was accepted: "the attach PDN list is set to profile 1:
  result 0, error 0", and the read-back just before the matrix says
  "before the start attach profile 1 of 1 is 1".
* The attach parameters re-read just before the matrix printed
  "before the start the LTE attach is attached 4294967295, ip type
  4294967295, APN \"\"".  Run 172318's corrected helper later showed
  what that really was: the modem **answered** the read with result 1,
  error 74 (InformationUnavailable) -- the values are not in the
  answer because the firmware refuses the read after the list set, not
  because the read went unanswered.  The code's `got != 0` check had
  taken the refused answer for a successful one.  Whether the attach
  moved to profile 1 is therefore unproven, and the attach-list lever
  is not yet tested in its full form.
* All nine Start Network forms are refused exactly as before:
  result 1, error 14 (CallFailed), call end reason 0x3, verbose
  `03 00 12 04` (type 3 CM, reason 1042 CM_ACL_FAILURE).  Accepting
  the list does not by itself unblock a start.
* The profile flags rule out a disabled APN: every surveyed profile
  says "APN disabled 0, roaming disallowed 0".  The masks are the
  same six as run 154032 (0x431, 0x2, 0x31, 0, 0x200, 0x431), "ims"
  only on profile 2.
* The packet service was 1 (disconnected) before the matrix; the SMS
  milestone again ended on the data path's gate, not on a second
  fault.

Two things follow.  First, the post-set attach has to be read with
retries -- a single unanswered read is not evidence either way -- and
when it still names "ims" while the list says {1}, the list has to be
set with action 0x02 (detach or PDN disconnect) so the modem rebuilds
the attach now rather than at some later detach.  Second, the set
alone did not move CM's access list, so the next thing to vary is
what the start itself asks for: the firmware's ACL denials include
ACL_DENY_REASON_UNPREFERRED_RAT, and none of the nine forms ever
named a technology.

The next kernel: (a) reads 0x0085 up to three times before the matrix
(5 s apart, transaction ids 0xb7-0xb9), and when the answer still is
"ims" (or none, or empty) and the list was set, sends 0x0093 with
action 0x02 and polls 0x0085 three more times (0xbb-0xbd); (b) adds
three start forms after the control -- the reference form with TLV
0x30 = 1 (3GPP allowed), the reference form with TLV 0x34 = 34944
(0x8880, EPC/LTE), and the ims profile with the ims APN and the EPC
preference -- to test the UNPREFERRED_RAT denial.  The profile store
still stays read-only.

What to read in the new log (the kernel after 165605 still logs this as
"went unanswered"; run 172318 corrected both the logging and the
reading, see below): "the attach parameters went unanswered", "after
the detach the LTE attach is attached", "the attach is still APN", and
whether a start with a technology preference is the first accepted one.

## Run 172318: the attach read after the set is refused (error 74), not silent; the EPC preference is rejected outright (2026-11-22)

The attach-poll/detach/technology kernel booted at 17:23 with zero MHI
faults and a complete log, and the phone went back to Android by
itself.  It settles three questions:

* **At boot the attach is healthy and readable.**  Before the list was
  touched, "the LTE attach is attached 1, ip type 1, APN \"ims\"" came
  back with result 0 -- so Get LTE Attach Parameters (0x0085) works on
  this firmware and carries the APN as a plain string.  The attach
  named 1 profile, and it was profile 6.
* **After the list set, the modem refuses 0x0085.**  Set LTE Attach PDN
  List to {1} was accepted (result 0) and read back as {1} before the
  matrix, but every 0x0085 after it -- transaction 0xb7 before the
  matrix, 0xbb after the detach-action set -- is a 7-byte answer that
  is just the result TLV: **result 1, error 74**
  (InformationUnavailable).  The detach-action set itself was accepted
  (result 0), and the modem sent one of its own WDS indications (0x93,
  12 bytes) while the reads waited.  So "attached 4294967295, APN \"\""
  in run 165605 was this refusal seen through a `got != 0` bug, not a
  silent modem: the firmware will not say which APN the attach runs
  while the list is set and the attach has not been rebuilt.
* **The EPC preference does not exist on this firmware.**  The two
  forms carrying Extended Technology Preference (TLV 0x34 = 34944)
  came back **result 1, error 48** (InvalidArgument) with no call
  attempted and call end reason 0xffffffff, while every other form --
  the 3GPP preference included -- is result 1, error 14 (CallFailed)
  with the same CM ACL denial (call end reason 0x3, verbose
  `03 00 12 04` = CM reason 1042).  The final summary line in that run
  quotes the error-48 form only because it ran last.
* NAS still says the modem is out on the network: "REGISTERED WITH A
  NETWORK, 4 bars, -96 dBm (radio 8)" with CS 2 (detached) and PS 1
  (attached) in the serving-system answer.  The refusal is CM's access
  list, not the radio.
* The packet service was 1 (disconnected) before and after the matrix,
  so nothing was connected in the background.

The user supplied two on-device facts the same day: the SIM is China
Broadnet (中国广电), whose data APN is **"cbnet"** -- profiles 1
("qdp_profile_ia") and 7 both carry it, and without it data will not
come up even where voice might -- and in HakiOS the status bar draws
the cellular item with all four bars empty, Settings has no SIM
entries, and calling 10099 does not go through.  The bars sample in
this run ("4 bars, -96 dBm, radio 8") shows the kernel's signal
pipeline does carry a good reading; the empty bars seen on screen
belong to moments before NAS answers, which is a separate userland
gap.

What follows from all this: the attach has to be **rebuilt** from
profile 1 for cbnet to become the attach APN -- the list set alone is
inert while the modem keeps its existing attach -- and the starts have
to name the APN the SIM actually uses.  The next kernel does both:
`mhi_wds_attach_params` now logs the read's QMI result and only fills
its values on result 0, so a refusal reads as one; after the list set
it sends NAS Attach Detach (0x0023, action 2 detach then action 1
attach, transactions 0xf5/0xf6) so the modem rebuilds the attach from
profile 1 during the profile survey; a NAS poll right before the
matrix records the registration the starts are judged against; and the
start matrix gains three cbnet forms (0xf0 reference-shaped, 0xf1
embedded call type, 0xf2 with the 3GPP preference) while the two EPC
forms are gone.  The profile store still stays read-only.

What to read in the new log: "the PS detach says result", "the PS
attach says result", whether the pre-matrix 0x0085 now answers with
APN "cbnet", and whether any cbnet form is the first accepted start.

## Run 175546: the forced re-attach does not rebuild it, and the cbnet APN changes nothing in the refusal (2026-11-22)

The attach-rebuild/cbnet kernel (sha c76f3028ef2d) booted at 17:55
with zero MHI faults and a complete 58-part log (SHA-256
f363b9cb3fe4a7edf16c3dbea8e101672273c72e3b2ecf86527a692b83942c73);
the phone went back to Android by itself.  It closes the
attach-rebuild idea:

* **The list set is accepted, the detach is accepted, the attach is a
  no-op.**  The attach PDN list set to profile 1 says result 0, error
  0 (and reads back {1}); the NAS PS detach says result 0, error 0;
  the NAS PS attach says **result 1, error 26** -- NO_EFFECT, the
  modem was still attached.  The pair did not observably rebuild the
  attach.
* **0x0085 stays refused across the detach.**  Before the matrix and
  again after the detach, the read is result 1, error 74
  (InformationUnavailable): "attached 4294967295, ip type
  4294967295, APN \"\"" is that refusal.  The firmware still will not
  describe an attach it has not rebuilt.
* **The cbnet APN changes nothing.**  The three cbnet forms -- the
  40-byte reference-shaped request was verified byte by byte (TLV
  0x14 "cbnet", 0x16 auth, 0x35 embedded call type), family-4 and
  with the 3GPP preference -- all come back **result 1, error 14**
  with the same verbose call end reason `03 00 12 04` (type 3 CM,
  reason 1042, CM ACL failure).  With them the matrix has now tried
  13 forms: profile 1 (control-shaped, embedded call type, 3GPP and
  3GPP2 preferences), profile 7's APN, the raw APN, HTTPS/MMS/IP
  reference shapes, and cbnet by name -- every one ACL-denied.
  Start Network has no remaining input TLV to vary (checked against
  libqmi's wds.json).
* Registration never wavered: REGISTERED WITH A NETWORK, 4 bars,
  -95 dBm, **radio 8 (LTE)**; packet service 1 before and after, so
  no call was sneaking through in the background.

Two host-side differences from the Android control (run 132112)
remain unexplored, and one of them explains an ACL deny reason the
firmware itself names:

* **The host never asks NAS for a RAT.**  The same phone under
  Android sits on NR SA (ARFCN 504990) with an IMS PDN; every HakiOS
  boot has camped on LTE (radio 8), and the firmware's ACL deny
  reasons include ACL_DENY_REASON_UNPREFERRED_RAT -- if the access
  policy wants NR for data, a host that never asks NAS for NR would
  see every data call denied while camped on LTE.
* The PDC software-MBN activation that Xiaomi's Android performs at
  boot (99 software configs) has never been queried; its answers
  arrive as indications, so it needs new indication pumping.

The next kernel probes the first: after the second data port opens it
reads NAS Get System Selection Preference (0x0034, transaction 0xf7)
and logs the mode-preference bits (GSM/UMTS/LTE/NR5G), then sends NAS
Set System Selection Preference (0x0033, transaction 0xf8) with TLV
0x11 = 0x5c (GSM|UMTS|LTE|NR5G) and logs its result.  The profile
survey that follows gives the modem ~200 s to reselect, and the NAS
poll right before the matrix records whether the serving system moved
from radio 8 to radio 12 (NR5G).  The matrix also gains two more
forms: cbnet with IP family 6 (CBN's APNs under Android are IPV4V6;
transaction 0xfa) and a start by profile index 7 (the second
"qdp_profile_ia" profile, transaction 0xfb).  The profile store stays
read-only.

What to read in the new log: "the modem's RAT mode preference is",
"the ask for GSM, UMTS, LTE and NR5G says result", whether any
REGISTERED line afterwards says radio 12, whether any start is
accepted, and the final packet service.

## Run 182220: the RAT preference was never the lock, and fifteen start forms are fifteen denials (2026-11-22)

The RAT-and-forms kernel (sha 0b893505153d6945) booted at 18:22
with zero MHI faults and a complete 58-part log that ends in END OF
LOG; the phone went back to Android by itself.  It closes the RAT
idea as decisively as the attach idea:

* **The modem already allowed every RAT.**  NAS Get System Selection
  Preference (0x0034) says result 0 with a 288-byte answer whose TLV
  0x11 (Mode Preference) is **0x7f** -- CDMA1x, EVDO, GSM, UMTS,
  LTE, TD-SCDMA and NR5G all set.  The host had never asked NAS for
  a preference, but the modem's own default was already permissive,
  so ACL_DENY_REASON_UNPREFERRED_RAT had nothing to fire on.
* **Setting it anyway is accepted and changes nothing.**  Set System
  Selection Preference (0x0033) with 0x11 = 0x5c (GSM|UMTS|LTE|NR5G,
  clearing the two CDMA bits and TD-SCDMA) says result 0, error 0.
  Over the ~200 s of profile survey that follows, every registration
  line still reads REGISTERED WITH A NETWORK, 4 bars, -95 dBm,
  **radio 8 (LTE)** -- no NR reselection.
* **Fifteen start forms, fifteen ACL denials.**  With cbnet by APN
  in family 4 and family 6, profile 7 by index, and the previous
  thirteen forms, every Start Network is **result 1, error 14**
  with verbose `03 00 12 04` (type 3 CM, reason 1042, CM ACL
  failure).  Set IP Family 4 and 6 both say result 0 -- the modem
  takes the setting and still denies every call.
* **The attach is equally stubborn.**  The list set to {1} says
  result 0 and reads back {1}; PS detach result 0; PS attach
  **result 1, error 26** (NO_EFFECT); 0x0085 before and after
  result 1, error 74 (InformationUnavailable).
* Packet service 1 before and after the matrix; the modem's
  operating mode is ONLINE at every steady-state read (two
  momentary mode-5 readings during warm-up and once mid-run flip
  straight back to ONLINE).

What is left on the host side is the one difference that has never
been observed at all: **PDC**.  Xiaomi's qcril activates 99 software
MBN configurations at boot, and the CM access list that denies every
call is part of the per-boot modem configuration that PDC selects.
HakiOS has never sent a single PDC frame.  The next kernel therefore
does a read-only PDC probe: Get Selected Config (0x22, service
0x24) for Config Type SOFTWARE (1) and PLATFORM (0), draining the
indications that carry the Active and Pending Id TLVs (the existing
call helper returns at the request answer and would miss them), and
List Configs (0x24) with Config Type 1 to count the software
configs the modem offers.  Nothing is written: Set Selected Config
(0x23) stays untouched.  The same kernel walks the TLVs of the
288-byte NAS Get answer so band/acquisition preferences are on the
record too.

What to read in the new log: "PDC" lines -- the get-selected-config
results and the Active/Pending Id hex -- the List Configs count,
and the TLV walk of the NAS answer; then the same start matrix
outcome as before.

## Run 185826: the PDC probe learns the modem announces 0x24 and the whitelist had dropped it (2026-11-22)

The first PDC kernel (sha 83ab8800a001bc02) booted at 18:58 with
zero MHI faults and a complete 58-part log.  Its PDC probe never
ran -- and the reason is the run's own evidence:

* **The modem announces PDC every round.**  The name-service
  dump logged `service 0x24 instance 1 node 3 port 0x11` among the
  forty-odd announcements.  But `mhi_qrtr_remember` keeps only a
  whitelist of services (WDS, DMS, NAS, WMS, UIM, IMS, WDA, IMSA,
  DPM) and 0x24 was not on it, so the lookup the probe makes saw
  "not announced" and skipped -- six times, once per call of
  `mhi_wds_ims_up`.  Fixed by adding 0x24 to the whitelist (and
  "PDC" to the name table); kernel 51c09a1c5e55f963.
* **Everything else held.**  The NAS TLV walk worked and gives the
  whole preference record: TLV 0x11 (mode preference) is 0x7f
  again, 0x13 is `ff 3f`, 0x15 `ff 3f df ff ff e7 01 00`, 0x23
  `ff 3f df ff ff e7 01 00 4e ...`, and 0x2c/0x2d are 64-byte
  band records.  All fifteen Start Network forms are still refused
  result 1, error 14 with `03 00 12 04` (CM 1042); the attach
  still reads result 1, error 74; packet service 1 before and
  after.  Registration: 4 bars, -93 dBm, radio 8.

The next run is the same kernel with the whitelist fix -- the PDC
probe's first real attempt.


## Run 191152: PDC answers, the selected config is id 0, and there are 25 of them (2026-11-22)

Kernel 51c09a1c5e55f963 (the whitelist fix). Clean boot, 58 parts,
no MHI faults. The probe runs once per `mhi_wds_ims_up` call and
answered every time:

* **Every PDC request is acknowledged** result 0, error 0 -- 26
  cycles of List Configs, 24 of Get Selected (platform), 22 of Get
  Selected (software).
* **Get Selected Config SOFTWARE** (type 1): the active id is 20
  bytes, `f1 aa ad 22 07 7c 60 fd ...`, and there is no pending
  id.  That id equals config 0 of the List Configs page.
* **Get Selected Config PLATFORM** (type 0): the active id is 81
  bytes whose first bytes are `2f 64 61 74 61 2f 76 65` = ASCII
  `/data/ve...`, i.e. a filesystem path, not a short hash.  No
  pending id.
* **List Configs SOFTWARE** returns exactly **25 configs**, one
  page per request (indication 0x24 body is 645 bytes: u8 count,
  then 25 entries of {u32 type, u8 len, 20-byte id}).  All are
  type 1, all 20 bytes, all opaque hashes -- no MBN file names.
  The ids begin: 0 `f1 aa ad 22 ...`, 1 `8d be 5c 56 ...`, 2
  `e8 4b c2 20 ...`, 3 `c8 ce 76 54 ...`, 4 `a2 0f 63 50 ...`, 5
  `56 a4 ef cc ...`, 6 `04 86 35 e4 ...`, 7 `e7 08 c1 74 ...`,
  8 `3f 8a 44 24 ...`, 9 `dc 9d 84 06 ...`, 10 `00 4b 4c 7f ...`,
  11 `b8 a8 96 21 ...` (full list in the log).

So the modem's software MBN pool on this device holds 25 configs,
and HakiOS boots with config 0 selected -- by construction, since
HakiOS never wrote PDC; this selection is whatever the modem kept
in EFS from the last Android boot.  The platform selection names a
host path, most likely the QCRIL-generated config file the last
Android session staged under /data/vendor.

The fifteen Start Network forms are still refused result 1, error
14 with `03 00 12 04` (CM 1042); attach still error 74; packet
service 1; registration 4 bars at -93 dBm.  Nothing about the deny
moved.

Next: kernel 485bf828cb98cfba, which raises the hex caps (full ids
in the log) and adds List Configs for PLATFORM so the path list the
modem stages is on the record.  With the full ids we can tell
whether the active 20-byte id is a known MBN hash and decide
whether a Set Selected Config (0x23) experiment is meaningful.
The user has authorized modem-state writes as of this run, so the
write, when we make it, needs no further sign-off -- only evidence.

## Run 192742: full ids, an empty platform pool, and a probe that repeats (2026-11-22)

Kernel 485bf828cb98cfba (hex caps raised, platform List Configs
added).  Clean boot, 57 parts, no MHI faults.

* **The software selection is stable.**  The active id is the full
  20 bytes `f1 aa ad 22 07 7c 60 fd ec f1 ca de 9f 91 63 97 56 f3
  4c d8`, no pending id, identical in all 13 probe cycles of the
  run, and equal to config 0 of the software list.
* **List Configs PLATFORM returns zero configs** -- in all 13
  cycles -- even though the selected platform id is an 81-byte
  path under `/data/ve...`.  The platform pool on this modem is
  empty; only the software pool (25 configs) exists.
* **The probe repeats at every `mhi_wds_ims_up` call** (13 cycles
  around the 15-form start matrix).  The next kernel gates the
  read probe to once per boot, and adds Get Config Info (0x28) for
  every software id: its Description TLV 0x12 is the MBN name
  behind the opaque hash.
* **The matrix is unchanged**: all fifteen start forms result 1
  error 14 with `03 00 12 04`, the attach read error 74, the
  attach rebuild error 26, packet service 1 before and after.
  Registration 4 bars, -94 dBm, radio 8; IMSA still not announced
  (as in every run since 185826).

## Run 194256: all 25 names, and the active config is China Broadnet's (2026-11-22)

Kernel 3035ec6316ccbf4e, boot at 19:42, SOC 31%.  Get Config
Info (0x28) answered for all 25 software ids; every exchange
got result 0.  The pool, in id order:

| id | size | version | description |
|----|------|---------|-------------|
| 0 | 61172 | 0xa011212 | Volte_OpenMkt-Commercial-CBN |
| 1 | 61612 | 0xa01203d | Volte_OpenMkt-Commercial-CMCC |
| 2 | 49112 | 0xa012d02 | YTL_Commercial |
| 3 | 49484 | 0xa015400 | Viettel_Vietnam_Commercial |
| 4 | 54204 | 0xa013d03 | UMobile_Commercial_Malaysia |
| 5 | 44752 | 0xa015009 | TStar_Taiwan_Commercial |
| 6 | 51316 | 0xa012f06 | Truemove_VoLTE_Commercial |
| 7 | 54648 | 0xa014109 | TaiwanMobile_Commercial |
| 8 | 55352 | 0xa01ce02 | XL_Axiata |
| 9 | 53924 | 0xa01ef02 | Indosat |
| 10 | 72088 | 0xa01fc16 | Telkomsel |
| 11 | 52020 | 0xa013301 | StarHub_Singapore_Commercial |
| 12 | 52064 | 0xa013706 | SmarTone_HongKong_Commercial |
| 13 | 53380 | 0xa012523 | SmartFren_VoWiFi_Commercial |
| 14 | 52108 | 0xa013408 | Singtel_Commercial |
| 15 | 43232 | 0xa013900 | P1_Commercial_Malaysia |
| 16 | 46808 | 0xa013202 | M1_Singapore_Commercial |
| 17 | 49520 | 0xa013107 | HKT_HK_Commercial |
| 18 | 51012 | 0xa014600 | Globe_Philippines |
| 19 | 53220 | 0xa01400d | FarEastOne_Taiwan_Commercial |
| 20 | 48632 | 0xa013003 | DTAC_VoLTE_Commercial |
| 21 | 74196 | 0xa014f0c | ChunghwaTel_Taiwan_Commercial |
| 22 | 51696 | 0xa014209 | APT_Taiwan_Commercial |
| 23 | 49768 | 0xa012e06 | AIS_Thailand_Commercial |
| 24 | 51044 | 0xa013603 | 3HK_Commercial_HongKong |

* **The modem already runs the right config.**  The selected
  software id (config 0, the same 20-byte id in every run) is
  "Volte_OpenMkt-Commercial-CBN" -- China Broadnet's own VoLTE
  MBN.  The CM ACL denial is therefore **not a wrong-MBN
  problem**, and there is no other China config in the pool to
  select: every other entry is a Southeast Asia, Hong Kong or
  Taiwan operator.
* **The matrix is unchanged**: all start forms result 1,
  error 14, verbose `03 00 12 04`; the attach read error 74,
  packet service 1 before and after.  Registration, PS attach
  and the UIM (slot 1, ready USIM, primary GW on slot 0) are all
  as before.
* The next kernel:
  * dumps the NAS serving-system TLVs (first 3 answers) so the
    roaming indicator (0x10), data service capability (0x11)
    and true current PLMN (0x12) are on the record -- the PLMN
    parser read 0x10 (the one-byte roaming indicator) before,
    which is why the operator line was always empty;
  * selects and activates the CMCC config (id 1) once the pool
    is named, lets one full start matrix run under it, and
    selects the modem's own config again on the next cycle --
    the one causal MBN experiment left;
  * keeps the rest of the probe read-only.
* After `XL_REBOOT_AFTER` the phone did not come back on adb or
  fastboot within two minutes (it had done so in earlier runs);
  the next run waits for the device to reappear.

## Run 203116: the CMCC switch, the detach action, and the serving-system TLVs (2026-11-22)

Kernel `2.3.4-hakios-darwin22.5-sm8250-g37c040ff-dirty`, boot at
20:31, SOC 36%.  The pool reads were repeated (all 25 Get Config
Info exchanges result 0; id 1 is still `61612`/`0xa01203d`/
"Volte_OpenMkt-Commercial-CMCC"), then the one causal MBN
experiment ran:

* **The CMCC switch itself works.**  Set Selected Config 0x23
  with config 1's 20-byte id
  (`8d be 5c 56 c7 2b a5 38 ad 22 e9 f6 78 c7 5f 0a fb bf fd 0a`)
  answered result 0, Activate Config 0x27 answered result 0, and
  the Get Selected Config read-back named that same id active.
  The modem's own config 0 (CBN,
  `f1 aa ad 22 07 7c 60 fd ec f1 ca de 9f 91 63 97 56 f3 4c d8`)
  was then set and activated again (both result 0) and the final
  read-back showed config 0 active: the modem was left exactly
  as found, and nothing was written to the profile store or any
  partition -- the set/activate pair is runtime modem state only.
* **But the switch was undone before any start ran.**  The
  fifteen-form matrix in this log ran *after* the restore; every
  form is unchanged (result 1, error 14, verbose `03 00 12 04`).
  No Start Network was attempted while CMCC was active, so the
  CMCC-MBN experiment is still **unrun** -- it needs the matrix
  between activate and restore, or a separate boot.
* **The activate disturbed the attach.**  Before the switch the
  attach read (0x0085) answered `attached 1, ip type 1, APN
  "ims"` on the modem's default 3GPP profile **6** (run 124850's
  "qdp_profile_ia", the profile the list bytes name first); after
  the CMCC activate+restore it answered error 74 in every round
  for the rest of the boot and never rebuilt on its own.
* **The detach action is not the lever.**  Setting the attach
  PDN list again with action 0x02 answered result 0, the PS
  domain went down, and the attach reads stayed at error 74; the
  matrix then ran with the attach in that state.  The block is
  removed from the probe (see the driver comment), and the next
  kernel only changes the default profile number and *polls*.
* **The serving-system TLVs are on the record**: 0x10 = `01`,
  0x11 = `01 0b`, 0x12 = `cc 01 0f 00 08 2d 4e fd 56 7f 5e 35
  ...` (13 bytes; the first three decode as MCC 460 / MNC 15 in
  QMI's little-endian form -- the serving PLMN is China Broadnet
  itself), 0x15 = `01 08 01`, 0x1a = `20`, 0x1b = `00`,
  0x1d = `01 9d b1 0f`, 0x21 = `02 02 00 00 00`,
  0x24 = `b4 91`, 0x27 = `cc 01 0f 00 00`.  Registration printed
  "4 bars, -94 dBm, radio 8" every round, but the operator string
  still decodes to garbage -- the PLMN-name formatting is a
  read-side bug of ours, not a network fact.
* IMSA is still not announced ("the modem has not announced the
  IMS settings service"); the IMS PDN was refused in every form
  (last result 1, error 14, call end reason `0x3`).  SMS EF reads
  and the UIM continue as before.
* **The next kernel** (built 22:03, sha `fad6f78d85e80920`):
  * sets the modem's default 3GPP profile number (0x004A) to 1
    and then to 7 -- the lever the radio log shows Android using
    150 ms before its *accepted* Start Network at 20:42 -- and
    reads it back (0x0049) after each set;
  * polls the attach read (0x0085) for 30 s afterwards, without
    forcing a detach;
  * adds the four start forms Android's accepted call actually
    uses: profile 1 + `cbnet` + embedded call type with no family
    TLV (0xfc), profile 7 the same way (0xfd), profile 1 + `cbnet`
    + family 4 (0xfe), and profile 1 alone (0xff);
  * logs each profile's PDP type (TLV 0x11) in the profile dump.

## Run 220529: the default profile set, and every start form still denied (2026-11-22)

Kernel `2.3.4-hakios-darwin22.5-sm8250-g37c040ff-dirty` (sha
`fad6f78d85e80920`), boot at 22:05, SOC 59%.  A note on the
evidence first: this run's *final* log window opens after the
cellular phase and carries only its SMS tail (3 `cellular:`
lines of 26,386).  Everything below is from the `.live.22`--
`.live.25` parts (`grep -al` over the parts, then ordered
dedupe); always mine the parts.

* **The default 3GPP profile set works.**  Set Default Profile
  Number (0x004A, TLV 1 = `type 0, family 0, index`) to 1
  answered result 0, and the read-back (0x0049) read 1; the same
  set to 7 answered result 0 and read back 7.  This is Android's
  own lever -- its radio log sets 1 then 7 a tenth of a second
  before its accepted Start Network.  It is runtime modem state;
  the profile store stays read-only.
* **The PDP types are all IPv4v6**: profiles 1, 2, 3, 4, 5 and 7
  each read `PDP type 3 (IPv4v6)`; the profile list names six
  profiles (`06 00 01 0e "qdp_profile_ia" ... 07`).
* **The attach still runs "ims" and never rebuilds.**  Before the
  set the attach read (0x0085) answered `attached 1, ip type 1,
  APN "ims"` on profile 6; after the set all six polling rounds
  refused error 74.  Read as before: a set with action none takes
  effect only when the modem rebuilds its attach, and this
  firmware does not rebuild it on the default-profile change
  alone.
* **All 19 Start Network forms are denied, unchanged.**  Every
  form -- the reference form, the APN forms (family 4/6, with
  and without the 3GPP preference), the ims-profile forms, and
  the four Android-shaped forms 0xfc (profile 1 + `cbnet` +
  embedded call type, no family), 0xfd (profile 7 the same),
  0xfe (profile 1 + `cbnet` + family 4) and 0xff (profile 1
  alone) -- answered result 1, error 14, verbose call end reason
  type 3 and reason 1042 (`QMI_WDS_VERBOSE_CALL_END_REASON_CM_
  ACL_FAILURE`, raw `03 00 12 04`).  Packet service read 1 after
  the refusals.
* The Set IP Family probes before the matrix (4 and 6) both
  answered result 0: the WDS client is alive and the refusals
  are policy, not transport.
* **Conclusion and next kernel**: the request shape is
  exhausted -- the CM's ACL refuses the call from a state this
  host has not reached.  The one remaining host lever is the
  attach itself: a DMS Set Operating Mode cycle OFFLINE (mode 3)
  then ONLINE (mode 0, retried while the modem comes back) after
  the attach list {1} and the default-profile set, the only
  action that makes this modem rebuild its attach.  The new
  block logs both mode results and the mode read-back
  (`mhi_dms_mode`), then the existing attach poll (extended to
  10 rounds) shows what the rebuilt attach runs.

## Run 225123: the OFFLINE cycle is one-way, and the attach-list set comes 55 s too late (2026-11-22)

Kernel `2.3.4-hakios-darwin22.5-sm8250-g37c040ff-dirty` (sha
`b5d8d1c3f2f63371`), boot at 22:51:23, SOC 60%.

* **The OFFLINE set is accepted and the way back does not exist.**
  DMS Set Operating Mode OFFLINE (mode 3) answered result 0; every
  one of the ten ONLINE retries over the next 50 s answered result
  1, error 60 (`QMI_ERR_INVALID_TRANSITION`), and the final read
  said mode 3 -- the closed window `mhi_dms_online` already
  records (transitions are accepted only while the modem's own
  boot is still settling).  The rest of the run lived with the
  modem OFFLINE: the attach read (0x0085) refused error 74 in all
  ten rounds, the serving system went "not registered", and the
  matrix's denials kept the CM verbose reason 1042.  **The
  mid-run radio cycle is a dead end on this firmware.**
* **The bind was never the blocker.**  The primary bind (0x00af)
  went unanswered twice, but the Get Bind read-back (0x00b0) says
  the client is on subscription 1 (primary).  The WDS supported
  messages read answered error 71 (`INVALID_QMI_COMMAND`), this
  firmware's answer to that convenience query; the client works.
* **Registration and the attach, on the clock.**  The boot read
  the DMS mode at 22:52:00.7 (ONLINE), asked NAS to register at
  22:52:01.9 (result 0), read the system info at 22:52:04.4; the
  first cellular poll at 22:52:38.4 read "REGISTERED WITH A
  NETWORK, 4 bars, -93 dBm (radio 8), operator \"中国广电\" (MCC 460
  MNC 15)".  The IMS phase starts from that poll, gated on
  `registered`; its attach-list set (0x0093, action none) landed
  at 22:52:57 -- 55 s after the registration asked for the attach
  -- and the attach read then still said `attached 1, ip type 1,
  APN "ims"` from the boot list {6}.  The profile survey found
  the SIM's cbnet in profiles 1 and 7 (PDP type 3, IPv4v6); the
  bearer the CM judges every Start against was built too early
  from the wrong profile.
* **PS detach/attach does not rebuild it either.**  The PS detach
  (action 2) answered result 0; the attach (action 1) answered
  result 1, error 26 (`QMI_ERR_NO_EFFECT`: already attached), and
  the attach params stayed "ims".
* The matrix repeated run 220529 exactly: every Start form
  answered result 1, error 14, verbose type 3 / reason 1042;
  packet service 1; the ims forms and the Android-shaped
  0xfc--0xff included.
* **Conclusion and next kernel**: the attach is built when the
  modem registers, so the sets must precede the registration, not
  follow it.  The next kernel sends Default Profile Number 1 and
  the LTE Attach PDN list {1} from the boot right before NAS
  initiate register (`mhi_wds_early`), retried from the first
  cellular poll cycles when WDS is not announced yet, and drops
  the mid-run OFFLINE block.  Evidence to watch: the early lines
  ("the attach right before the registration is attached ... APN
  ...") and the IMS-phase attach read now saying "cbnet".

## Run 234850: the accepted set reverts to {6} by the attach, and Android re-sends it all through boot (2026-11-23)

The single pre-register set (`mhi_wds_early`, kernel c5e3aa52) was
accepted exactly as intended -- and did not survive:

* **The set went out on time.**  `before the set the default 3GPP
  profile reads 7`; `the default 3GPP profile is set to 1: result 0,
  error 0`; `the attach PDN list is set to profile 1: result 0,
  error 0`; the read-back `after the set the attach PDN list names 1
  profiles, first 1 (cbnet)` at 23:49:27.9.  NAS initiate register
  went out 0.8 s later at 23:49:28.77 (result 0).
* **The modem still attached on its own list.**  The registration
  read at 23:50:06.366 was already good -- REGISTERED, 4 bars,
  -95 dBm, 中国广电 460/15 -- and the attach read came back
  `attached 1, ip type 1, APN "ims"`; the IMS-phase survey then found
  `attach profile 1 of 1 is 6`.  The list the boot set had been
  replaced by {6} before the attach was built.  Every Start form in
  the matrix was refused 1/14/1042 again (23:53:19).
* **{6} does not exist in the runtime profile store.**  The survey
  lists six profiles: 1 `qdp_profile_ia` cbnet (mask 0x431), 2
  `ims`, 3 `CMNET`, 4 `CMWAP`, 5 `SOS` (emergency), 7
  `qdp_profile_ia` cbnet -- and no 6.  So {6} is not a WDS profile
  the attach list names; it is the modem's own initial-attach
  configuration (EFS/NV/mcfg level), re-applied when the attach is
  built.  A runtime 0x0093 set is answered 0 and then overwritten.
* **Android's control shows the message itself is right, the
  repetition is the difference.**  The radio log around Android's
  own boot: qcril sends SET_INITIAL_ATTACH_APN with the cbnet
  profile (`中国广电 (CBN) GPRS, profile 3749, 46015, cbnet, types
  supl|hipri|default|xcap`) at 00:11:37.232, 00:11:38.245,
  00:11:38.248 and 00:11:42.421 -- one of them answered `error 2`
  (GENERIC_FAILURE) and retried -- maps each to "Attach pdn list
  profile = 1" (the same WDS 0x0093), and sets default profile num 1
  then 7.  Same messages this driver sends; Android sends them
  repeatedly, all the way to its data call.
* **Next kernel**: `mhi_wds_early` takes the stage's name and is
  called at three boot stages -- after the DMS before-card block
  (before the card work), after the UIM switch/provisioning (after
  the card work, before the DMS after-card read), and right before
  the NAS register -- each doing read -> set -> read-back, plus the
  poll-thread retry when WDS was not announced.  What the next run
  measures: the first stage whose read-back says 6, which brackets
  the exact moment the modem reverts the list, and whether seeding
  again after the card work keeps it at 1 through the register.

## Run 002738: the list holds {1} through the register and the attach still runs profile 6, which run 124850 overwrote with "ims" (2026-11-23)

The three-stage seeding worked as designed and proved the attach does
not follow the runtime list at all:

* **Every stage read back {1} cbnet.**  `before the card work`
  (default 7 -> 1), `after the card work` (default 1), `before the
  registration` (default 1): `result 0, error 0` each, and after each
  set `the attach PDN list names 1 profiles, first 1 (cbnet)`.
* **The modem was ONLINE on its own before the card block.**  The DMS
  reads said `before the card the modem is ONLINE` and `after the EFS
  sync, the modem is ONLINE after 0 ONLINE requests`: it registers and
  builds the attach on its own schedule, early, so no set made from
  this boot can precede it.
* **The attach at the IMS phase was still `attached 1, ip type 1,
  APN "ims"`**, with `the modem's default 3GPP profile is 6` and
  `attach profile 1 of 1 is 6` -- the modem had reverted both runtime
  values to its stored initial-attach configuration.  The 0x0085
  reads inside the stages did not answer (74 INFORMATION_UNAVAILABLE:
  no attach had reported itself yet), the attach itself carried ims,
  and every Start was refused 1/14 again.
* **Profile 6 is the write our own probe made.**  Run 113605's survey
  listed profile 6 as `qdp_profile_ia` (the initial-attach profile the
  modem boots with) and its Create Profile landed on **7** with APN
  "ims".  Run 124850's Create Profile landed on **6** --
  `THE IMS PROFILE WAS CREATED as 6 (type 0)`, `the created profile 6
  reads back with the APN "ims"` -- overwriting the IA profile's APN
  in the store.  The current 0x002a survey (profiles 1,2,3,4,5,7 with
  the cbnet `qdp_profile_ia` pair at 1 and 7) no longer lists 6, but
  the attach list and the default profile still name it, and its APN
  is what the attach carries.
* **Android never writes 6 by hand; qdp does.**  The radio log shows
  `SET_PROFILE_PARAMS_SYNC` (a qdp profile write of profile 1 with
  the cbnet APN) -> `setProfileDefault` -> `Attach pdn list profile =
  1`, repeated at 00:11:37.2/.6, 00:11:38.2/.5, 00:11:42.4, and DPM's
  `Initial attach data profile updated ... forceUpdateIa= true`.  The
  initial-attach APN is written into the store (the IA slot), which is
  exactly the slot our Create Profile clobbered.
* **Next kernel**: read profile 6 (0x002b {0,6}) and log name/APN/PDP/
  IMCN; modify its APN back to the SIM's `cbnet` (Modify Profile
  0x0028, with a Create Profile 0x0027 fallback), read it back; then
  set the attach list to {6} with action 0x02 (detach-or-disconnect)
  to make the modem rebuild the attach at once, and read 0x0085/0x0094
  after it.  The Start matrix then says whether the CM's access list
  accepts cbnet with the repaired attach.

## Run 013903: the reference's own shapes, byte for byte, are refused the same 1042 (2026-11-23)

The 01:32 kernel carried the qcrild capture from the running phone's
01:16 airplane re-setup (strace: bind mux `{type 3, interface 4}` +
mux 3, bind subscription 2, Set IP Family, Start body `16 01 00 00 19
01 00 04|06 35 01 00 01 39 01 00 01 3d 01 00 01 3e 08 00 02...`):

* **The reference's subscription bind is taken; the primary's never
  was.**  `the bind to subscription 2 (the reference's) says result 0,
  error 0`, and the read-back `after the bind to 2 the client reads
  subscription 2 (secondary)` -- run 225123's primary bind (0x00af,
  value 1) went unanswered.  The client now sits where qcrild's
  clients sit, and nothing changed.
* **The Start body replicated to the byte is still refused.**  All
  four forms (ims mask 2 and default mask 1, families 4 and 6, with
  0x39/0x3d = 1 and Set IP Family 0x004d first) answered result 1,
  error 14; the 24 forms after them answered the same; the verbose
  call end reason stayed type 3, reason 1042 (`03 00 12 04`,
  CM_ACL_FAILURE).  The captured body and this driver's are the same
  31 bytes in a different TLV order, so the order was nothing.
* **The attach list this boot was the seeded {1} cbnet** (`before the
  start attach profile 1 of 1 is 1`), yet the attach parameters read
  (0x0085) never answered -- error 74, rounds 1-3 at the phase start
  and rounds 1-10 after the default-profile set.
* **The PDC experiment was off during the matrix.**  The CMCC config
  was selected and activated (both result 0), the modem's own active
  id `f1 aa ad 22 ...` was selected again (both result 0), and only
  then did DPM open the port ({4,1}, result 0), WDA set the format
  (result 0) and WDS bind mux 1 (result 0).
* **What remains different is the data port pair.**  The phone's own
  clients bind `{type 3 PCIe, interface 4}` with mux 3 (one legacy
  client mux 2); this driver opened `{4,1}` and bound mux 1.  The next
  kernel tries the reference's `{3,4}` port first, then mux 3, 2, 1,
  0, and sends the Start body in qcril's order (0x16 first).

## Run 015549: the reference's own port, mux and subscription are accepted, and the start is still refused 1042 (2026-11-23)

The 01:46 kernel (qcril's byte order, DPM tries starting with the
reference pair `{type 3, interface 4}`, mux tries 3, 2, 1, 0) booted
at 01:55:49.  The three data-path differences that run 013903 still
had against the captured Android client are now closed, and the
matrix's verdict did not move:

* **The reference's port is accepted.**  DPM open port (type 3,
  interface 4, endpoints 2/16) says result 0, error 0 -- the exact
  pair qcril binds in qmi3.dec, instead of the `{4,1}` the driver
  had been using.
* **The reference's mux is accepted.**  The mux bind to mux 3 of
  that data port says result 0, error 0 (the tries are 3, 2, 1, 0).
* **The reference's subscription is accepted.**  Bind to
  subscription 2 says result 0, and Get Bind Subscription reads
  back secondary.
* **The reference's body, in the reference's order, is refused.
  The request frame is 31 bytes of TLV body -- `16 01 00 00 19 01
  00 04 35 01 00 01 39 01 00 01 3d 01 00 01 3e 08 00 02 00 00 00
  00 00 00 00` -- byte for byte the body in qmi3.dec (TLV 0x16
  first, then 0x19, 0x35, 0x39, 0x3d, 0x3e), sent on the bound,
  bound-to-2 client.  It answers result 1, error 14 and verbose
  reason type 3, reason 1042 (CM_ACL_FAILURE) again, as do the
  family 6 twin and the default-mask forms.
* **The ACL denies a plain data call too.**  The matrix's
  default-mask and cbnet-APN forms -- no IMS mask, no IMS profile,
  a plain `cbnet` data call -- are refused with the same 1042.
  The denial is not about the APN type: on this boot the CM
  refuses *every* start, IMS or not.
* Context unchanged: attach read error 74, packet service 1
  (disconnected) before and after, NAS serving system registered
  (TLV 0x10 = 01) the whole run.  UIM: logical slot 1 is pointed
  at slot 2 where the card is ready (single-card remap), card
  USIM ready, EF_MSISDN without a path reads records fine.
* Next probe: capture the *responses* on the Android side
  (`sendto,recvfrom,recvmsg` while airplane mode rebuilds data)
  to learn whether the same Start actually succeeds there, what
  its response TLVs are, and what qcril does right after -- and,
  if it does succeed, diff the two clients' state instead of
  guessing at the ACL.

## The two-way Android capture: the reference start succeeds from a fresh client, and its v4 twin fails for a different reason (2026-11-23 02:18)

The 01:16 capture (qmi3.trace) was sendto-only: it showed qcrild's WDS
frames but not the modem's answers.  A second capture traced **both
directions** (sendto/recvfrom/recvmsg, strace -xx, qcrild 2051/2075,
qmipriod 1852, imsqmidaemon 1926) across an airplane-mode data rebuild
(qmi5.trace, 234 616 bytes, decoded by /tmp/dec4.py to qmi5.dec).  It
answers the question the ACL work was waiting on: **the same Start body
that HakiOS sends and is refused with result 1, error 14, verbose CM
reason 1042 is accepted on Android** -- from a brand-new WDS client, and
only in its IPv6 form.

### The fresh clients' whole WDS lives

pid 2378 (qcrild) opened two new qrtr sockets for the rebuild.  Each one's
entire WDS traffic, in order:

* **fd=101** (tries family 4):
  - Bind Mux Data Port 0x00a2: TLV 0x10 = `03 00 00 00 04 00 00 00`
    (endpoint type 3 PCIe, interface 4), TLV 0x11 = `02` (mux **2**) ->
    **result 0**
  - Bind Subscription 0x00af: TLV 0x01 = 2 -> **result 0**
  - Set IP Family 0x004d: TLV 0x01 = 4 -> **result 0**
  - Start Network 0x0020, 31 bytes: `16 01 00 00 | 19 01 00 04 | 35 01 00 01
    | 39 01 00 01 | 3d 01 00 01 | 3e 08 00 02 00 00 00 00 00 00 00` ->
    **result 1, error 14**, TLV 0x10 (verbose call end) = `01 00` (type 1,
    reason 0), legacy TLV 0x11 = `02 00 d0 00` (type 2, reason **208**) --
    *not* CM 1042.
  - Get Current Settings 0x002d, TLV 0x10 (requested settings) = 0x800 ->
    result 0, no address TLVs.
* **fd=104** (family 6):
  - Bind Mux Data Port 0x00a2: same {3, 4} port, TLV 0x11 = `02` (mux
    **2**) -> **result 0**
  - Bind Subscription 0x00af = 2 -> **result 0**
  - Set IP Family 0x004d = 6 -> **result 0**
  - Start Network 0x0020, 31 bytes, **only TLV 0x19 differs** (`06` in
    place of `04`) -> **result 0**, TLV 0x01 packet data handle
    `00 6e ac 6e`
  - Get Current Settings 0x002d, TLV 0x10 = `10 82 00 00` (0x8210) ->
    result 0, TLV 0x26 = `24` + `240a:42ea:0810:dd74:4857:a8e0:e1e6:5db8`,
    TLV 0x27 primary DNS `2409:8080:2000:0003::1`, TLV 0x28 secondary
    `2409:8080:2000::2`, TLV 0x2b IP family = 6.

The long-lived client fd=82 (the default-data one) re-bound mux **2**,
bound subscription 2, set family 6 and read the packet service as
**connected (2)**, then set family 4 and read it **disconnected (1)**:
changing the family tears the session down, which is why the driver's own
"set family, start, set family, start" matrix on one client can only ever
have one live family at a time.

### What this changes

1. **CM 1042 is not the network's answer to the body.**  Android's modem
   -- same MBN (Volta_OpenMkt-Commercial-CBN), same firmware, same NV
   profiles -- accepts the identical 31-byte start (family 6) with result
   0.  The ACL denial HakiOS sees belongs to the state or identity of the
   client that sends it, not to the request.
2. **The only structural difference left is the client.**  Android's
   accepted start comes from a fresh qrtr socket whose whole WDS history
   is bind mux, bind subscription, set family, start.  HakiOS sends the
   same 31-byte start from a single long-lived client (QRTR src port
   0x1002) that by then has carried every profile write, default-profile
   set, attach-PDN-list set, subscription bind, mux bind and twenty-odd
   refused starts of the boot.  The next kernel gives the reference start
   a client of its own to test exactly that.
3. **Mux 2 is the reference rebuild's mux.**  The 01:16 boot capture had
   fd=101/104/107/110 bind mux 3 (fd=85 bound 2 and later 3), while the
   02:18 rebuild bound mux 2 on every WDS client.  Both are accepted, so
   the mux is not a fixed requirement -- but the fresh-client probe binds
   mux 2 first and mux 3 second to cover both.
4. **IPv6 is the form to dial.**  CBN's IMS PDN refuses the IPv4 start on
   Android too (verbose type 1 reason 0; legacy type 2 reason 208).  The
   driver's v4 forms cannot succeed against this network; the v6 form is
   the one to carry forward.

### The next probe (in the new kernel)

`mhi_wds_ims_up` now, right after DPM/WDA, runs a fresh WDS client: QRTR
src port 0x1005 binds mux 2 on the opened {3, 4} port (0x00a2), binds
subscription 2 (0x00af), sets family 6 (0x004d) and sends the exact
31-byte start on that client -- its first and only WDS message of each
kind.  If the ACL refusal is the shared client's state, this start is
accepted and Get Current Settings on the same client (TLV 0x10 = 0x8210)
returns the ipv6 address, DNS and family.  If it is refused 1042 the same
way, a second fresh client (0x1006, mux 3) rules out the mux, and the ACL
is not per client: the remaining axis is the modem's call-manager state
itself.

## Run 023826: qcril's own mask-2 ims profile is created in the store, and the detach form differs (2026-11-23 02:38)

The two-way Android capture (qmi5.dec) shows qcrild's WDS client read
the profile lists and then send Create Profile 0x0027 with seven TLVs --
profile type 1 (3GPP2), 0x8f = 0, 0x9a = 0, extended APN "ims" (0xa1),
extended IP type IPv6 (0xa2), 0xad = 0, and the 8-byte APN type mask
`02 00 ...` (0xdd) -- and the modem answered {type 1, index 104}.  The
kernel reproduced that create byte for byte:

* The store read first: profile 1 type 0 mask 0x431 (name
  qdp_profile_ia, APN cbnet); profile 6 type 0 (the default attach
  profile) read error 81 on the first probe but its repair modify
  answered result 0 and it reads back cbnet 0x431; profile 0 type 1
  mask 0x200; qcril's type-1 slot 104 read error 81 (never created on
  this phone's Android); **profile 2 of type 0 carries the APN "ims"**
  (run one-192742).
* The create landed at **type 1 index 100 with mask 0x2** (the type-1
  store's next free slot on this phone, not Android's 104); the
  read-back's name/APN fields came back empty, which is the read
  parser's artifact, not the write's answer.
* The starts after the create were still refused 1042: the mask-2
  profile alone does not open the CM ACL.
* The libqmi-era NAS detach (Attach Detach 0x0023, TLV 0x10, one-byte
  action) had answered result 0 then NO_EFFECT 26 in runs 175546,
  203116 and 023826 -- the modem never acted on it.  Android's own
  frames carry the action in **TLV 0x16 as a 4-byte value** (qmi5
  line 1112, qmi3 lines 151/289); the kernel now sends that form
  between the two fresh clients.

## Run 035648: a second kernel in one loader session is not a usable state (2026-11-23 03:56)

Booting a second kernel without cycling through Android left the
modem in `mdm2ap status 1, errfatal 1`, GPIO leftovers 0xc3/0x3c3 and
PCIe "no link training done: 0x3001" -- no IMS probe at all.  The
loader session rule stands: `./xl reboot` to Android (the Android
kernel resets the modem), wait for `android`, then `./xl enter` for a
fresh loader session.

## Run 041147: the mask-2 profile and a fresh client do not lift 1042, and the PDC block never ran (2026-11-23 04:11)

* **Fresh client 0x1005** (mux 2, subscription 2, family 6, the exact
  reference body including TLV 0x3e mask 2) -> result 1, error 14,
  verbose TLV 0x11 `03 00 12 04` = type 3 (CM), reason 1042
  (QMI_WDS_VERBOSE_CALL_END_REASON_CM_ACL_FAILURE).
* **NAS detach in Android's TLV 0x16 form** (action 2, 4-byte) got
  **no answer at all** on this firmware -- a new datum against the old
  TLV 0x10 form, which answered result 0 then NO_EFFECT 26.
* **Fresh client 0x1006** (mux 3) sent the same body after the detach:
  identical refusal (1 / 14 / type 3 / 1042).  Packet service 1
  (disconnected) both times, NAS registered throughout.
* The forced re-attach did move the attach: "attached 1, ip type 2,
  APN cbnet" after the action-2 set on profile 6; the attach PDN list
  set to profile 1 answered result 0.
* **Authoring bug found by the zero PDC lines:** the fresh-client
  block sits above the NAS RAT-preference block, the PDC read block
  (Get Selected / List / Get Config Info) and the CMCC select in
  `mhi_wds_ims_up`, and its refusal path `return 0` skips them all.
  Run 194256 proved the PDC read works, but every boot since the
  fresh-client block was added (023826, 041147) ran without a NAS
  preference set and without a single PDC request.  Fixed: both blocks
  now run before the fresh matrix, and the CMCC undo is the helper
  `mhi_pdc_restore_own`, called on the refused matrix's return path
  (an accepted start leaves the restore to the next call so the live
  session is not disturbed).  The CMCC select/activate experiment --
  run 194256's active config is "Volte_OpenMkt-Commercial-CBN" with
  CMCC at index 1 -- runs in the next boot under this kernel.
* **Battery datum:** battguard held 30-35% through the run.  Forty
  minutes later Android first read 10-12% and then settled at 29-30%
  with the charge counter and cell voltage rising under a 500 mA USB
  charger -- that 10% was the known fuel-gauge glitch (this phone
  already jumps to 9% for a second), not a real drain; it does mean a
  glitch landing under 25% can end a HakiOS run early through
  battguard.

The next discriminators, in order: (a) the start matrix under the
CMCC PDC config (coded, first run pending), (b) Start Network with TLV
0x31 (Profile Index 3GPP) naming the mask-2 profile explicitly,
against the mask-based resolution, and (c) the DPM/WDA calls qcrild
makes before its accepted dial (already matched by the driver's data
path).

### The DPM mux bind parity gap (found while re-reading qmi5.dec)

The reference's DPM client (fd=107) sends two DPM messages immediately
before its starts: **Bind Mux Data Port 0x002b**, TLV 0x10 as two u32
`03 00 00 00 04 00 00 00` ({type 3 PCIe, interface 4}) plus TLV 0x11
`02` (mux 2) -> result 0, then **message 0x002d**, TLV 0x01 as u32 2 ->
result 0.  Until this kernel the driver only sent DPM Open Port 0x0020
and WDS's own Bind Mux Data Port 0x00a2 -- it never sent either DPM
message.  If the CM access list is keyed by the DPM port mapping, every
start without it is ACL-refused no matter how the WDS client is shaped.
`mhi_wds_data_path` now sends both frames byte-for-byte (mux 2) right
after WDA Set Data Format, and logs their results.

To keep one variable per boot, the CMCC select/activate experiment is
now gated on the boot argument `pcie2_pdc_cmcc=1` (default off), so the
next boot tests DPM parity alone; if its matrix still refuses, the
following boot arms CMCC.

## The reference's DSD conversation, decoded from qmi5 (2026-11-23 08:20)

The APN-manager conversation is DSD (service 0x2A), node 3 port 0x3b.
libqmi defines only a few of its messages: 0x0024 Get System Status
(output TLV 0x10, a one-byte-count array of {u32 technology, u32 RAT,
u64 SO mask}), 0x0025 System Status Change (its input TLV is 0x11
Register Indication; the captured clients instead send vendor TLV 0x10
= 0/1), 0x0026 System Status indication (same array), 0x0033 Get APN
Info and 0x0051 Set APN Type.  0x0068-0x006c are vendor messages.

The two clients that are active around the data call (qmipriod pids
49/42) each send, in this order:

```
0x0025 TLV 0x10 = 1             -> result 0
0x0069 TLV 0x10 = 0             -> result 0
0x0025 TLV 0x10 = 0             -> result 0
0x0024 (no TLVs)                -> TLV 0x10 system {tech 0, RAT 6,
                                   mask 0x0000130000000000},
                                   TLV 0x11 APN list, TLV 0x13 APN list
0x0069 TLVs 0x10/0x11/0x12 = 1  -> result 0
0x0068 (no TLVs)                -> result 0, TLV 0x13 = u16 10 or 11
```

pid102 sends a different order (0x0025=0, 0x0024, 0x0025=1, 0x0025=1,
0x0025=0, 0x0024) and reads the same 0x0024 answer: a six-entry list
("", cbnet, CMNET, CMWAP, ims, SOS), every entry's flag = 1 in TLV
0x11; TLV 0x13 repeats the names with flag 0 and a 32-byte system per
entry (the 0x11 entries carry 16 bytes).  pid92's shorter client sees
only ims and cbnet.  The modem also pushes 0x0026/0x006b/0x006c
indications carrying the same system struct and APN lists (one carries
ims with flag 2; one carries TLV 0x11 empty and TLV 0x13 = ims, cbnet).
The 0x0025 answers never carry more than the operation result, and
every answer in the capture is result 0.

The driver's first replay (mhi_dsd_arm) sent one merged sequence:
0x0025=0, 0x0024 (with the 0x11/0x13 dumps), 0x0025=1, 0x0069 three
ones, 0x0068, 0x0024 again.  Run 083846 found it never executed (the
section below); it now follows the pids 49/42 order exactly and runs
before the first Start.  It still uses one source port (0x1007) where
the reference uses separate clients.

## Runs 081505/082143/082859: the DSD kernel never ran -- an invocation fault, not a kernel bug (2026-11-23 08:29)

The first three boots of the DSD kernel ended at
`load_init_program: attempting to load /sbin/launchd` plus three
`cpu_signal failed in DebuggerXCallEnter` lines about 30 s in, and
`xl run` returned after ~45 s.  That reads as a launchd-exec panic, but
the fault was `.scratch/run-dsd.sh`:

* It did not run `./xl enter` / `./xl ramdisk userland/system.apfs` and
  did not pass `debug=0x8 rd=md0`, so the loader had no resident root:
  the log header lacks `resident image at pa fc000000 ... it is the
  root (md0)` and prints the UEFI window-choice lines instead (`largest
  free block`, `phys window`, `slpi heap range`).  Without a root the
  launchd exec panics.  The payload's cmdline was 0x18b (395 bytes)
  against the healthy runs' 0x1dc (476).
* It did not pass `pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1
  pcie2_sid=5 pcie2_mhi=1`, so the modem driver never started: zero
  `pcie2:` lines in all three logs (only the Wi-Fi MHI driver's
  `UMI PCIe:` lines).  None of the three boots could have run the DSD
  conversation.

The canonical cellular invocation is `.scratch/run4.sh`: `./xl enter`,
`./xl ramdisk userland/system.apfs`, then `XL_CMDLINE="debug=0x8 rd=md0
$(cat devices/thyme/ram.args) pcie2_test=1 pcie2_bhi=2 ufs=2
pcie2_msi=1 pcie2_sid=5 pcie2_mhi=1"` with XL_SECS=600.  A cellular run
is only an experiment when its log header carries the resident-image
line and its payload cmdline is the 0x1dc-byte one.  The early-panic
signature itself is not new to this port: seven older short logs
(203358, 214725, 215739, 221053, 234257, 234839, 235222) stop with the
same three dbgx lines at 16-87 s.

## Run 083846: the full boot ran, the replay was dead code behind the refused Start (2026-11-23 08:52)

The canonical run4.sh boot of the DSD kernel was healthy this time:
cmdline 0x1dc, `resident image at pa fc000000 ... root (md0)`, 781
`pcie2:` lines and the whole cellular matrix.  It still produced no DSD
line at all.  The reason is the call order, not the DSD service:

* The IMS stage's two fresh clients (0x1005 v4 leg first, 0x1006 v6
  second) ran their starts at 08:41:51 and were refused again:
  `the fresh client 0x1005's start is refused result 1, error 14,
  verbose reason type 3 1042 -- its only WDS traffic was the reference's`.
* `both fresh clients' starts are refused like the shared client's; the
  shared client's start matrix is not repeated` followed at 08:42:01,
  and mhi_wds_ims_up returned there -- before the old mhi_dsd_arm call
  site, which sat after the whole start matrix next to
  mhi_cellular_poll.  The replay could never run in exactly the boots
  (1042 refused) it exists to fix.

The evidence came from the kept 2-second `.live` snapshots
(XL_FOLLOW_KEEP=1): a run's ~1.3 MB log is a 256 KB head plus a ~1 MB
ring, so a line three minutes old has scrolled out by the time a later
snapshot is read.  Grepping all 256 kept snapshots found the refusal at
08:41:51 and the early return at 08:42:01; the final log keeps neither.

The fix, in this order:
* `mhi_dsd_arm` is now called before the fresh-client block (the first
  Start in the function), matching the capture, where the conversation
  is the last host action before the accepted dial.  The old call stays
  as the fallback for boots whose data path never opened.
* The replay now follows the pids 49/42 order exactly: 0x0025(1),
  0x0069 TLV 0x10 = 0, 0x0025(0), 0x0024 with the 0x11/0x13 dumps,
  0x0069 three ones, 0x0068, 0x0024 again.
* A one-per-boot guard (`replayed`) keeps the two call sites from
  sending the conversation twice.

The next canonical boot should show the six `pcie2: cellular: DSD:`
outcome lines before the fresh clients' starts, and the fresh clients'
first Start after the replay is the test of the ACL theory.

## Run 100618: the DSD replay ran and the ACL still said no (2026-11-23 10:06)

The replay did run before the first Start this time, and every one of
its six messages was answered result 0, error 0: 0x0025(1), 0x0069
TLV 0x10 = 0, 0x0025(0), 0x0024, 0x0069 with 0x10/0x11/0x12 = 1, 0x0068,
0x0024 again.  The APN list dumps are the new evidence from it:

* TLV 0x11, 130 bytes: `cbnet`, `CMNET`, `CMWAP`, `ims`, `SOS`, each
  entry's flag byte 01, before and after the 0x0069 set.
* TLV 0x13, 220 bytes: eleven entries, the same names plus unnamed
  slots.

So the data-system service holds an armed, answered APN table, and
arming it did not move the refusal:

* fresh client 0x1005 (mux 2, subscription 2): bind result 0, error 0;
  start refused result 1, error 14, verbose `03 00 12 04` (type 3 CM,
  1042 CM_ACL_FAILURE); packet service 1 (disconnected).
* fresh client 0x1006 (mux 3, subscription 2): the same three answers.

Two other results of the run:

* The shared 0x1002 client's WDS pre-bind loop (`mhi_wds_data_path`,
  muxes 3, 2, 1, 0) was refused by every mux with result 1, **error
  71** -- `QMI_PROTOCOL_ERROR_INVALID_QMI_COMMAND` (libqmi
  `qmi-errors.h`), the answer for a command invalid in the client's
  state.  The QOS service's Bind Mux Data Port (0x002b, {3, 4}, mux 2)
  and its 0x002d = 2 were result 0 in the same function, and the fresh
  clients' byte-identical 0x00a2 frames were accepted later in the same
  boot -- 71 is client state, not a frame defect.
* PDC Get Config Info answered for all 25 configs (0
  "Volte_OpenMkt-Commercial-CBN" 61172 bytes, 10 "Telkomsel", 11
  "StarHub_Singapore_Commercial", 12 "SmarTone_HongKong_Commercial",
  ...).  SMS is still refused.

## What Android's own dial actually sends: the 09:44 window

The boot capture's window 09:44:22.4-23.95, decoded in full, is the
reference for every host action between the DSD conversation and the
accepted dial.  It changes the parity picture in three ways -- the
driver's fresh clients bound mux 3 on the second client and
subscription 2 on both, and did none of the steps in between:

* 22.681-22.737 NAS housekeeping: 0x0002 TLV 0x12 = 1, vendor 0x0070
  with TLV 0x01 (u64) = `07 00 00 01 00 00 00 00`, Set Event Report
  0x0003 with 15 TLVs, 0x0075 TLV 0x01 = 1.  The same shapes recur at
  09:43:53/58 and 09:44:02, so they are periodic, not dial gates.
* 22.765/.777/.788 DSD: 0x0025 TLV 0x10 = 1 from two netmgr clients,
  then 0x0069 TLV 0x10 = 0 from one of them.  These are the last DSD
  words before the dial, 1.07 s ahead of it.
* 22.801/.808 p0x42 fd52: 0x0023 TLV 0x16[4] = 1, 0x0028 TLV 0x15[4]
  = 1 (the alternating pair of the boot conversation; identity open).
* 23.562-23.671 WDS fd95: Get Profile List 0x002a type 0, Get Profile
  Settings for {0,1} and {0,2}, list type 1, settings {1,0}, then
  **Create Profile 0x0027 type 1** with 0x8f = 0, 0x9a = 0, 0xa1
  "ims", 0xa2 = 02, 0xad = 0, 0xdd[8] = 02 -- qcril's 3GPP2 ims
  profile, re-created a quarter second before the dial -- list type 0
  again, then settings {0,1} {0,2} {0,3} {0,4} {0,5} {0,7}.
* 23.722-23.770 probe client fd85: 0x00a2 {3, 4} mux 2, 0x00af
  TLV 0x01 = **0** (subscription 0), 0x004d = 6, 0x0022, 0x004d = 4,
  0x0022.
* 23.785-23.867 the dial: fd101 0x00a2 mux 2, 0x00af = **0**, 0x004d
  = 4, Start (auth none, 0x19 = 4, 0x35 = 1, 0x39 = 1, 0x3d = 1,
  0x3e[8] = 02...); fd104 the same with 0x00af = **2**, 0x004d = 6,
  0x19 = 6.  Both dial clients bind mux 2; the QOS client fd107
  ({3, 4} mux 2, 0x002d = 2) sits between the binds and the starts.
* 23.890-23.927 after the starts: 0x002d with mask 0x00008210 from
  pid4171/4183 on fd104 and with 0x00080000 from fd101/fd104.

No NAS message appears anywhere in that window.

## The subscription: the SIM answers on QMI subscription 2

Android's own dumpsys names the SIM: `dumpsys isub` reports **Logical
SIM slot 1: subId=1**, defaultSubId 1, defaultDataSubId 1,
activeDataSubId 1, carrier 中国广电 (MCC/MNC 46015).  And the 11:06
dial capture (sendto+recvfrom, QMI payloads) has **every** qcrild WDS
client bind `0x00af` TLV 0x01 = **2** before its first WDS read: the
dialog client fd 85 at 11:06:25.347, and both dial clients fd 101
(.408) and fd 102 (.433), plus fd 110 for the QOS work.  QMI's
subscription id is 1-based here (0 default, 1 primary, 2 secondary),
and value 2 is the state the SIM's traffic runs in.

Until run 103830 this driver's shared client 0x1002 was bound 1 --
the empty slot -- so every Set Default Profile, attach read/force and
0x0093 ever sent hit subscription 1's state, and the SIM's own state
never changed.  What the SIM's subscription actually held at our dial
(run 103830, read on the mis-bound client): default 3GPP profile 6
("qdp_profile_ia", APN cbnet, PDP 3, IMCN 0), profile 7 unreadable
(error 81), LTE attach attached 1, ip type 1 (**IPv6**), APN "ims".
That is the modem's own boot state -- an IMS-only attach -- while
Android's dial runs with an IPv6 PDN already CONNECTED.

### Android's dial-time state and its pre-dial writes

* fd 85: Bind Mux {3, 4} mux 2, Bind Subscription 2, Set IP Family 6,
  0x0022 → **status 2 (connected)**; then Set IP Family 4, 0x0022 →
  status 1.  The family-4 leg is refused, the family-6 leg is the
  session Android has up.
* fd 101 (v4 leg): bind 2, family 4, Start with `0x16=00 0x19=04
  0x35=01 0x39=01 0x3d=01 0x3e[8]=02` → result 1, error 14, verbose
  `0x11[4]=02 00 d0 00` (type 2, reason **208**
  INTERNAL_PDN_IPV4_CALL_DISALLOWED) -- the benign "no IPv4 in this
  PDN" answer.
* fd 102 (v6 leg): family 6, the same body with `0x19=06` → **result
  0, packet data handle 0xf9abfc40**.
* The pre-dial WDS writes are minimal: a Stop for a stale handle, Get
  Profile List, Delete Profile type 1 id 103, Create Profile type 1
  with qcril's TLVs `{0x01=01, 0x8f=00, 0x9a=00, 0xa1="ims",
  0xa2=02, 0xad=00, 0xdd[8]=02}` → index 104, the per-client binds,
  and the QOS binds (`0x002b` {3,4} mux 2, `0x002d` = 2).  **No
  0x0049/0x004a/0x0093/0x0094/0x0085 at dial time.**  The default
  profile 1→7 belongs to qcril's boot (its boot capture ends at 7);
  Set LTE Attach PDN List (0x0093) is never sent at all.

### What the driver does now

`mhi_wds_early` binds 0x1002 to subscription 2 first, reads 0x00b0
back, then replays qcril's boot sets 1 then 7 and reads 0x0049 back
-- before NAS registers, because the attach is built then.  0x0093 is
never sent (the list is only read).  The profile-6 repair and the
forced re-attach are compiled off.  Before the dial, `mhi_wds_ims_up`
reads 0x0049 on subscription 2 and replays 1→7 only if it drifted.
Every fresh client binds subscription 2 and its 0x00b0 read-back is
logged.  A new default-PDN attempt runs before the probe client: a
fresh client 0x100b binds {3, 4} mux 2 and subscription 2, sets
family 6, and Starts "cbnet" (mask 1, qcril body) so the dial has the
connected default PDN Android has; if accepted it is left up, and its
0x0022 is polled; if refused, the verbose reason is logged.

## The Start has two shapes: Android's default-data call (2026-11-23 12:30)

Every capture until now only ever held Android's IMS dial, because Wi-Fi
(SSID "1701") was the active default network and telephony never asks for
the mobile default PDN while it is: `svc data disable/enable` alone
produces no Start, and an airplane-mode cycle re-dials only the IMS PDN
(port 0x35, v4 leg refused 1/14 verbose type 2 reason 208, v6 leg
accepted with handle 0x4289f9e0, then 0x002d IPv6/PCSCF reads).  Turning
Wi-Fi off (`svc wifi disable`, `.scratch/togglecap3.sh`, per-thread
strace with requests *and* responses) made telephony bring cbnet up:

* 35.823-35.963 fd95 surveys the profiles: 1 "qdp_profile_ia" cbnet mask
  0x431, 2 ims mask 2, 3 CMNET mask 0x31, 4 CMWAP mask 0, 5 SOS mask 2,
  7 cbnet mask 0x431.
* 35.870 fd95 Create Profile 0x0027 `0x01=01 0x8f=00 0x9a=00
  0xa1="cbnet" 0xa2=02 0xad=00 0xdd[8]=3104000000000000` -> 0x0167.
* 35.883 fd30 Set Default Profile 0x004a `0x01[3]=000001` -> **1**.
* 36.218/.250 fresh clients fd113/fd116 bind `0x00a2 {3, 4} mux 4`,
  `0x00af` = 2 (subscription 2), `0x004d` = 4 and 6, all result 0.
* 36.339/.344 **Start Network**, both legs:

  `0x14[5]="cbnet" 0x16[1]=00 0x19[1]=04/06 0x35[1]=01 0x39[1]=01
  0x3c[1]=01`

  -- no 0x3d, no 0x3e mask.
* 37.060/.084 both answered `0x02[4]=00000000` with packet data handles
  0x42acfe40 (v4) and 0x428e5520 (v6).  The 0x002d reads that follow
  return the IPv4 address/DNS on the v4 leg and, on the v6 leg,
  240a:42e6:2002:6f05:e9ac:de73:65ac:0138 with DNS 2409:8080:2000:3::1
  and ::2.
* 37.62+ a keeper client (fd122/fd125, {3, 4} mux 4) binds and polls
  0x002d; the data connection client (port 0x36) binds {3, 4} mux 4 and
  0x002d = 2 (36.290-.317).

Against this modem and this SIM the CM/ACL accepts the cbnet data call
for **both** families -- with TLV 0x3c = 1.  The IMS dial's body is the
same frame with 0x3d = 1 (plus the mask 0x3e = 2 on some legs); that is a
different call kind to the modem.  Every Start this driver has ever sent
-- the boot matrix, the per-profile forms, the reference-shaped 31-byte
body and the cbnet APN with mask 1 -- carried the IMS shape, which is why
1042 held for all of them regardless of APN, profile, mux, subscription
or client.

**The driver's new form** (`mhi_wds_start_data_shape`, consumed per
start): the cbnet attempt goes out with 0x3c = 1 in place of 0x3d and no
0x3e; the default profile is set to 1 first (Android's 12:30 state) and
back to 7 after a refusal (the boot capture's dial state).  The first
attempt keeps client 0x100b, mux 2 and family 6 so the body shape is the
only change from the earlier refused start; a refusal retries the same
body on a fresh client 0x100c bound to mux 4 (Android's data mux).

## Run 125117 (12:51): the data shape alone is still refused; the RAT is not the gate (2026-11-23)

Run 125117 sent the data-shape cbnet Start exactly as the 12:30 capture
has it (0x3c = 1, mask 1, family 6, subscription 2), first on the existing
client 0x100b mux 2 and then on the fresh client 0x100c mux 4, with the
default profile set to 1 first.  Both were refused the same way as every
IMS-shaped call: result 1, error 14, verbose reason type 3, 1042.  The
body shape alone does not move the ACL.

Two more captures narrowed what is left:

* 13:32 (`togglecap4`, the whole QMI daemon set): telephony re-created
  its cbnet profile (Create Profile 0x0027 -> index 0x0166), set the
  default profile to 1, probed mux 3 and mux 4 on short-lived clients,
  then the two dial clients bound mux 3 + subscription 2 and their
  data-shape Starts were accepted.
* 14:02 (`ltecap.sh`): Android was forced LTE-only (`cmd phone
  set-allowed-network-types-for-users -s 0 00000001000000000000`; the
  bitmask is binary text -- bit 12 = LTE, bit 19 = NR) and the radio
  reselected to LTE (radio 14) within ~10 s.  The identical dial was
  captured again: Create Profile -> 0x0167, Set Default Profile 1,
  **netmgrd's WDS client (fd 19, subscription 2) sent 0xfff2 twice**,
  mux 3/4 probes, the dial clients on mux 3 + subscription 2, Start
  Network with `0x14[5]="cbnet" 0x16[1]=00 0x19[1]=04 0x35[1]=01
  0x39[1]=01 0x3c[1]=01` -> result 0, packet data handle 0x405d7f61
  (the family-6 leg -> 0x302a338f).

The second point is decisive: **the ACL is not RAT-bound.**  On LTE the
same modem accepts Android's data-shape cbnet Start.  HakiOS camping LTE
(the NAS set-system-selection-preference 0x0033 TLV 0x11 = 0x5c still
ends on LTE) is therefore not the refusal cause, and no NR-only
experiment is warranted.  The dial clients **do** send Set IP Family
0x004d immediately before their Start -- 14:02:23.280 (fd 101, family 4)
and .289 (fd 104, family 6), with netmgrd's own client (fd 19) sending
four more at 23.03-23.14, and the 12:30 clients (fd 113/116) sending 4
and 6 at 36.218/.250 before their 36.339 starts -- *and* the Start body
still carries TLV 0x19.  An earlier reading of this section said they
never send it; that was wrong, and the driver's `skip_family`
suppression (run 132112 onward) rested on it.  The suppression is
removed.  They always bind a mux (3 at 14:02/13:32, 4 at 12:30) before
Start.

## 0xfff2 is netmgrd's WDS route look up (2026-11-23 14:3x)

The vendor message 0xfff2 that netmgrd sends before Android's data dial
is a route look up:

* Host side: `netmgr_qmi_wds_route_look_up` (netmgrd symbol @0x18d514 in
  the `.gnu_debugdata`; the call site at 0x18db60 loads w1 = 0xfff2 and
  calls `qmi_client_send_raw_msg_sync(client, 0xfff2, req, len, resp,
  512, &resp_len, 10000)`).  It is reached from `netmgr_qmi_route_look_up`
  (@0xfcdc8) through the netmgr UNIX-socket command path
  (`netmgr_kif_unix_recv_user_cmd` @0xb6c98): telephony asks netmgrd to
  resolve the route before its dial.
* Request body (captured byte-exact): `0x01[1]=01 0x14[5]="cbnet"
  0x19[1]=04` (a second call with 06) `0x35[1]=01 0x3c[1]=01`.
* Answers (result 0): TLV `0x01[10]` = `00ffff06110000008188` (family 4)
  and `00ffff6d040000008088` (family 6); TLV `0x11[5]` = five zeros.
* Firmware: `qmi_wdsi_route_look_up` over
  `ps_route_look_up_iface_by_policy()` -- the policy/route lookup of the
  PS call manager, the same machinery the CM ACL consults when it
  refuses our Start with verbose 1042.

**The driver now replays it.**  New `mhi_wds_route_look_up()` sends
0xfff2 with exactly the captured body on a fresh client 0x100d bound to
subscription 2, for families 4 and 6, logging result/error and the
10-byte route value; it runs after the default profile is set to 1 and
before either default-PDN Start.  In the same change the mux-2 attempt
is followed by a mux-3 retry (the 14:02 dial's mux) instead of mux 4.
That change also added `mhi_wds_start_skip_family` to suppress Set IP
Family 0x004d for these starts, on the reading that the dial clients
never send it; **that reading was wrong** (see the correction above) and
the flag is removed: both default-PDN attempts send 0x004d for their
family and still carry TLV 0x19, exactly like fd 101/104.

## Run 141259 (14:2x): the route look up matches, the Starts still do not

Kernel built 14:0x with the route-look-up replay; the run returned
cleanly at 14:23.

* The route look up itself succeeds and matches Android's: family 4
  returns `00ffff06110000008188` byte-identical, family 6 returns
  `00ffff6c040000008088` against Android's `00ffff6d040000008088` --
  one byte differs (6c vs 6d), and the policy lookup resolves.
* The default profile is set to 1 before the default-PDN start (result
  0), the mux-2 client's binds are accepted, and the data-shape Start
  (cbnet, family 6) is refused result 1, error 14, verbose CM 1042.
  The mux-3 retry client's binds are accepted and its Start is refused
  identically; the default is set back to 7.
* That run still had `skip_family = 1`, so its cbnet Starts were the
  only ones on this phone that carried no Set IP Family 0x004d.  The
  next kernel restores it and adds the dial's Create Profile.

## Run 163145 (16:31): the full dial parity is refused too -- host actions are not the gate (2026-11-23)

Kernel built 16:27; the run returned to Android cleanly at 16:42.

Every host-side difference from Android's accepted dial window is now
replayed, and the Start is still refused:

* The dial's Create Profile 0x0027 (3GPP2 store, `{type 1, index 0}`,
  APN "cbnet", mask 0x3104) answers **result 0, error 0**; its response
  TLV 0x01 is `01 65`.
* The default profile is set to 1 (result 0), the route look up
  answers result 0 with the same 10-byte routes as run 141259, the
  mux-2 client binds mux 2 and subscription 2 (reads back 2), and
  **Set IP Family 0x004d = 6 answers result 0** -- then the data-shape
  cbnet Start is refused result 1, error 14, verbose CM 1042.
* The fresh mux-3 retry client binds mux 3 + subscription 2, sets
  family 6 (result 0), and its identical Start is refused 1/14 too.
* The four fresh ims clients also ran with 0x004d restored; the
  refusals are unchanged.

With the byte-exact Android frame, the exact client bindings, the
dial's own Create Profile, the default-profile set and the route look
up all in place and accepted, the CM ACL still denies cbnet.  **No
WDS/DSD/DPM action Android sends in the dial window is the gate.**
What remains is state written outside the dial window or not
expressible over WDS -- the modem's selected software MBN (the CMCC
selection experiment has still never run) and the attach the modem
built for itself before any host message.

## The live Android QMI lab (2026-11-23 15:3x-16:2x)

Rather than spend a kernel build per question, the working modem was
queried directly: `.scratch/qrtrprobe.c` is a static aarch64 binary that
opens AF_QIPCRTR sockets and sends raw QMI frames, run as root through
`adb shell su -c`.  Each experiment takes seconds.  WDS answers on node
3 at the port this boot's name service announced (0x36 here; the port
changes per modem boot, so every session starts with `lookup 1`).

Read on Android itself, with its data connected (subscription 2, the
SIM's store):

* Profile 1 (type 0) reads "qdp_profile_ia", APN "cbnet", PDP type 3,
  mask 0x431; profile 7 is identical.
* Profile 6 (type 0): result 1, **error 81** (EXTENDED_INTERNAL) --
  Android's own client cannot read it either.  The store hides it, so
  every earlier "profile 6 repair" was writing a profile the firmware
  does not serve through 0x002b.
* Get LTE Attach Parameters (0x0085): result 1, **error 74**
  (INFORMATION_UNAVAILABLE) while Android's data is up -- Android camps
  NR SA and has no LTE attach to report.  HakiOS's 0x0085 answer
  ("ims") therefore has no Android counterpart and cannot be read as a
  divergence.
* Get Default Profile Number (0x0049): result 1, error 2.

Mux and call semantics on the live modem:

* The data-shape Start on **mux 2** was refused 79 (POLICY_MISMATCH);
  the identical body on a fresh client bound **mux 3** + subscription 2
  was accepted (packet data handle 0x1689d080).  Mux 3 is the 14:02
  dial's mux.
* A Stop from a second client does not tear that call down, and the
  attach PDN survives it -- the modem's call ownership is per client,
  which is why an external Stop cannot clear the way for a cold Start.

The 0x00f7/0x00f8 pair: Android's fd 24 sends 0x00f7 `0x01[2]=0101`
then 0x00f8 empty four times around its boot/dial window (14:02:22.747-
22.798, and again from fd 48/51 at 24.2), with no call up.  Replayed on
the live modem, both answer result 1, **error 15 (OUT_OF_CALL)** even
with Android's data connected, so this firmware refuses them regardless
of state; they are not a dial-gating input and the driver does not
replay them.

Kernel built 16:27 from these findings: `mhi_wds_start_skip_family` is
gone; the two default-PDN attempts send Set IP Family 0x004d for their
family and keep TLV 0x19 in the body; and the dial's own Create Profile
0x0027 (3GPP2 store, `{type 1, index 0}`, APN "cbnet", mask
0x3104) is replayed on the survey client 0x1002 immediately before the
mux-2 bind, its result and returned index logged.


## The ACL's own strings, and the inputs it reads (2026-11-23 17:1x)

The modem's ACL is ds707's data-session policy check.  `strings` on
`fw-modem-sdx55m/qdsp6sw.mbn` names the deny reasons and the policy
inputs it reads:

* `ds707_data_session_call_allowed check failed with deny reason:
  ACL_DENY_REASON_APN_DISABLED`, `..._MANDATORY_APN_DISABLED`,
  `..._UNPREFERRED_RAT`; everything else maps to
  `ACL_DENY_REASON_POL_NO_MATCH` ("No Specific end reason provided").
  A fourth family of reasons sits in the iface selection:
  `ds707_rt_acl_use_selected_iface: Policy requires V4 but PDN context
  %d does not have a V4 iface. Deny with reason
  V4_DISALLOWED_SUBS_LIMITED` (and the V6 twin), plus `IFACE_IN_USE`
  when `ds707_rt_is_cfg_supported` is FALSE on a NVRUIM pending
  profile.  `ds707_rt_is_cfg_supported: profile compatibility failed`
  and `IFACE_AND_POL_FAMILY_MATCH failed` are its two inputs: the
  profile must be compatible with the policy and must match the
  policy's family.
* APN enablement is checked twice, in the profile and in the UICC card:
  `ds707_data_session_is_apn_enabled_in_profile` and
  `ds707_data_session_is_apn_enabled_in_uicc_card` -- so an
  APN_DISABLED denial can come from the *SIM's* APN control list, not
  only from the profile.
* The 5G policy side is ds_mppm/URSP: `dnn: %s, match_all rule find in
  preconfig policy, call allowed: %d` (nw policy and atcop policy
  variants too), with `/data/ursp/preconfigured_ursp_policy.xml`,
  `/data/ursp/nw_ursp_policy`, `/data/pdn_policy_db.txt` as its files;
  `ds_mppm_print_policy_db_entry` shows the per-APN PDN policy DB
  fields (`default_apn`, `has_data_profile`, `override_mask`,
  `v4_pdn_status`, `v6_pdn_status`, `override_cause_list[DS_MPPM_TECH_5G
  / LTE / ...]`), and `ds_mppm_get_pdn_policy_db_entry: Failed to add
  missing apn_string %s to PDN DB` is the failure path.
* policyman loads `/policyman/carrier_policy.xml`, `restrictions.xml`,
  `segment_loading.xml`, `device_config.xml` and the NR MSIM XMLs per
  subscription, `refresh_on_sim_change="true"` -- all from the mcfg.

The mcfg tree is not a divergence.  The repo's served tree
`fw-modem/image/modem_pr/mcfg` is byte-identical to Android's
`/vendor/firmware_mnt/image/modem_pr/mcfg`: CBN `mcfg_sw.mbn` md5
`64f3af34f224ea88a802412fbc71c861`, CMCC `7d41929b3d8d72aaae03ea0f553091b2`,
267 `mcfg_sw.mbn` on both sides.  The boot TFTP traffic is only the
`modem_pr/so/517_0_0.mbn` read, the two digests (`mcfg_sw/mbn_sw.dig`,
`mcfg_hw/mbn_hw.dig`) and the `/readwrite/mcfg.tmp` copy; the missing
`/readwrite/ota_firewall/ruleset` is ENOENT on Android as well.

## DSD Get System Status, live Android versus HakiOS (2026-11-23 17:0x)

Read on the live Android modem (DSD at node 3 port 0x3d this boot,
`00710024000000`, result 0) while its data call was up on NR SA:

* TLV 0x10, 17 bytes: `01` then sixteen **zero** bytes.
* TLV 0x11, 130 bytes: six entries ("", cbnet, CMNET, CMWAP, ims,
  SOS), each flag 01 and a sixteen-byte **zero** system block.
* TLV 0x12, 32 bytes: zero.  TLV 0x13, 220 bytes: the same six names,
  flag 00, zero systems.  TLV 0x14, 8 bytes:
  `02 00 00 00 00 00 00 00`.

The HakiOS answer (run 163145) carries the same shape but a non-zero
system: TLV 0x10 = `01` + {tech 0, rat 3, mask 0x2000} and every 0x11
entry the same block (byte 4 = 03, byte 9 = 0x20).  The doc's earlier
02:18 capture had {tech 0, RAT 6, mask 0x0000130000000000}, so the
struct follows the phone's state, and netmgrd writes this TLV itself
through DSD 0x0025 (its TLV 0x10 writes are 1 then 0 around every data
setup).  Android reports an all-zero system *while its data call is
live*, so the struct is host-written advice, not a gate the ACL reads.
The rat value 3 is undecoded; nothing else in the DSD answer differs in
a way Android's own traffic contradicts.

## The long probe command line, and the loader that never received it (2026-11-23)

The modem probe has outgrown the boot line Apple's `boot_args` carries.
`BOOT_LINE_LENGTH` was iBoot's 608 in both `xnuloader/loader.c` and
`xnu-src/xnu-8796.121.2/pexpert/pexpert/arm64/boot.h`; the payload's line
(the full `devices/thyme/ram.args` set plus `pcie2_test=1 pcie2_bhi=2
ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=1 pcie2_pdc_cmcc=1` and xl's
three sha arguments) is about 697 characters.  The loader cut the tail
at 607 without failing: `WARNING: the kernel command line is TRUNCATED:
90 characters of the payload's cmdline did not fit in the 607 the kernel
has`, and the line printed ended `pcie2_mhi` for `pcie2_mhi=1`; the
kernel took what it received, so `pcie2_mhi` parsed as 0 and
`pcie2_pdc_cmcc` never arrived -- the driver logged "the CMCC experiment
is off" on every run on which the argument had been added.

Both sides are now `BOOT_LINE_LENGTH 1024` (`loader.c` and the XNU
header, kept equal so the struct layout matches), and a loader that cuts
the line says so with the exact count.  The boot_args page has room: the
struct is about 1.1 KB, and the device tree sits at +0x10000.

A second trap: `xl enter` with `XL_BOOT_IMAGE` set (the thyme scripts
all set it to `devices/thyme/prebuilt/xnu-usb.img`) boots the
**prebuilt** image and never looks at `xnuloader/loader.efi`; that image
is built by `devices/thyme/build-loader.sh` from
`xnuloader/loader-thyme.efi`, which plain `make loader` does not rebuild
(that one builds redwood's `loader.efi`).  The first two attempts at the
CMCC run therefore still used the 22 November `loader-thyme.efi` and its
608-character line even after the source was fixed; the rebuilt image
(sha256 `67d06423...`) carries the 1024-character loader.  A loader and
a kernel built with different `BOOT_LINE_LENGTH` values disagree about
the boot_args layout after `CommandLine` (bootFlags, memSizeActual), so
rebuild both together.

## The CMCC software config under the start matrix (2026-11-23 17:43, run 174033)

The never-run variant of the PDC experiment ran on the rebuilt boot line
(697 of 1023 characters, `pcie2_pdc_cmcc=1` arrived for the first time).
The boot arguments and the PDC survey itself are the same as every run;
what is new is that the arm executed before the dial's start matrix.

* `the active software config is index 0; the CMCC config is index 1;
  the CMCC experiment is armed`
* set selected (0x0023, token 0x5c5c0001) with id
  `8d be 5c 56 c7 2b a5 38 ad 22 e9 f6 78 c7 5f 0a fb bf fd 0a`:
  result 0, error 0; the indication named no active id (the switch is
  reported by the read-back, as on the live phone).
* activate (0x0027, token 0x5c5c0002): result 0, error 0.
* read-back 0x0022 (token 0x5c5c0003): **the active id is 20 bytes:
  `8d be 5c 56 ...` -- the CMCC config**, so the selection really was in
  force for the whole matrix below.

Every start under CMCC was refused exactly as under CBN:

* default PDN, Android's data shape (cbnet, family 6, mux 2):
  result 1, error 14, verbose reason type 3 **1042**.
* the same on mux 3: result 1, error 14, 1042.
* fresh client 0x1005 (reference v4 leg): 1042.
* fresh client 0x1006 (reference v6 leg): 1042.
* fresh clients 0x1008 and 0x1009 with TLV 0x31 = the ims 3GPP
  profile (2): 1042.

Then the modem's own config was selected and activated again
(0x0023/0x0027 with the copied CBN id `f1 aa ad 22 ...`, tokens
0x5d5d0001/2) and the 0x0022 read-back confirmed it; the CMCC
selection is undone in the same boot.

**Conclusion: the selected software MBN does not gate the CM ACL.**
The denial is not a wrong-MBN (CBN) artefact: with the CMCC
configuration selected and activated, `Volte_OpenMkt-Commercial-CMCC`,
the access list still refuses every cbnet start with 1042, across
clients, muxes, IP families and the ims-profile form.  PDC selection is
out as the lever; the surviving host-controllable inputs are the
modem's camped RAT and NAS state at the start (the phone camps LTE,
radio interface 8, in HakiOS), the DSD/URSP advice the host writes, and
the APN-enable state the firmware checks in the profile and the UICC's
EF_ACL.

## What Android itself is camped on, and the voice service it answers (2026-11-23 18:05)

The same phone, same SIM, back on Android after run 174033, settles the
question the denial raised.  `dumpsys telephony.registry`:

* operator 中国广电 (46015, China Broadnet);
* `getRilVoiceRadioTechnology=20(NR_SA)` and
  `getRilDataRadioTechnology=20(NR_SA)` -- **Android is camped on NR SA**,
  not on LTE, and not on NSA;
* the CS domain is HOME with `availableServices=[VOICE,SMS,VIDEO]`, the
  PS domain HOME with `[DATA,MMS]`;
* `mChannelNumber=504990`, which as an NR-ARFCN is 2524.95 MHz (n41).

So 5G SA works on this SIM, at this spot, on this modem: the earlier
guess that SA is simply too weak to camp does not hold, and the 1042
denial is not a network-level "no IMS anywhere" verdict either.  The
HakiOS boot camps LTE while Android camps NR SA with the same firmware
configuration -- the camped RAT is a host input worth changing, which
is what the NR-first experiment below does.

The live QRTR lab also corrected the voice message numbering.  Its
services this boot: NAS 3 at 3:0x31, VOICE 9 at 3:0x30 (instance 2),
IMS 10 at 3:0x32, IMSA 11 at 3:0x34, PDC 0x24 at 3:0x11.  The
`0x001F` sent earlier was never Get All Call Info: in libqmi's
qmi-service-voice.json that message is **0x002F** (`Dial Call` 0x0020
with TLV 0x01 the number and answer TLV 0x10 the call ID, `End Call`
0x0021 with TLV 0x01 the call ID, `All Call Status` indication 0x002E
with TLV 0x01 a u8 count of 7-byte {ID, State, Type, Direction, Mode,
Multipart, ALS} records).  Probe results:

* VOICE 0x0003 (Indication Register, TLV 0x13 = 1) answered
  result 0, error 0 -- the service takes a raw client's request;
* Get All Call Info 0x002F and 0x002E sent as requests were not
  answered by this firmware;
* NAS 0x0034/0x004D/0x0024 sent from the same raw client were not
  answered either, while PDC 0x0022 kept answering -- so silence is
  per-service, not a broken probe path (the driver's own NAS path has
  answered every one of these messages in every run);
* PDC 0x0022 read-back still names the CBN active id
  `f1 aa ad 22 ...`, i.e. the CMCC selection was indeed undone.

## The NR-first experiment and the 10099 call experiment (run 181805)

Two host inputs are armed together for run 181805 (boot line
`pcie2_pdc_cmcc=0 pcie2_nr=1 pcie2_call=10099`):

* **NR-first** (`pcie2_nr=1`): inside `mhi_wds_ims_up`'s NAS stage,
  after the mode preference (0x5C already names NR5G) is read and
  written, Set System Selection Preference (0x0033) goes out again with
  TLV 0x1E Acquisition Order Preference = u8 count 4 then
  {NR5G 0x40, LTE 0x10, UMTS 0x08, GSM 0x04}.  The preference is read
  back (0x0034) and Get System Info (0x004D) follows so the NR5G
  service status TLVs (0x4A/0x4B/0x4E) are on the record.  If the modem
  reselects NR SA, the IMS start below runs from an NR camp -- the
  condition under which Android's IMS PDN exists.
* **10099 dial** (`pcie2_call=10099`): after the milestone's IMS
  transport read and SMS submit, QMI VOICE is used directly:
  Indication Register 0x0003 (TLV 0x13 = 1), Dial Call 0x0020 (TLV 0x01
  "10099"), thirty seconds of All Call Status indications decoded by
  name, then End Call 0x0021 with the call ID.  The VOICE service (0x09)
  is now in the driver's QRTR whitelist.  China Broadnet has no CS
  domain, so the call can only ride IMS; the dial is quoted against the
  IMSA registration state that preceded it, and a refusal is evidence
  too.
* The CMCC selection experiment is off this run (`pcie2_pdc_cmcc=0`):
  run 174033 already showed it does not gate the ACL, and the arm plus
  matrix costs the window.

## Run 182052: the NR-first answer, and why the dial sent "s'"

Both arms ran and both produced a surprise:

* **NR-first is refused.**  Set System Selection Preference (0x0033)
  with TLV 0x1E (count 4, NR5G/LTE/UMTS/GSM) answered result 1,
  error 25 -- `QMI_PROTOCOL_ERROR_DEVICE_UNSUPPORTED`.  The read-back
  (0x0034) TLV 0x1C `05 0c 08 05 04 01` says the modem's acquisition
  order is *already* NR5G, LTE, UMTS, GSM, CDMA1X, and TLV 0x11
  `5c 00` names GSM|UMTS|LTE|NR5G.  So the LTE camp is not an
  acquisition-order problem, and this TLV cannot move it.  Next
  candidate levers: NR5G SA band preference (TLV 0x2F, sequence of
  8 x u64 masks; n41 = bit 41 -> mask0 0x0000020000000000), DSD/URSP
  advice, PDC/MBN config.
* **The dial sent the wrong bytes.**  The dial's TLV 0x01 on the wire
  was `73 27` -- the ASCII string `s'`, not `10099`.  Root cause:
  `PE_parse_boot_argn("pcie2_call", ...)` reads a numeric-looking
  value with `getval()` and writes it with `argnumcpy()` as a *binary*
  number (10099 = 0x2773, little-endian `73 27`).  `bootargs.c` copies
  a string only when the name begins with '_'.  The modem answered
  result 1, error 48 (invalid argument).
* **But a call existed anyway.**  Before the dial answer, the modem
  had sent All Call Status indication 0x002E: call id 1, state 4
  (CC in progress), type 0 (voice), direction 1 (outgoing), mode 6.
  The driver returned without ending it.

Fixes in the driver for the next run: parse `_pcie2_call` (underscore
forces the string copy), name the QMI errors in the log, remember the
call an All Call Status indication created (`voice_indication_call`),
end it when the dial answer is refused, and prefer its ID for End Call
on the success path too.  The NR-first arm is off (`pcie2_nr=0`).

## Run 190153: the dial bytes are right, the voice service still refuses

* **Settings root list** (54 items, from the injected Preferences
  dylib): item 0 PRIMARY_APPLE_ACCOUNT_GROUP, 1 APPLE_ACCOUNT,
  2 WIRELESS_GROUP, 3 Airplane Mode, 4 Wi-Fi, 5 Bluetooth,
  6 NOTIFICATIONS_GROUP_ID, 7 Notifications, 8 Sounds & Haptics,
  9 Focus, 10 Screen Time, 11 GENERAL_GROUP, 12 General,
  13 Control Center, 14 Display & Brightness, 15 Home Screen.  The
  cellular row went in at index 1 -- inside the account group, above
  the account row -- which is where the user saw it.  The row now goes
  after the identifier "Bluetooth" (fallback APPLE_ACCOUNT, else the
  end), which is the connectivity group's place.
* **Status bar bars.**  The log says "signal item 4 ... up 1
  registered 1 bars 4 dbm -100", "-[_UIStatusBarCellularSmallSignalView
  setSignalMode:] asked for signal mode 2" and "4 bars shown (item 4;
  kernel bars 4, -100 dBm, registered 1)", yet the screen drew four
  grey outlines: the aggregator's data and the mode are right, the
  live view never takes the count.  This build walks
  UIApplication.sharedApplication.windows, finds every *SignalView*,
  sets signal mode 2 and setActiveBars:/setNumberOfBars: 4, and says
  each view class's bar/mode method inventory once (evidence for the
  next step if a class names its bars differently).
* **The dial.**  `_pcie2_call=10099` now puts the right bytes on the
  wire (log: "the call experiment is armed for 10099"; the request and
  answer payloads are hex-logged).  The request now also carries
  GobiAPI's Call Type TLV 0x10 = 2 (VoiceOverIP, `_pcie2_calltype`),
  because China Broadnet has no CS domain.  The answer is still result
  1, error 48 (invalid argument), and the modem still emitted All Call
  Status indication id 1, state 4 (CC in progress), type 0 voice,
  direction 1 outgoing, mode 6.  End Call of id 1 answered error 41
  (INVALID_QOS_ID in libqmi's qmi-errors.h).  On a refused answer the
  driver now listens 12 s for the indication call's states and then
  ends it, so a call that progresses despite the answer is seen and no
  call outlives the run.
* IMSA registration, SMS and voice capability still read 0xFFFFFFFF
  (not given), and every IMS PDN start is refused result 1, error 14
  with verbose reason 3/1042 (the ACL).  The voice service is therefore
  not the real gate for a call on this network: IMS registration is,
  and it is refused at the PDN.


## The live Android baseline (2026-11-24, 01:00) and the subscription mismatch

With Android running and its data up (rmnet_data1/2 carrying IPv6 and
IPv4, registered NR SA on 中国广电), the modem's own view was read
through qrtrprobe (`/data/local/tmp`, static aarch64, one fresh QRTR
socket per request -- each socket is a fresh QMI client; the QRTR
header is stripped by Android's socket API and the 7-byte QMI header
follows):

* UIM Get Card Status (0x002f, no TLVs): primary GW application
  **65535**, slot 65535, 2 slots; slot 1 card 2 (ERROR), slot 2 card 1
  (PRESENT), UPIN 0, app 0 type 2 (USIM) state 7 (READY), AID
  `a0 00 00 00 87 10 02 ff 86 ff ff 89 ff ff ff ff`.  Android never
  sends Switch Slot: its working card sits where the modem found it.
* UIM Get Slot Status (0x0047) answered result 0 with physical slot
  2's status.
* Copying HakiOS's own UIM block against the live modem: Switch Slot
  (0x0046, logical 1 -> physical 2) is refused **result 1, error 2
  (NO_MEMORY)** while Android's UIM clients hold the card, so the
  A/B cannot be done on the live modem; it is a HakiOS-only test.
* `dumpsys isub` says the working card is "logical slot 1, subId 1,
  defaultDataSubId 1" -- Android's 0-based index of tray slot 2 --
  and every qcrild WDS client binds **subscription 2**
  (QmiSubscriptionType: 1 primary, 2 secondary) before its first
  read.  So the modem's "secondary" subscription is the card Android
  actually uses, and the wds dial captures' 0x00af = 2 is this
  numbering, not a logical-slot index.
* NAS Bind Subscription (0x00af on the NAS service) answered
  **error 1 MALFORMED_MESSAGE** on the live modem; libqmi's service
  definitions carry 0x00af only in WDS and QoS, so NAS does not take
  that bind at all.
* The QMI error code table (libqmi's qmi-errors.h) corrects earlier
  readings in this document: **1 MALFORMED_MESSAGE, 2 NO_MEMORY,
  3 INTERNAL**, 10 INVALID_PROFILE, 14 CALL_FAILED, 15 OUT_OF_CALL,
  16 NOT_PROVISIONED, 25 DEVICE_UNSUPPORTED, 37 UIM_UNINITIALIZED,
  48 INVALID_ARGUMENT, 70 INVALID_OPERATION, 71 INVALID_QMI_COMMAND,
  74 INFORMATION_UNAVAILABLE, 79 POLICY_MISMATCH, 80
  SIM_FILE_NOT_FOUND, 82 ACCESS_DENIED.

HakiOS's UIM block does the opposite of the working baseline: its
Switch Slot moves the card to logical slot 1 (the status afterwards
reads slot 1 PRESENT, slot 2 ABSENT) and Change Provisioning Session
makes that card application 0 the primary GW one, while every WDS
client -- the shared 0x1002, the IMS dial clients, the fresh matrix --
binds subscription 2 (the secondary: where the card was before the
switch).  Android's working combination is "card left in its slot,
primary GW application 65535, WDS on subscription 2".  That makes the
subscription/card mismatch the leading explanation of the CM ACL
reason 1042 that refuses every Start Network:

* ds707's access-list inputs include
  `ds707_data_session_is_apn_enabled_in_profile` and
  `..._in_uicc_card` (EF_ACL 6f57) per subscription; a session bound
  to the subscription whose slot now holds no card can fail the
  family/interface/policy match (ACL_DENY_REASON_POL_NO_MATCH), which
  the modem reports upward as CM_ACL_FAILURE 1042.

The experiment (kernel `ba8d8ea960243213`, booted with the same
run4.sh argument set): when every fresh client's Start is refused, one
fresh client is re-bound to subscription 1, the bind is read back, and
the reference 31-byte Start is sent under that subscription -- the one
loop position that never ran, because the fresh-client matrix returned
before the later subscription probes.  An accepted Start there means
the ACL was the subscription binding, and the fix is to bind the WDS
clients to the subscription the card is actually on after the UIM
block's switch.

## Run 021620: the subscription probe -- the ACL is per subscription

Kernel `ba8d8ea96024`.  The UIM block took the card to logical slot 1
again (the dig-window switch, then `THE PROVISIONING TOOK: primary GW
application 0`), the modem registered (`radio 8`, 中国广电, 4 bars),
and then, after every fresh client's reference Start had been refused
with CM 1042, the new probe re-bound fresh client `0x1005` from
subscription 2 to **subscription 1** and sent the reference 31-byte
Start under it:

| step | result |
|------|--------|
| Bind Subscription 0x00af, TLV 1 = 1 | result 0, error 0 |
| Get Bind Subscription 0x00b0 | reads `subscription 1 (primary)` |
| Start Network 0x0020 (reference body, family 4, ims mask 2) | result 1, error 14, **verbose reason type 2 (internal) 208** |

That is not the CM ACL 1042.  The verdict splits by subscription:

* Subscription 2 (secondary -- where the card sat before the UIM
  block's switch, and where every WDS client of this boot binds):
  refused at the ACL, CM 1042.
* Subscription 1 (primary -- where the switch moved the card, and the
  one the provisioning bound the USIM to): the ACL passes, and the
  Start fails later inside the data service (verbose type 2, internal
  reason 208) -- no card/attach context for that subscription's data
  path, since the modem's initial attach and default data subscription
  are the secondary's.

So the CM 1042 is a per-subscription ACL verdict, not a property of
the APN, the body or the client.  Live Android never switches: card on
slot 2, primary GW application 65535, every qcrild client on
subscription 2, dials accepted.  The next run restores exactly that
combination -- `pcie2_uiswitch=0` leaves the card where the modem found
it (the default keeps the phone #1 remedy) with every client still on
subscription 2 -- and tests whether the Start is then accepted.

## Run 022954: parity with the working phone (pcie2_uiswitch=0) -- the switch is load-bearing

Kernel `545d553a4e90`, booted with `XL_EXTRA_ARGS=pcie2_uiswitch=0`.
The gate worked: the UIM block logged "the switch is off
(pcie2_uiswitch=0): the card stays in slot 2, Android's posture", the
card status read slot 1 card 2 (ERROR), slot 2 card 1 (PRESENT), app 0
type 2 (USIM) state 7 (READY), and "after the switch (0) and the
provisioning (0): primary GW application 65535" -- Android's exact
posture -- with every WDS client left on subscription 2.

Registration did not follow, though: NAS Initiate Network Register
(0x0022, the 28- and 32-byte bodies) was refused **result 1, error 37
(UIM_UNINITIALIZED)** on every attempt through 02:34, the serving
system stayed unregistered, and the attach read said result 1, error
74 (INFORMATION_UNAVAILABLE).  The IMS stage stopped there: no
default-PDN Start, no fresh-client Start, zero Starts in the whole
run.

So the two postures are each insufficient on their own:

| posture | registration | Start |
|---------|--------------|-------|
| parity (card in slot 2, clients sub 2) | refused 37 UIM_UNINITIALIZED | never attempted |
| switch (card logical slot 1, clients sub 2) | registered, LTE camp | refused CM 1042, per-subscription ACL |
| switch + clients on sub 1 (the next run) | expected registered | to be measured |

NAS appears to tie its UIM state to subscription 1 while the card sits
in logical slot 2; the switch is what gives NAS a ready card.  The
next run keeps that switch (default on) and moves every WDS client to
the card's subscription through `mhi_wds_card_sub` (set to 1 by the
successful Change Provisioning Session): the early "after the card
work" and "before the registration" steering, the IMS dial's
default-PDN and retry clients, the route look-up, the probe, the fresh
matrix and the reference Start all bind it, so the whole data posture
follows the card the UIM block created.  Kernel `46e5cc88b436aead`.
## Run 024409 (02:44): the card's subscription carried the data PDN, not the IMS dial

Posture: the switch is on, Change Provisioning Session took on the
first attempt (result 0), and `mhi_wds_card_sub` became 1, so every
WDS client binds subscription 1 from here on.  Kernel
`46e5cc88b436`, image `6ef0e8eb7b37`, loader `66d0331b2347`.

The early binds read back "primary" on subscription 1, NAS Initiate
Network Register said result 0, error 0 (registered), and the whole
CM 1042 ACL wall of the earlier runs is gone: the default-PDN client
bound mux 2 on {type 3, interface 4} and the data-shape Start (APN
cbnet, family 6, auth none, no mask) was

    IPCR WDS start the default PDN (Android's data shape, cbnet,
    family 6, mux 2) says result 0, error 0
    THE DEFAULT (cbnet) PDN WAS ACCEPTED (packet data handle
    429517712); the ims dial below runs with it up
    the default-PDN client's packet service says 2 (connected)

That is the first accepted Packet Data Network of the whole port.

The IMS dial, though, was refused on every leg, and the verdicts
split by family exactly the way a busy mux predicts:

| client | form | verdict |
|--------|------|---------|
| 0x1005 | reference v4 leg, mux 2 | result 1, error 14, verbose reason type 2 208 |
| 0x1006 | reference v6 leg, mux 2 | result 1, error 79 POLICY_MISMATCH |
| 0x1008 | TLV 0x31 profile 2, family 6 | result 1, error 79 |
| 0x1009 | TLV 0x31 profile 2, family 4 | result 1, error 14, verbose 2 208 |
| 0x1005 | re-bound sub 1, reference v4 | result 1, error 14, verbose 2 208 |

The clients' own packet service reads give the reason: the refused
family-6 clients read **2 (connected)** and the family-4 ones 1
(disconnected).  The fresh clients bound the same mux 2 the accepted
cbnet PDN already occupies, so the modem reports the data PDN's
session on them and refuses a second Start -- the v6 refusals are the
APN-mask policy against the mux's existing APN, the v4 ones "call
failed" with the internal 208.  The reference phone dials its ims
PDN on mux 2 while its data PDN lives on mux 3/4 (the 14:02 and
13:32 captures); run 024409 let the data Start take mux 2 first
because the data shape was accepted there, which is exactly the slot
the ims dial needs.

Smaller asymmetries on subscription 1, recorded, not blocking: Set
Default Profile 7 fails error 81 (EXTENDED_INTERNAL) where
subscription 2 took it, so the default profile stays 1; the attach
PDN list on sub 1 names profile 6 first (sub 2 named 1, cbnet).
Profile survey unchanged: 2 = ims, mask 0x2; 3 CMNET mask 0x31;
5 SOS mask 0x200; 7 unreadable (81).

Downstream: IMSA bind result 0 but registration / SMS / voice all
"not given", so the SMS-to-self milestone submitted over NAS/WMS and
failed (attempt 1: error 56 WMS_MESSAGE_DELIVERY_FAILURE; attempts 2
and 3 no answer; the message is not sent).  The automatic 10099 dial
ran with call type 2 (VoIP): the indication register took (result
0), the modem raised a call-status indication for call id 1 (state 4
"CC in progress", mode 6, the calling-number TLV 10099) while the
Originate waited, and then refused the Originate itself **result 1,
error 48 (INVALID_ARGUMENT)** with no call id; the end-call of id 1
answered error 41 (invalid QoS id) -- the call never existed, and
the modem does not accept a VoIP Originate while the IMS PDN is down
and IMS is unregistered.

Next: the default (cbnet) data PDN binds mux 3 (the 14:02 capture's
data mux), mux 4 on a refusal, never mux 2, so the ims dial keeps
the reference phone's mux 2 to itself; the fresh matrix is unchanged.

## 06:17 and 06:29 Android reference captures: the address TLV, and Android's own 10099 dial

Two strace captures were taken on the reference phone (rooted Mi 10S,
China Broadnet 46015, IMS registered and dialling) with the IMS/QMI
processes followed through `sendto/recvfrom`:

- `imscap2` (airplane-mode cycle): org.codeaurora.ims, imsqmidaemon,
  imsdatadaemon, qcrild, qmi-proxy, qmipriod, dpmQmiMgr.  The IMS data
  service's socket (fd 5) talks to the modem's IMS stack at 3:0x3e and
  the exchange matches the bytes this driver replays, TLV for TLV.
- `imscap4`: `am start -a android.intent.action.CALL -d tel:10099`
  while the same set was traced; the call went ACTIVE over IMS.

### The IMSDCM exchange, from the AP side

The modem (3:0x3e) writes message 0x21 with TLV 0x01[1]=0 and
0x10[4]=2; the daemon answers result 0, TLV 0x10[1]=0xfa, 0x11[4]=2,
and 140 ms later sends the unsolicited 0x20 indication:

```
04 02 00 20 00 19 00  02 04 00 00 00 0d 00  01 01 00 15
10 04 00 64 00 00 00  12 04 00 02 00 00 00
```

The 0x02 TLV on the wire is **00 00 0d 00** -- result 0, error 13.  The
driver's `ready` word was 0x00000d00, i.e. the 13 landed in the result
half (0x0d00); it is 0x000d0000 now, and the 0x10 TLV is 100, not 0x65
(0x65 is only what the *answer* below echoes).

Ten seconds later the stack asks message 0x20 (transaction 9):

```
01 14 00 03 69 6d 73 00 00 00 00 02 00 00 00 01 00 00 00 02 00 ff ff
10 04 00 65 00 00 00  11 04 00 01 00 00 00  12 04 00 02 00 00 00
13 04 00 02 00 00 00
```

and the daemon answers result 0, 0x10[1]=0x15, **0x11[4]=0x65** --
the request's own TLV 0x10 echoed back -- and 0x12[4]=2, then sends the
address indication:

```
04 03 00 20 00 46 00  02 04 00 00 00 00 00  01 01 00 15
10 04 00 64 00 00 00  11 2a 00 01 00 00 00 25
"240a:42e6:810:ebb:4b41:bd55:b67e:6bd4"  12 04 00 02 00 00 00
```

So the shape this driver already sends is right; the 0x11 echo and the
ready word are now aligned.

### WDS Get Current Settings TLV 0x26: address first, prefix last

qcrild's IMS client asked Get Current Settings (0x2d) with mask
0x400 and the answer carried:

```
26 11 00 24 0a 42 e6 08 10 0e bb 60 e3 56 ce 48 e0 43 58 40
27 10 00 24 09 80 80 20 00 00 03 00 00 00 00 00 00 00 01
28 10 00 24 09 80 80 20 00 00 03 00 00 00 00 00 00 00 02
2b 01 00 06
```

TLV 0x26 is 17 bytes: the sixteen address bytes **first**
(`240a:42e6:0810:0ebb:60e3:56ce:48e0:4358`) and the prefix length
`0x40` = /64 **last**.  The driver read it the other way round --
`flen = see[0]` (0x24 = /36) and the address from see[1..16] -- so
every run so far handed the modem's IMS stack a text address starting
`0a42:...`, outside 2000::/3, in run 054720
`0a42:ea20:128f:27b1:d2d4:0c60:5e32:a440/36`.  That is the address the
stack would bind its SIP sockets to; no registration can succeed with
it, and IMSA's 0x20/0x21 polls have answered result 1 error 70 in
every run ever logged.  The parse now takes see[0..15] as the address
and see[16] as the prefix (defaulting to 64 for a 16-byte TLV).

### Android's dial of 10099 (voice service 3:0x30, msg 0x20, txn 27)

```
01 05 00 31 30 30 39 39        number "10099"
10 01 00 00                    call type 0 (voice automatic)
16 04 00 01 00 00 00           service type 1
18 08 00 03 00 00 00 00 00 00 00
19 08 00 00 00 00 00 00 00 00 00
22 01 00 00
23 04 00 00 00 00 00
```

answer: result 0, TLV 0x10[1]=1 (call id), TLV 0x15[1]=5, and the All
Call Status indications 0x2e follow with the same state-4/mode-6 entry
this driver logged from its own refused dial.  The driver's dial used
call type 2 (VoiceOverIP) and only the number and call-type TLVs, and
the modem refused it result 1 error 48; it now sends Android's set with
call type 0 by default (`_pcie2_calltype` still overrides), and
run4.sh passes 0.

### Runs 030016 to 054720, 2026-11-24: the same wall in every log

Five runs overnight and this morning (03:00, 05:06, 05:20, 05:34, 05:47)
are byte-for-byte alike in their outcomes, so they are recorded once:

| run | address handed to the stack | IMSA 0x20/0x21 | policy 0x48 | settings 0x8f/0x90 | dial 10099 |
|-----|------------------------------|----------------|-------------|--------------------|------------|
| 030016 | `0a42:ea20:1275:9530:9a27:2910:b11f:0a40/36` (twice) | result 1, error 70 | result 1, error 70 | result 1, error 70 | result 1, error 48, no call id |
| 050622 | `0a42:ea20:125b:30cd:f328:ba4b:0a27:e640/36` | result 1, error 70 | result 1, error 70 | result 1, error 70 | result 1, error 48, no call id |
| 052027 | `0a42:e620:124e:7cc8:02a7:d432:af16:da40/36` | result 1, error 70 | result 1, error 70 | result 1, error 70 | result 1, error 48, no call id |
| 053422 | `0a42:ea20:1240:2325:b9ef:82a3:2112:5f40/36` | result 1, error 70 | result 1, error 70 | result 1, error 70 | result 1, error 48, no call id |
| 054720 | `0a42:ea20:128f:27b1:d2d4:0c60:5e32:a440/36` | result 1, error 70 | result 1, error 70 | result 1, error 70 | result 1, error 48, no call id |

The `/36` and the leading `0a42` are the reversed TLV 0x26 parse
described above, not the network's assignment; the reference phone holds
`240a:42e6:2012:4626:…/64` on the same card at the same time.

Run 053422 is the clearest picture of the IMS data service this driver
replays.  The modem's stack asked 0x20 (transaction 7) while other
traffic waited; the driver answered "no IMS PDN address yet"; it asked
0x33 (transaction 8) and got SUB_DESTROY_INSTANCE success; it asked
0x20 again (transaction 9) and got the same "not yet"; and once the
family-6 leg of the ims PDN came up, the address indication went out
(78 bytes, to 3:0x39).  050622 shows the same indication at 77 bytes
(IPv4-mapped text one group shorter).  030016 and 054720 carry only
the indication, no asks -- early-boot console loss or a stack that had
already stopped asking by then.  In every run the modem accepted the
indication and still answered every IMSA poll with error 70, which is
what a SIP stack that could not bind its IMS address would do.

Where the IMSA checks sit in each log, for future grepping:

- registration status 0x20: first request transaction 199, first
  answer `result 1, error 70` at 030016:8725, 050622:9572, 052027:9570,
  053422:9914, 054720:9528.
- services status 0x21: answer `result 1, error 70` at 030016:10064,
  050622:10579, 052027:10693, 053422:11136, 054720:10709.
- dial: `THE DIAL TO 10099 says result 1, error 48 (invalid argument)`
  at 030016:13949, 050622:13585, 052027:13418, 053422:13256, 054720:13480.
- every log has 19 `error 70` lines (the five checks above plus their
  repeats) and 2 `error 48` lines.

The next run is the first with the corrected TLV 0x26 order, the
Android-matching ready indication and the full Android dial TLV set;
these five logs are the baseline it has to beat.

### Run 063523, 2026-11-24: the address lands, the dial still meets error 48

Run 063523 was the first with all three IMSDCM fixes (the TLV 0x26
address order, the Android-matching ready indication and the full
Android dial TLV set).  Two things were settled:

- the fix to TLV 0x26 is right.  `the family-6 leg's started session
  carries ipv6 240a:42e6:0810:027c:c1a1:1e01:7fca:643f/64` with the two
  `2409:8080:2000:0003::1` DNS servers appears twice (lines 9092 and
  9146 of `logs/063523-run.log`), and the IMS data service (port 0x3005)
  is handed that address in its 77-byte message 0x20 both times
  (`the IMS data service (port 0x3005) sends node 3 port 0x39 message
  0x20 type 4, 77 bytes: sent`, lines 9100 and 9154).  The address is a
  global /64 from the network, not the reversed /36 the earlier logs
  showed.
- the dial TLV fix alone does not make the call.  The request now is
  byte-identical to Android's (`00 96 00 20 00 34 00 01 05 00 31 30 30
  39 39 10 01 00 00 16 04 00 01 00 00 00 18 08 00 03 ...`, logged as
  `the dial request: 59 bytes`), and the answer is still
  `message 0x20, result 1, error 48` with no call id (line 14526,
  `THE DIAL TO 10099 says result 1, error 48 (invalid argument)`).

The interesting part is what the modem did while the dial waited: it
sent its own All Call Status indication 0x2e (60 bytes) with call id 1,
state 4 (CC in progress), type 0 (voice), direction 1, mode 6, and the
number 10099 in TLV 0x10.  So the modem's call engine accepted the call
and began to set it up; the QMI response then refused it as an invalid
argument, and the End Call of id 1 thirty seconds later answered error
41 (`THE END CALL OF 1 (from the indication) says result 1, error 41`).
The call never reached alerting.

Android's own dial of the same number (imscap4, 06:29, peer 3:0x30)
settles what "working" looks like: the request is the same 52 payload
bytes, the answer is `result 0, error 0` with TLV 0x10 = call id 1 and
TLV 0x15 = 05, and the call-status indications then run
state 4 (CC in progress) -> state 5 (alerting, 06:29:25.230) with
**type 2 (VOICE_IP, VoLTE)**, until the capture's own End Call twelve
seconds later.  HakiOS's indication never leaves state 4 and its call
type is 0 (plain voice), which is what a modem does when the request is
marked for IMS but no IMS registration exists to carry it.

### What Android's vendor IMS service does (imscap2, 06:17)

The port Android's IMS client talks to (peer 3:0x3b in this boot) speaks
no libqmi message ids: its traffic is 0x3c (once, TLV 0x01 = 01 01),
0x3d (three times: an array of per-service values in TLV 0x10/0x11),
0x48 (ten times, len 0) and 0x4a (seventeen times, len 0).  The answers:

- 0x4a -> result 0, TLV 0x10 = 00;
- 0x48 -> result 0, TLV 0x10 = 01, TLV 0x11 = 00 01;
- 0x3c -> result 0;
- 0x3d -> result 0 once, then result 1 error 25 twice.

This is the vendor IMS control surface (the same one that answers
HakiOS's 0x48 with error 70 when it is asked too early).  It also
explains why Android never shows 0x8f/0x90 traffic: the vendor client
uses 0x47-0x4a where libqmi documents 0x8f/0x90.

Correction to the earlier note (around line 4110 of this file): libqmi
1.36's `data/qmi-service-ims.json` carries **only** Get IMS Services
Enabled Setting 0x0090 (output TLVs 0x11 voice, 0x12 video telephony,
0x15 voice over Wi-Fi, 0x18 registration, 0x19 UT, 0x1a SMS, 0x1c USSD)
and Bind 0x0098 (input TLV 0x01, guint32 binding).  It has no Set
0x008f and no Policy Manager 0x0047-0x0049; those exist only as vendor
messages in the firmware, and Android's captures above are the only
description of them this project has.

### The next run (in flight at the time of writing)

The kernel now (a) sends the IMS settings twice -- once where they
always went and once after `mhi_wds_ims_up` succeeds -- (b) tries the
Set 0x8f with the field ids the Get 0x90 names (0x11/0x12/0x15/0x18/
0x1a/0x1c) and falls back to the old 0x10/0x18/0x1a shape if that is
refused, (c) asks the IMS service for its supported messages (0x001e)
and logs whether 0x48/0x8f/0x90/0x98 are among them, (d) binds the IMS
client once per boot, and (e) asks IMSP (service 0x1f, message 0x24)
for the modem's enabler state before and after the PDN.  The log lines
to read are `THE SET WITH THE GET'S FIELD IDS SAYS`, `THE OLD THREE-FLAG
SHAPE SAYS`, `the IMS service lists`, `did not report its supported
messages`, and `the IMSP enabler state says`.

### Run 065457, 2026-11-24: the IMS service's own list, and the dial still refused

Run 065457 asked the IMS settings service (0x12) three new questions and
got three clean answers.  All line numbers below are
`logs/065457-run.log`, which holds the whole boot (the `.live.N`
snapshots were deleted once this file was complete).

- **Get Supported Messages (0x001e) is answered result 0** (lines
  5804-5810, and again 8728+ after the PDN).  The answer's TLV 0x10 is
  24 bytes: a count of 0x0016 = 22 and 22 id bytes.  The driver's check
  says none of policy 0x48 / set 0x8f / get 0x90 / bind 0x98 is among
  them (`the IMS service lists 22 supported messages: ... 0, 0, 0, 0`,
  line 5810).  The raw dump is cut by the console: every log line is
  truncated at about 190 characters (`... 03 40 e` running straight
  into the next record, and the frame dump's own ` ...` marker lost),
  so only the first id bytes were visible.  This run's kernel logs the
  whole list eleven bytes to a line instead.
- **Set IMS Services Enabled (0x008f) with the Get's field ids**
  (0x11 voice, 0x12 video, 0x15 VoWiFi, 0x18 registration, 0x1a SMS,
  0x1c USSD) is refused **result 1, error 1 -- malformed message**
  (lines 5826 and 8748), not InvalidOperation: the message id is known
  and the TLV shape is what the firmware will not parse.  The old
  three-flag shape (0x10/0x18/0x1a) is refused error 70 both times
  (5834, 8756).
- **The policy read (0x0048, no payload) is refused error 70 both
  before and after the IMS PDN** (5818, 8740) -- the same state Android
  is answered result 0 from, so the difference is not timing alone.
- **IMSP (service 0x1f) is never announced** -- `the modem has not
  announced IMSP` at 5843 and 8765, and no NEW_SERVER for 0x1f in the
  boot's name-service table.
- Get Supported Messages is also refused by WDS in this boot (`WDS did
  not report its supported messages`, 5875), so a 0x001e refusal is not
  special to the IMS service.

The IMS PDN itself stays healthy: the family-6 leg's started session
carries `240a:42e6:0810:56fc:047b:daff:52b6:8cba/64` with the two
`2409:8080:2000:0003::1` DNS servers (lines 8616 and 8670), and the IMS
data service (port 0x3005) is handed that address in its 77-byte
message 0x20 twice (8624, 8678).  The IMSA registration reads answer
nothing at all -- every round says `registration is 4294967295 (not
given), SMS ... not given` (8788 through 10171).

The dial is unchanged from run 063523: the 59-byte request (13396) is
byte-identical to Android's, the modem sends its own All Call Status
indication for call id 1, state 4 (CC in progress), type 0 (plain
voice), number 10099 (13408-13410), and the QMI answer is **result 1,
error 48** (13416); the End Call of id 1 later answers error 41
(13535).  The call type 0 in the indication is the tell: the modem
falls back to circuit-switched voice, and this network gives it no CS
domain -- Android's own dial of the same number on the same boot
reaches alerting with type 2 (VOICE_IP) precisely because its IMS
registration exists.

### What Android's client actually does with the IMS service (imscap2, 06:17)

The vendor client (one fd, peer port 3:0x3b in that boot) never sends
0x8f/0x90.  Its whole conversation is 0x4a, 0x48, 0x3d and 0x3c, in
this order:

- **0x4a (no payload), polled about once a second.**  Early answers
  carry TLV 0x10 = 00; at 06:17:33.955 the same read answers TLV 0x10 =
  **04** (`... 10 01 00 04`).  The state word is one byte.
- **0x48 (no payload) right after the state turns 4** (06:17:33.970):
  result 0 with **TLV 0x10 = 01 and TLV 0x11 = 00 01**, repeated on
  every later poll.
- **0x3d** (06:17:34.023): TLV 0x01 = `01` and a 37-byte TLV 0x10,
  `07 00 00 11 02 11 01 03 11 03 11 00 04 11 04 11 01 12 11 1b 11 01
  1c 11 1e 11 00 1f 11 28 11 01 29 11 2b 11 00` -- result 0.  A count
  of 7 and seven five-byte entries; each entry reads as two little-
  endian 16-bit values whose high byte is 0x11 (0x1100, 0x1102, ...)
  and a flag byte (01, 00, 01, 01, 00, 01, 00).
- **0x3c** (06:17:36.336): TLV 0x01 = `01 01` -- result 0.
- The other 0x3d shape (TLV 0x01 = `00`, TLV 0x11 = 27 bytes `05 00 00
  10 01 00 01 01 10 01 00 01 02 10 01 00 01 03 10 01 00 01 04 10 01
  00 00`) is refused **result 1, error 25** twice (06:17:37.966 and
  38.489) and is not part of the working sequence.

The same port also pushes unsolicited 0x49/0x4b/0x5d indications while
the state settles (0x4b payloads `01 01 00 00` then `01 01 00 04`; 0x49
`01 01 00 01 10 02 00 00 01`); the 0x4b value tracks the 0x4a state
word.  A separate client (fd 37) sends 0x4a to the same port and gets
error 52 (DeviceNotReady), and the identical read to port 0x36 is
answered result 0 -- 0x4a exists on more than one service, so the
sequence above is identified by the client that also sends 0x48/0x3d/
0x3c, not by 0x4a alone.

### The next run (in flight at the time of writing)

The kernel now replays that sequence on its IMS client (port 0x1003)
after the bind: it polls 0x4a up to five times, 2 s apart, logs the
state each round, then reads 0x48 and logs its TLV 0x10/0x11 flags, and
only when the 0x4a state is 4 writes the captured 0x3d array and 0x3c
`01 01`.  The log lines to read are `the vendor state read (0x4a,
round`, `the policy read's vendor flags`, `the vendor services write
(0x3d)`, `the vendor command (0x3c)`, and `the vendor configuration is
withheld`; the supported list is logged in two lines as `the supported
list, id bytes 0-10` / `11-21`.  If the 0x4a state stays 0, the modem's
IMS stack never becomes ready and the missing piece is on the AP side
of the IMS PDN bring-up, not in this configuration sequence.

### Run 071041: the sequence is refused, and the dial is not the problem

The run reached all of it and the modem refused every message: the 0x4a
state read was `result 1, error 3` (internal) in all five rounds, both
before and after the accepted profile-2 IPv6 IMS PDN; 0x48 answered
`result 1, error 70` (InvalidOperation) -- so `the vendor configuration
is withheld: the 0x4a state is 4294967295, not 4` -- and IMSA's
registration (0x0020) and services (0x0021) reads were error 70 on
every attempt although its bind (0x0033) answered 0.

The same run dialled 10099 and was refused `result 1, error 48`
(invalid argument) -- while the modem's own All Call Status indication
had already reported call id 1, state 4 (CC in progress), type 0
(voice), direction 1, mode 6 with the number 10099, byte for byte what
Android's dial produced in capture `imscap4` at 06:29:24.  Android's
request -- the same 52-byte payload this driver sends, TLV 0x01 the
number plus the vendor TLVs 0x10/0x16/0x18/0x19/0x22/0x23 -- was
answered result 0 (call id 1) and progressed 4 -> 5 (alerting) -> 3
(conversation) -> 8/9 (end).  The dial therefore is correct; the refusal
is the state of the modem's IMS/voice stack, which has no usable
registration to place the call on.

Two corrections came out of the run's IMS section:

- Get Supported Messages (0x001e) TLV 0x10 is **not an id list but a
  bitmap**: a u16 byte count (22 here), then that many bitmap bytes,
  bit N of byte N/8 naming message id N.  Service 0x12's bytes
  `00 00 00 c0 00 04 00 00 00 03 40 e0 f8 ...` carry 0x1e/0x1f in byte
  3, 0x48/0x49 in byte 9 and 0x8f/0x90/0x98 further on -- so the
  firmware **does** implement the policy read, the services set/get and
  the bind; the run's "policy 0x48 0, set 0x8f 0" lines were the parse's
  fault.  0x3c, 0x3d and 0x4a are genuinely absent from service 0x12's
  bitmap.
- WDS's 0x4a is Set Default Profile Number (profile 2 is the IMS
  profile), not a state read; the Android capture's fd-37 0x4a to
  3:0x36 is that WDS message, and its 0x4a with no payload is the one
  refused error 52.

The vendor client's peer in the Android captures is **3:0x3b**, which
need not be service 0x12's port (the QRTR frame names no QMI service
id): our byte-identical 0x4a/0x48 to service 0x12's announced port are
refused while Android's are answered result 0 from that port.  The next
kernel therefore surveys every announcement with its instance
(`mhi_qrtr_all`, filled by `mhi_qrtr_learn`) and, when the 0x4a read on
the 0x12 entry fails, probes 0x4a (no payload) against every announced
service on the modem's node -- one client port each, transaction
0x1300+i, answered by the pump's `mhi_ims_probe_note` -- keeps the
first service that answers result 0 with a state, and runs the 0x48 /
0x4a-poll / 0x3d / 0x3c sequence at that port.  The probe's answer
line to read is `the vendor probe: service 0x... (instance ...) at
... says result ..., error ..., state ...` followed by `the vendor
state service is ...`.


### Run 074446: the probe answers are there, the matcher was blind

The survey learned 49 distinct {service, instance} announcements; the
probe asked 32 of them (the cap) and 26 answered message 0x4a inside
the 4 s pump.  No answer line and no winner appeared because
`mhi_qmi_open` pinned the transaction's high byte to `0x00`: the probe
asked with transactions 0x1300+n and the modem echoed n, so
`mhi_ims_probe_note`'s `txn >= 0x1300` test never held.  The client
ports tell which service each answer came from regardless:

| client port | answer from | result |
|---|---|---|
| 0x1205..0x121e | 3:0x12 .. 3:0x3a | result 1, error 0x34 (52) |
| 0x1218 (again) | 3:0x34 | result 0 (WDS 0x4a, the default profile) |
| **0x121f** | **3:0x3b** | **result 1, error 0x34** |

The 22-byte supported bitmap of service 0x12 (3:0x49) parses as
`0x3c 0, 0x3d 0, 0x48 1, 0x4a 0` -- the firmware has the policy read
but service 0x12 has neither the vendor state (0x4a) nor the 0x3c/0x3d
writes, so 3:0x49 was never the vendor peer.

The decisive observation is in the Android capture `imscap2` of the
same firmware minutes before: the long-lived settings client fd=60
asks 3:0x3b for 0x4a with **no payload** and gets result 0, TLV 0x10 =
state (00 while the modem's IMS stack comes up, 04 once ready), then
0x48 -> result 0 (TLV 0x10 = 01, TLV 0x11 = 00 01), 0x3d -> result 0,
0x3c -> result 0.  Another Android client, fd=37, sends the
byte-identical bare 0x4a to the same 3:0x3b and is refused **result 1,
error 0x34** -- exactly what our probe got.  The difference is
introduction: fd=60's socket went through the name service, fd=37's
(and our probe's) went straight at the port.  The modem's QMI-over-QRTR
services only serve clients the name service has introduced
(`net/qrtr/ns.c` forwards NEW_LOOKUP to the service, naming the
client's node and port).

The next kernel therefore registers every probe client with a
NEW_LOOKUP (cmd 10, header type 10, the frame `mhi_qrtr_tftp_lookup`
uses) before its 0x4a, matches answers by the frame's destination
client port (0x1200+i) with the now-correct transaction as fallback,
keeps the answering client's port for the whole vendor sequence (0x48,
0x4a-poll, 0x3d, 0x3c -- a second port would be a second unregistered
client), and probes up to 64 announcements so none is skipped.  Lines
to read: `the vendor probe: service 0x... (instance ...) at ...:...
says result ..., error ..., state ...`, then `the vendor state service
is ... at ...:..., client ...; its state is ...`.

## The qcrild capture: what the stock daemon actually sends

`strace -f` on qcrild in Android (`.scratch/qcrild.trace`, decoded by
`.scratch/qcap.py`) settles every open question about the vendor
sequence, byte for byte:

* **The vendor state service is WMS (service 0x05), instance 1, at
  3:0x3d** -- not service 0x12 and not 3:0x3b.  Its client sequence:
  `0x004c` TLV 0x01 = `00` (the bind, result 0) -> `0x005c` -> `0x0032`
  with a 26-byte TLV 0x01 routes table `06 00 00 00 ff 02 00 01 ff 02
  00 02 00 01 00 03 ff 02 00 04 ff 02 00 05 ff 02` and TLV 0x10 = 01 ->
  `0x0001` TLVs 0x10/0x11/0x12 = 01 -> `0x0047` TLVs
  0x10/0x11/0x13/0x15 = 01 -> `0x004d` (capabilities; TLV 0x10..0x18 =
  01,01,00,01,00,01,00,01,01) -> `0x005c` again -> `0x0048` (TLV 0x10
  = 00) -> `0x004a` polls, state 00 then 04.  The earlier "result 1,
  error 0x34" on a bare 0x4a is the answer to a client that never sent
  the `0x4c` bind -- not a name-service matter.  Service 0x12's
  bitmap omitting 0x4a/0x3c/0x3d was right: they are not its messages.
* **3:0x3b is DSD (service 0x2a), instance 1** -- qcrild's data-system
  client: `0x0027` TLV 0x01 u32 = 1 (bind), `0x0038`, `0x0024` (Get
  System Status; its 429-byte answer names the APNs cbnet/CMNET/CMWAP/
  ims/SOS), `0x0025`, `0x0047`, `0x0065`, `0x006d`, `0x004f`, `0x0069`,
  `0x0068`.  Its 0x4a answer with result 0 in the old probe run was
  WDS's Set Default Profile Number seen through a different client, not
  the vendor state.
* **The IMS enable is vendor message `0x002a` on service 0x12 (at
  3:0x4b)**: bind `0x0098` TLV 0x01 u32 = **0**; `0x001e` supported
  bitmap; `0x001f` TLV 0x01 u16 = 0x8f and u16 = 0xa2 (two config
  reads); then `0x002a` with TLVs 0x11/0x1f/0x2d/0x31 = 01 -- result 0.
  `0x0090` (Get IMS Services Enabled) is refused result 1, error 70 in
  Android too, so its refusal here was never the blocker.
* **The registration is pushed, never read**: IMSA (service 0x21, at
  3:0x49) bind `0x0033` TLV 0x10 u32 = 0; `0x0022` Register
  Indications with TLVs 0x10,0x11,0x12,0x13,0x19,0x1b,0x1e,0x1f = 01,
  then a second `0x0022` with TLV 0x18 = 01.  Registration arrives as
  indications `0x0023` (TLV 0x12 state, 0x11 error, 0x14 technology,
  0x13 text) and `0x0024` (TLVs 0x10 SMS, 0x11 voice).  Getters 0x20/
  0x21 are never used by Android -- and are refused in Android as here.

The kernel now mirrors all of it: the probe binds every client with
`0x004c` before its 0x4a; `mhi_ims_vendor_prelude` replays 0x5c/0x32/
0x01/0x47/0x4d/0x5c/0x48 when the state is not 4 (three times a boot);
`mhi_ims_vendor_catchup` looks at the state once a cycle (first fifteen,
then every tenth) and writes 0x3d/0x3c the moment it turns 4;
`mhi_ims_enable` sends the 0x1f reads and the `0x002a` enable on
service 0x12; and `mhi_imsa_wait` registers the IMSA indications whose
`mhi_imsa_note` hook logs the registration the getters refuse.

## Run 085605: the WMS winner, the address handoff, and the IMSA decode

Run 085605 (the kernel with the 64-byte IMSA registration request) is
the first whose vendor probe found the state service: "the vendor state
service is WMS (0x5, instance 1) at 3:0x3a, client 0x121d; its 0x4c
bind is accepted" -- the qcrild capture's sequence, one instance port
over.  The prelude was accepted reply by reply (0x5c, 0x32, 0x01, 0x47,
0x4d, 0x5c all result 0) except its last two reads: 0x48 and 0x4a are
refused result 1, error 52 (DeviceNotReady) -- every 0x4a answer the
run holds carries that pair, including the catch-up peeks after the
PDN.  The IMS PDN itself came up in the IMSDCM capture's shape: the
fresh matrix's v6 legs on mux 2 (clients 0x1006 and 0x1008, handles
865654032 and 865382944) were accepted with their 240a:42e6:... address,
the v4 legs refused 14/51 and 14/208 (an IPv6-only IMS network), the
default cbnet PDN accepted on mux 3, and the IMS data service (the
vendor IMSDCM dialogue at 0x3005) was answered, with the address
indication (message 0x20 type 4, 78 bytes) sent as each v6 leg
connected.  The IMSA bind and both 0x22 registrations answered result 0
with no panic: the 64-byte request holds.

What the run could not see was the registration itself, because
`mhi_imsa_note` had never decoded an indication: its type filter was
`(qmi[0] & 0x06) != 0x02`, commented "an answer, which the caller
reads" -- but a QMI indication is 0x04, so every 0x23/0x24 was dropped
before its TLVs were looked at.  The TLVs it would have read were wrong
too.  `imscap2`'s capture of the same modem's IMSA traffic settles the
real layout of 0x23: TLV 0x01 a u8 flag, 0x10 the error u16, 0x11 the
registration state u32 (0 none, 1 registering, 2 registered), 0x13 the
technology u32, 0x14 the URIs as {02 <len> "sip:..." 12 <len>
"tel:..."}.  Android's own indications: state 0, then 1 at 32.598 s,
then 2 at 33.55 s -- about one second after the IMSDCM address
indication carried the same address the PDN had just given it, with
sip:+8619299130041@sn.ims.mnc015.mcc460.3gppnetwork.org and
tel:+8619299130041 in TLV 0x14.  The same capture has WMS's 0x4a
answering result 0, state 0 at that stage, and state 4 once the
vendor stack is up.  `mhi_imsa_note` now accepts type 4 and decodes
TLVs 0x11/0x13/0x14/0x10 as above and logs the URIs, so the next run's
log names the registration and whether the address handoff moves it.

## Run 092358 and the IMSDCM address profile: why the stack never registered

Run 092358 answered the whole IMSDCM dialogue and still never saw an IMSA
`0x23`/`0x24`: the vendor `0x4a` state stayed 0, the dial to 10099 raised a
call status state 4 (CC in progress), mode 6, and then answered result 1
error 48 with no call id, and the WMS raw send heard nothing.  The vendor
prelude itself was accepted message by message at 3:0x3b (`0x5c`, `0x32`,
`0x01`, `0x47`, `0x4d`, second `0x5c`, all result 0, the `0x4d` answer
byte-identical to Android's: TLVs 0x10..0x18 = 01,01,00,01,00,01,00,01,01)
-- and then `0x48` and `0x4a` were refused result 1, error 52
(DeviceNotReady) until the vendor stack was up later in the run, when the
quiet `0x4a` peek answered result 0, state 0.

Re-reading the Android captures byte for byte settles what the IMSDCM
address indication is keyed by.  In both imscap (04:09) and imscap2
(06:17) the daemon's messages carry these TLV 0x10 words:

* capability answer `0x21`: TLV 0x10[1] = 0xfa, TLV 0x11[4] = 2;
* **ready** indication `0x20`, right after that answer: TLV 0x02 =
  `00 00 0d 00`, TLV 0x10[4] = **0x64**, TLV 0x12[4] = 2;
* the stack's APN request `0x20`: TLV 0x01[20] = `03 "ims" ...`, TLV
  0x10[4] = **0x65**, TLVs 0x11/0x12/0x13 = 1/2/2;
* the answer to it: TLV 0x10[1] = 0x15, TLV 0x11[4] = **0x65** (the
  request's word, echoed), TLV 0x12[4] = 2;
* **address** indication `0x20`, 70 bytes in imscap2: TLV 0x02 = 0,
  TLV 0x10[4] = **0x65**, TLV 0x11[42] = `01 00 00 00 25` + the 37-char
  text, TLV 0x12[4] = 2 -- and the request preceded it by half a second
  (06:17:32.02 -> 06:17:32.56).

The driver had the ready indication right (0x64) but sent every address
indication with 0x10 = 100: two unsolicited ones when the IMS PDN's v6
legs connected at 09:27:27/09:27:30, before the stack had asked anything,
and one after the APN request at 09:34:51.  A stack that dispatches on
that word files an address sent as 0x64 as a second ready, which fits a
run whose stack acknowledged everything and never registered.  The
`0x11` text itself was right (inet_ntop form, 38 characters, the fresh
leg's 240a:... address), and the 0x02/0x01/0x12 TLVs matched Android's.

The driver now echoes: the APN request's TLV 0x10 is remembered in
`mhi_imsdcm_profile`, the answer repeats it, the address indication
carries it, and `mhi_imsdcm_indicate` refuses to send before the stack
has asked -- the fresh-leg trigger delivers the address only for a
request that arrived before the PDN did, as Android's daemon would.  A
rebuilt address clears the delivered flag.

Two bind words were wrong against the same capture and are corrected:
the IMSA bind `0x0033` on 3:0x49 sends TLV 0x10 u32 = **0** (we sent 1;
every IMSA getter after it was refused error 70) and the IMS bind
`0x0098` on 3:0x4b sends TLV 0x01 u32 = **0** (we sent 1).

The qcrild capture also corrects one thing written above: the vendor
state service is not pinned to a port number.  Android's boot reached it
at 3:0x3d, run 085605 at 3:0x3a, run 092358 at 3:0x3b -- the name
service hands out a fresh port each boot, and 3:0x3b there answered the
whole WMS vendor sequence, so "3:0x3b is DSD" describes only the Android
boot in which both existed.  Its `0x4c` bind TLV 0x01 = 00 opens the
session, and everything the prelude sends, including `0x0001` with TLVs
0x10/0x11/0x12 = 1, is on the same port as the `0x4a` polls.

## Run 100833: the address handoff works, the modem's IMS stack still does not register

Run 100833 (2026-11-24 10:08) was the first with the echoed IMSDCM
profile and the fresh-client dial in one boot.  It camped LTE on 中国广电
(46015), 4 bars, -98 dBm, radio 8; the attach ran "cbnet" on profile 6
("qdp_profile_ia", PDP type 3, APN type mask 0x431); the IMS settings
service answered the bind, the 0x1f reads and the 0x2a write (the "Get
field ids" 0x8f Set was refused error 1, the old three-flag 0x8f Set and
the 0x90 readback were result 0).

The IMSDCM gating worked as designed: the stack asked its APN request
(txn 9) before any PDN existed, the answer was the explicit deferral,
and when the v6 leg connected (client 0x1006, mux 2, handle 949381808,
ipv6 240a:42ea:810:b53f:d9c0:bbc5:cfee:c6e3/64, both DNS servers) the
address indication went out with profile 100 and was re-sent on the later
0x20 retry (txn 12).  The v4 leg (client 0x1005) was refused result 1,
error 14, verbose reason type 6 value 51; the v6 leg with TLV 0x31 = 2
(client 0x1008) was also accepted, handle 949456112, same address; the
profile-2 v4 leg (client 0x1009) was refused error 14, reason type 2
value 208.  None of it was fatal.

What did not happen: IMSA never left state 0.  Both 0x0022 registrations
(the eight-TLV form and the 0x18 one) answered result 0, every 0x0020
getter answered state 0, and no 0x23/0x24 indication ever arrived --
Android's state 1 (TLV 0x11 u32 = 1) and state 2 (with the sip:/tel:
URIs) never came.  The vendor WMS prelude was again byte-identical to
Android's and again the 0x48/0x4a pair was refused (error 52, later
error 3) with the vendor state never reaching 4; one quiet 0x4a peek
answered result 0, state 0.  The 10099 dial again raised a transient All
Call Status state 4 (CC in progress), mode 6, number "10099", and
answered result 1, error 48, no call id; the end-call answered error 41.

The one start shape never tried is the one the working phone's
imsdatadaemon itself sends (imscap2 06:17:32.234-.335): a fresh WDS
client binds {type 3, interface 4} mux 3 and subscription 2, Set IP
Family 6, then a 22-byte Start carrying exactly TLV 0x14 "ims", 0x19 =
6, 0x31 = 2, 0x32 = 0xff and 0x35 = 1 -- no auth TLV, no 0x39/0x3d, no
APN type mask.  The modem answered result 0 (handle 0x748d0ee0), the
daemon's Get Current Settings carried the address 0.23 s later, and the
modem's own IMS stack registered 0.04 s after that (state 1 at 32.598;
state 2 with sip:+8619299130041@sn.ims.mnc015.mcc460.3gppnetwork.org at
33.55).  Every accepted leg in run 100833 carries qcril's mask shape
instead, which the modem also accepts but which never moves IMSA.

The driver now sends the daemon shape first from the IPv6 fresh client
(Set IP Family 6, then the 22-byte body) and falls back to qcril's mask
shape on a refusal; the run that tests it is recorded below.

## Run 104131: the daemon shape is accepted, the address is handed over, IMSA still stays 0

Run 104131 (2026-11-24 10:41) was the first with the 22-byte
imsdatadaemon start shape on the fresh IPv6 client, and it showed the
whole IMSDCM path working end to end: the fresh client's start was
accepted (client 0x1006, mux 2, family 6, packet data handle 1821910544,
the previous leg's 1821608384 still on mux 3), the Get Current Settings
answer carried the address, and 38 characters of it with profile 100
went to the modem's stack through the indication at 10:52:38 -- the
handoff that Android itself performs.  The default cbnet PDN was
accepted on mux 3 as before.  All four fresh legs except 0x1006 were
refused.

The registration did not follow.  IMSA's bind 0x0033 answered result 0,
error 0, and its 0x0022 registration answered result 0 too, but every
0x0020 status read said TLV 0x12 = 0 (state 0), TLV 0x10 = 0 and TLV
0x11 = 0; the 0x21 services status answered result 0 with TLV 0x16 = 2
(SMS over IMS available) and TLV 0x17 = 1 (voice limited), so the stack
considers its IMS services up while it stays unregistered.  The 10099
dial again raised a transient All Call Status state 4 (CC in progress)
and was refused error 48; the vendor state service never reached 4
(the 0x4a state reads were refused error 3, then error 52).

## Run 114439: service 0x44 refuses the stream -- it is not the settings service

Run 114439 (2026-11-24 11:44) replayed the two streams below to service
0x44 (which this boot announces as instance 1 at 3:0x40) and the modem
refused every single message: all 26 steps answered result 1, the
configuration stream with error 57 (invalid message id), the status
stream with 58 and 74 (log lines 9216-9418).  Service 0x44 is therefore
the wrong peer, and the trace reading that blamed it needed correcting.

Decoding the name-service control packets of the 08:17 boot capture
(qcrild.trace) with the little decoder (.scratch/ann.py) gives the real
map:

- service 0x44 instance 1 is announced at node 3 port 0x41, and the
  conversation on fd 49 is its real protocol: 0x20 {0x01 u32 = 2},
  0x2d {0x01 u64 = 1/2}, 0x22 {0x13 = 1}/{0x11 = 1}, then a repeating
  0x23/0x28 pair {0x0c u32 n, 0x0d u32 = 0x0bb8 (3000 ms), 0x0e u8 =
  0x32 (50), timer tables} -- a keepalive/timer configuration, not the
  feature stream.  The driver now names it "IMS timers".
- the feature/APN stream belongs to service 0x2a (DSD), instance 1 at
  node 3 port 0x3b.  fd 99's lookup of 0x2a is answered with the
  NEW_SERVER announcement of 0x3b; the 0x27 {0x01 u32 = 1} bind answers
  result 0, 0x38 {0x15, 0x18, 0x1a = 1} result 0, and the empty 0x24
  answers result 0 with a 429-byte payload whose TLV 0x11 is the APN
  list (count 6: "cbnet", "CMNET", "CMWAP", "ims", "SOS"), duplicated
  in 0x13; 0x65 {0x11 u32 = 2, 0x12 u32 = 2} and {0x10 u32 = 1} answer
  21 and 7 bytes; 0x68 answers 12 bytes.

The driver's pre-existing mhi_dsd_arm already replays the access-list
subset to 0x2a -- 0x25 {0x10 = 1}, 0x69 {0x10 = 0}, 0x25 {0x10 = 0},
0x24 (with the APN dump), 0x69 {0x10/0x11/0x12 = 1}, 0x68, 0x24 -- and
every message answered result 0 in run 114439, yet IMSA stayed at state
0.  What Android sends before those and this driver never did: the 0x27
subscription bind first, then 0x34, 0x65, 0x6d, 0x25 {0x13 = 1}, 0x38
{0x15/0x18/0x1a/0x1e/0x1f = 1}, 0x47, 0x4f, 0x69, plus the status
stream.  The client (now mhi_dsd_settings_client) replays that whole
stream to 0x2a instead, ahead of the PDN and the registration waits;
its 0x24/0x25/0x69/0x68 steps duplicate mhi_dsd_arm harmlessly.

Run 114439's other results: the IMSDCM path held -- the address
240a:42e6:810:66ae:ec90:b4ea:5741:a1bd with profile 100 and 38
characters went to the modem's stack at log line 10940, delivered while
the WMS raw-send wait ran; the default cbnet PDN was accepted on mux 3
(line 11298); registration stayed 0 in every poll; and the dial to
10099 was refused result 1, error 48 (invalid argument), with no call
id (line 11442).

## Service 0x2a: the settings streams Android's boot configures

The Android boot capture shows qcrild bringing up two more IMS clients
that this driver had never sent: both look up QMI service 0x2a and
address the frames to node 3 (port 0x3b in the 08:17 capture, 0x3c in
our boot), 2.3 s to 12 s into the boot.  Their protocol is an IMS
configuration conversation, and it precedes IMSA's registration in
Android's own ordering:

- 0x27 with TLV 0x01 u32 = 1, three times (the instance's bind with its
  subscription), then 0x34 {0x12 u32 = 1}, 0x65 {0x11 u32 = 2, 0x12 u32
  = 2}, 0x6d (empty);
- 0x25 {0x13 u8 = 1}, 0x38 {0x15 u8 = 1}, 0x47 and 0x4f, 0x38 {0x18 =
  1}, 0x65 {0x10 u32 = 1}, 0x69 {0x10/0x11/0x12 = 1}, 0x68 twice, 0x38
  {0x1e = 1, 0x1f = 1}, 0x68 (the configuration stream, fd 52);
- a second socket (fd 102, the status stream) sends 0x27 {0x01 = 1},
  0x38 {0x15/0x18/0x1a = 1}, 0x24, 0x25 (empty), 0x47, 0x25 {0x10 = 0},
  0x24, 0x25 {0x10 = 1}, 0x24.

The driver now replays both streams byte for byte
(mhi_dsd_settings_client in hakios.thyme_pcie2.c), to service 0x2a: the
configuration stream from client port 0x100e with transactions
0x1600+n, the status stream from 0x100f with 0x1700+n, once per boot,
right before the WDS PDN work and the registration waits in
mhi_wds_ims_up -- Android's order.  A stream whose peer stops answering
is abandoned after three silent steps.  Service 0x44 stays named "IMS
timers" for its keepalive conversation, and because the rolling run log
drops the boot's announcement lines from its front, the driver also
dumps the full name service table once per boot
(mhi_qrtr_survey_dump), so the survey is readable in any kept log.

## Run 120804: the 0x2a stream is accepted; the IMSDCM handle is the block

Run 120804 sent the whole 0x2a settings conversation on both streams and
the modem accepted every step -- result 0, error 0, all 26 steps, no
silent abandonment -- yet IMSA registration stayed 0 in every poll and
the 10099 dial was refused again.  The other results of the run:

- the IMS PDN came up on the fresh client 0x1006 (APN "ims", profile 2,
  TLV 0x32 = 0xff) with the IPv6 address
  240a:42ea:810:f2f2:b45f:5194:5c3c:3b8a, 38 characters;
- the IMSDCM handoff delivered that address to the stack (log line
  around the address indication), the stack's 0x20 APN request was
  answered result 0, and the address indication went out;
- the IMSA bind and both indication registrations answered result 0, and
  the 0x20/0x21 getters answered, but the registration word never left
  0;
- the dial to 10099 was answered result 1, error 48 (invalid argument),
  yet a call-status indication then created call id 1 in state 4 (CC in
  progress); the end-call request was refused error 41.

Decoding the run's IMSDCM exchange against the Android captures found the
difference the driver had carried since the replay was written: the
modem's stack names its own handle in TLV 0x01 of its 0x21 request --
0x15 in both Android captures, but 0x64 in run 120804 -- and Android's
imsdatadaemon echoes that handle in every message it sends afterwards
(the ready indication's TLV 0x01, the 0x20 answer's TLV 0x10, the
address indication's TLV 0x01).  The driver hardcoded 0x15 in all three
places, so the stack was handed indications for a client that never
asked, and its registration never started.  mhi_imsdcm_handle now takes
the value from the stack's 0x21 request and every indication and answer
echoes it (the log line says which handle the ready/address indications
carry).

Also corrected there: Android's 0x21 services-status answer carries the
SMS word in TLV 0x16 (value 2) and voice in 0x17 (value 1); the driver
read 0x10/0x11, which are absent, so its log always said "not given".
The read now tries 0x16/0x17 first and falls back to 0x10/0x11.

## Run 123850: the handle is echoed; the address comes two minutes late

Run 123850 (started 12:38:50, the kernel with the handle echo) confirmed
the echo live: the stack asked handle 0x64 in its 0x21 and every answer
and indication carried it ("the stack's handle is 0x64; the ready and
address indications now carry it"); its 0x20 request named profile 0x64
and the answer's TLV 0x11 echoed it.  The APN request's TLV 0x01 tail
changed with it (Android's `0200ffff` tail became `02006400`), so the
stack does read the handle out of what the daemon sends.  The IMS PDN
came up on fresh client 0x1006 (APN "ims", profile 2, TLV 0x32 = 0xff):
IPv6 240a:42e6:2012:f3d0:cc57:8081:74ae:95b2/64, DNS
2409:8080:2000:0003::1 and ::2, family 6, packet service 2 connected.
The address indication went out at the end of mhi_wds_ims_up with handle
0x64, profile 100, 39 characters.  The registration still never left 0
(seven polls, SMS 2 available, voice 1 limited), no 0x23 state 1/2 ever
arrived, and the dial to 10099 was refused result 1, error 48, with no
call id.

The Android capture says why: its daemon hands the address **0.44 s**
after the stack's APN request (imscap 04:09: 09:17.304 -> 09:17.746), the
stack goes state 1 35 ms after the handoff (09:17.781) and state 2
0.67 s later (09:18.446); its 0x24 with SMS 2/voice 1 (09:16.793) comes
0.5 s *before* the request.  Run 123850's first request arrived during
the IPCR NAS register wait, but the IMS PDN was only started at 12:42:12
by the SMS stage -- two minutes later.  The stack had stopped waiting by
then: at 12:49:29 it reported 0x23 state 0 with **error 999** (Android's
initial 0x23 carries error 0), sent 0x33 SUB_DESTROY_INSTANCE, re-sent
its 0x20 (transaction 12), and this time the answer and the address
indication went out at once because the PDN was already up -- and the
registration still did not start.

So the driver now brings the IMS PDN up in the boot path itself, right
after the "before the registration" attach steering and before the NAS
register: mhi_ims_enable, then mhi_wds_ims_up, then a one-round
mhi_imsa_wait (bind plus the 0x22 indication registration), and
ims_state.brought keeps the SMS stage from repeating it (the SMS-stage
path stays when the settings or the PDN cannot start there).  The
stack's APN request now meets a PDN that is already up, as Android's
does, and mhi_imsdcm_answer answers with the address in the same second.

Two decode fixes ride along: the 0x24 services indication on the wire
carries SMS in TLV 0x16 and voice in 0x17 (run 123850's packet:
16[4]=2, 17[4]=1, Android's 09:16.793 shape), not the 0x10/0x11 the
note read; and IMSA indications that arrive while a QMI call waits on
another port used to be hex-dumped only ("IPCR data elsewhere"), so
mhi_imsa_note now reads those frames too.

## Run 131620: the daemon-shape start is accepted; the address is 108 s late

Run 131620 (started 13:14:5x, kernel 2.3.4-hakios-darwin) proved the
fresh-client start in the IMS data service's own shape is accepted by
this firmware: "IPCR WDS start on the fresh client (the IMS data service's
own shape: APN ims, profile 2, 0x32 = 0xff): message 0x20, result 0,
error 0", IPv6 240a:42e6:2012:3aee:440f:6623:7614:5c67 on client 0x1006
(v4 leg refused result 1, error 14; the cbnet default PDN started too).

The timing still failed, and the log shows exactly where.  The stack
bound with 0x21 (handle 0x64) at 13:17:02.681 and asked the APN with 0x20
(transaction 9, 60-byte request, TLV 0x01 = 20 bytes `03 "ims" 00 00 00 00
02 00 00 00 01 00 00 00 02 00 ...`) in the same second; the driver had no
address and said so ("no IMS PDN address yet").  The address indication
only went out at **13:18:50.845** -- 108 s later -- because the boot
stage calls mhi_ims_enable (its vendor scan probes 44 services and reads
0x4a again with 5 rounds; 0x4a never reached 4 in this run: "the vendor
configuration is withheld: the 0x4a state is 4294967295", the policy
read refused error 52) *before* mhi_wds_ims_up.  The stack did not react
to the late unsolicited indication: no 0x23 state 1/2 ever came, IMSA
reported state 0 with error 0 then **error 999** at 13:26:56, the stack
sent 0x33 SUB_DESTROY_INSTANCE and re-asked 0x20 (transaction 12) at
13:27:03, the address went out again at once (PDN was up by then), and
the run ended at 13:31 with no registration.  The 10099 dial at 13:25:21
was refused result 1, error 48 with no call id.

The Android capture (imscap2, UTC 06:17, the daemon's own sockets)
puts the requirement in seconds: the stack's 0x20 ask at 17:32.022, the
daemon's answer at .029, the daemon's own WDS start of the "ims" PDN at
.319 (APN ims, profile 2, TLV 0x32 = 0xff -- the shape this driver now
sends), the address indication at .562, IMSA state 1 at .598 (**36 ms**
after the address) and state 2 at 18:33.550 with the SIP/tel URIs.
Ready indication was at 17:21.606; the ask comes 10.4 s after it.

Same capture, IMSA port 0x49: message **0x26 is modem-to-AP**.  It
arrives as a type-4 indication 14 ms after the daemon's ready indication
(TLV 0x01 = 00, flag 0) and 16 ms after the address indication (flag 1)
-- the stack's echo of the IMSDCM events, 20 ms before IMSA state 1.
The host sends nothing back.  This driver read only 0x23/0x24 and
dropped 0x26 silently; mhi_imsa_note now logs every unread message id,
so one run says whether this firmware pushes it.

Changes this run makes: the early-PDN stage calls mhi_wds_ims_up *before*
mhi_ims_enable (the enable scan follows; a PDN that did not come up is
retried after the enable, the old order), so the address exists when the
stack's 0x20 arrives; and the 0x20 request dump is 32 bytes a line up to
128 bytes, because the 48-byte lines were cut at byte 27 and hid the
request's profile TLV (0x64 in run 123850).

## Run 140914: the bind comes after the address; eight and a half silent minutes

Run 140914 had the address and the timing both right and exposed an
order Android's daemon never sees.  The ims PDN was up and the stack's
first 0x20 ask was answered with the address at **14:11:23.338** --
under the default handle 0x15, because the stack had not bound yet.
The bind (0x21, TLV 0x01 = 0x64, transaction 10) arrived at
**14:11:31.673**; the driver answered it and re-sent the ready
indication under the new handle, but not the address (it went out only
from the 0x20 path), so the stack's new client had none.  The stack
then sent nothing at all for **8 min 33 s**, destroyed the instance
(0x33, transaction 18, 14:20:05.243, answered result 0), pushed two
unread IMSA messages (id 0x39, TLV 0x01 = 0 then 1, 28 bytes each),
and re-asked 0x20 at **14:20:06.695**.  That ask was answered in full
-- handle 0x64, profile 100, the address; the ask's own words were
0x11 = 0 (V4), 0x12 = 1, 0x13 = 1 -- and the run ended at 14:24:13
with no further traffic from the modem's IMS ports.

The first message of the run was the address family's link report:
0x23 LINK_ADDR on 3:0x3b at 14:09, QMI header `00 01 00 23 00 23 00`,
TLV 0x01[32] = `8b 23 01 00 00 00 19 "fe80::8133:8047:f566:7a75"` --
the modem hands the host its own IPv6 link-local.  The driver answered
bare result 0.

The ask's family word was found by elimination against Android's
working capture: imsdatadaemon's PDP activate handler
(ims_qmi_dcm_pdp_activate_handler.cfi, 0x216b4) validates a struct
field logged as " IP Address Family : %d" to 0 or 1 and its LINK_ADDR
logger says "family_type[%d]. 0 is V4, 1 is V6".  Of the ask's three
4-byte words only 0x11 can be it (Android asks 1, 2, 2; a 2 fails the
check), so this modem asks V4 while the only "ims" leg this network
grants is IPv6.  The daemon tolerates that by naming the actual family
in the address indication, which mhi_imsdcm_indicate already does, so
the word is a suspicion to watch, not a proven gate.

Changes this run makes: the bind branch hands the stack the address
already up, under its new handle; the 0x20 branch logs the ask's three
words; an unread IMSA message's first 64 payload bytes are hex-dumped;
the IMSA bind goes out once per boot (Android's client binds once);
and after every handoff the cellular thread reads the modem's own
enabler layer (IMSP 0x24: 1 uninitialized, 2 initialized, 3 airplane,
4 registered) every ~10 s and the IMSA registration every ~30 s for
four minutes, restarting the clock on a re-indication -- so a stack
that stays silent says whether it is uninitialized, initializing, or
registered underneath.

## Run 152612: the bind re-indication works; the stack's own re-ask asks V4

Run 152612 (boot 15:26, 900 s, the build with the four changes) closed
the 140914 silence: the bind (0x21, transaction 10, handle 0x64)
arrived at about 15:28 and the driver answered, sent the ready
indication and re-indicated the address under 0x64 in the same second.
The IMSDCM order Android's daemon never has to handle -- ask, address,
then bind -- is now answered at every step.

The exchanges around it, in order: 0x23 LINK_ADDR (txn 1), 0x34 (2),
0x23 (3), 0x2e (4), 0x34 (5), the **first 0x20 ask (txn 6) with words
0x11 = 1 (V6), 0x12 = 2, 0x13 = 2** -- Android's shape -- then 0x33
SUB_DESTROY_INSTANCE twice (txn 7, 8), then the bind.  At 15:36:5x
the stack destroyed the instance again (0x33 txn 11) and re-asked 0x20
(txn 12) with **0x11 = 0 (V4), 0x12 = 1, 0x13 = 1** and the APN's own
trailer flipped `ff ff` -> `64 00`: a boot's first ask is V6 and the
stack's own retry asks V4, which is the ask run 140914 saw after its
eight-minute silence.  The re-ask was answered and the address handed
under 0x64 at once; IMSA registration still answers state 0.

The profile word in both asks is `00 64 00 00` (100 in the second
byte), not the 0x65 of Android's capture.

The handoff watch found the modem does not announce IMSP (service
0x1f) at all: "the modem has not announced IMSP; its enabler state is
unknown" every look, so the enabler state is not observable on this
firmware; the 0x4a vendor state read is refused error 52 throughout.
The IMSA services answer, however, is steady: SMS 2 (available), voice
1 (limited).

The unknown IMSA message is now decoded: message 0x39 with a 4-byte
payload, TLV 0x01[1] = 0 then 1 (28-byte frames), a one-byte flip that
travels with the instance destroy; the stack also sends 0x99, 0x4b
and 0x46 indications to the host's IMS client ports, and 0x46 carries
a text: "145", `0x0e`, then a "+8619202900500..." string.

The 10099 dial was refused again: result 1, error 48 (invalid
argument), no call id -- but a voice indication still created call id
1 right after, which the driver ended ("THE END CALL OF 1 (from the
indication) says result 1, error 41").  A call object appears even
behind the refusal, so the voice path is not simply dead.

Next variable: the active PDC config is Volte_OpenMkt-Commercial-CBN
(China Broadnet's VoLTE MBN) while the card is CMCC; the next boot
runs with `pcie2_pdc_cmcc=1` so the modem's own carrier config is the
one the IMS stack reads.

## Run 154832: the CMCC PDC config is not the gate; the first accepted dial dies in origination

Run 154832 (boot 15:48:32, 900 s, `pcie2_pdc_cmcc=1`) selected and
activated the modem's own CMCC VoLTE config: both steps answered
result 0, error 0, and the selected-config read-back changed from
`f1 aa ad 22 07 7c 60 fd ec f1 ca de 9f 91 63 97 56 f3 4c d8`
(CBN) to `8d be 5c 56 c7 2b a5 38 ad 22 e9 f6 78 c7 5f 0a fb bf fd 0a`.
The IMS behaviour did not move: IMSA registration 0, SMS 2
(available), voice 1 (limited) throughout.  So the PDC config is not
the registration gate, and the earlier carrier read is worth
revisiting: the 192-prefix number and 10099 point at China Broadnet,
which makes the original CBN VoLTE MBN the right one.  The next boot
drops `pcie2_pdc_cmcc`.

The dial was accepted for the first time: message 0x96 (0x0020),
call type 0, with the full TLV set (0x10 type 0, 0x16 = 1,
0x18 = 3, 0x19 = 0, 0x22 = 0, 0x23 = 0) answers "THE DIAL TO 10099
says result 0, error 0 (none), THE CALL ID IS 1" -- run 152612's
result 1, error 48 is gone.  The call then never reached alerting:
state 4 (CC in progress) during the dial, state 1 (origination) twice,
then state 8 (disconnecting) twice and state 9 (end) twice.  The
driver's own end call afterwards says result 1, error 41 -- there was
no call left.  Voice is refused after origination, which is what a
network does with a call whose IMS registration never came up.

The 80-byte All Call Status frames do carry TLV 0x14 (the call end
reason, 4 bytes), but the generic hex dump cuts at 48 bytes, so the
value never showed.  The next build decodes it in mhi_voice_note.

The registration retry repeated 152612's shape: IMSA bind 0x33
(TLV 0x10 = 0) answered 0/0; the 0x22 indication registrations
answered 0/0; the getters say registration 0, SMS 2, voice 1,
technology 0xffffffff.  Late in the run an IMSA indication carried
`registration 0, technology 1, error 0`, then the same with
**error 999** -- a value we have no QMI table for; treat it as the
stack's own registration abort, not a mapped error.  Then two 0x39
frames, 0x33 SUB_DESTROY_INSTANCE (txn 14) answered, and a fresh
0x20 ask (txn 15) with words 0x11 = 0 (V4), 0x12 = 1, 0x13 = 1.
The answer went out and the address was handed a third time
(handle 0x64, profile 100).  Registration stayed 0.

What did not happen, and is now the leading difference: **the modem's
IMS stack never echoed our ready or address indications** with IMSA
0x26.  Android's stack echoes each of them within 14-16 ms (imscap2:
17:21.45 ready -> .47 echo; 17:32.56 address -> .58 echo), and reaches
state 1 (registering) 36 ms after the address, state 2 (registered)
0.95 s later.  Comparing bytes, our frames are identical to the
daemon's; the ordering is not.  Android: bind -> ready -> 10.4 s ->
ask -> answer -> PDN start -> address 0.53 s after the answer.
Ours: the PDN is up before the ask, and the address goes out in the
same millisecond as the answer to the ask, and again inside the
bind's own burst.  Runs 140914, 152612 and 154832 all did that, and
all three stayed at state 0.

SMS is consistent with no registration: the submit to
+86192991300141 got no QMI answer at all on any of three attempts
(result/error 0xffffffff), and the WMS transport status read is
refused error 52 (device not ready).

Changes this run makes: the address is no longer sent on the bind and
no longer inside the answer's burst.  The bind answers and readies
the client and, when an address is already up, marks it pending; the
ask answers, marks it pending, and the address indication leaves from
the cellular thread's next cycle (~2 s), only once the stack has both
bound and asked (mhi_imsdcm_bound).  The same pending path carries
the address after the IMS PDN comes up.  This mirrors Android's
step-wise handoff and tests whether the stack's state machine needs
the address as a separate event; the IMSA 0x26 echo is the signal to
watch.

Run 163801 was the first with the step-wise handoff.  The address
left on a cycle of its own, once the stack had both bound and asked,
and the stack still sat at state 0; the modem's IMSA service sent no
0x26 for the ready or the address.  (The log put the address ~3
minutes after the bind answer, but pcie2 lines carry no timestamps of
their own and inherit the previous `^2026-` line, so that gap is
approximate.)  What the run did show:

- the stack's ask came *before* the bind (txn 12 at the end of the
  run repeated it), so the bind's pending block handed the address on
  the bind's own cycle;
- the ready went out inside the bind's burst, in the same
  millisecond as the bind answer;
- after a 0x33 destroy late in the run, the fresh ask repeated the
  same words: 0x11 = 0 (V4), 0x12 = 1, 0x13 = 1;
- registration stayed 0, with the IMSA `error 999` seen before;
- the dial to 10099 said result 1, error 48, and the three WMS raw
  sends got no answer at all (result/error 0xffffffff).

Re-reading both Android captures byte by byte corrected the ready
indication's word.  The ready always carries **0x64**; the ask follows
with **0x65**, and the answer and the address indication repeat the
ask's 0x65:

- imscap 04:09: ready 0x64, ask 09:17.304 0x65, address 09:17.746 0x65;
- imscap2 06:17: ready 17:21.606 0x64, ask 17:32.022 0x65, answer
  .029 0x65, address .562 0x65.

(The earlier reading of imscap as "ready 0x65, ask 0x64" came from a
window that did not line the frames up: the ready's sendto carries
`... 10 04 00 64 00 00 00 ...` and the ask's and address's `... 10 04
00 65 00 00 00 ...`, in both captures.)

Run 180808 then showed what the wrong half does.  Its ready went out
as the complement of the default profile (0x65, no ask parsed yet) and
the stack's ask that followed named **0x64 with family V4 (0/1/1)** --
while run 152612's first ask, which came before any ready, was V6
(1/2/2) and its re-ask after a 0x65 ready was V4.  Three data points
agree: the ready's word is what the stack's ask follows, and 0x64 is
the word that yields Android's V6 ask.  If a ready 0x65 yields a V4
ask, the family word the daemon's handler validates is downstream of
our own indication, not a fixed quirk of this firmware.

## Run 180808: the ready is starved out of its own handshake (2026-11-24 18:08)

Run 180808 (boot 18:08:08, 900 s, the build with the two-cycle ready
countdown and the complement word) never got its ready out in time.
The bind arrived at **18:10:25** (handle 0x64, the 25-byte answer
sent); the countdown was set, but the milestone step that carried it
was the same one that ran the SMS submit, and that submit waited
~118 s for a QMI answer that never came.  The ready therefore left at
**18:14:06** -- 3m41s after the bind -- with the word **0x65**, and
the address followed it in the same second with profile 0x64, ahead of
any ask.  The stack then stayed silent for 4m50s: 0x33
SUB_DESTROY_INSTANCE (txn 11, answered) at **18:18:56**, then a fresh
0x20 ask (txn 12) naming 0x64 with family V4 (0x11 = 0, 0x12 = 1,
0x13 = 1).  The answer echoed 0x64 and the address was handed at
18:19:00 with 0x64.  Registration stayed 0 all run; **no IMSA 0x26
came** for the ready or the address; the 10099 dial was refused
result 1, error 48; the single SMS submit got no answer at all.

The missing 0x26 is consistent with both defects at once: the ready
was 3m41s late and named the wrong half of {0x64, 0x65}.  The 0x26
echo is Android's signal that the modem's IMS stack accepted the
ready -- 14 ms after a daemon ready, 16 ms after a daemon address.

Changes this boot makes: the ready is sent from the bind branch
itself -- `delay(150000)` and then the fixed **0x64** word, the
daemon's own 143-164 ms pause -- so no cellular cycle or slow QMI
call can hold it back; the countdown state is gone entirely.  The
ask's answer is followed **530 ms** later by the address indication
when the IMS PDN already has one (imscap2's .03 -> .56 gap), sent
from the same thread, so a milestone block cannot delay that either;
when no address exists yet, the pending path hands it over after the
PDN brings one.

## Run 185152: the ready is on time; the ask still is not (2026-11-24 18:51)

Run 185152 (started 18:51:52, 900 s) carried the changes the 180808
section ends with.  The bind arrived at **18:54:09** (txn 10, handle
0x64, from 3:0x3d); the final log lost the bind and ready lines to the
snapshot rotation -- they survive only in the mid-run live copy.  The
ready left 150 ms after the bind answer and is **structurally
Android's** -- same TLVs, same order, same length.  Its 0x01 carries
0x64, this firmware's bind word, where Android's ready carries its
stack's 0x15 (the raw traces were re-read for the 191905 section
below); under the echo reading that word is right for this stack:

    04 04 00 20 00 19 00 02 04 00 00 00 0d 00 01 01 00 64
    10 04 00 64 00 00 00 12 04 00 02 00 00 00

The timing and the word are now mechanically right and immune to QMI
blocks.  The IMS PDN came up at 18:54:03 with address
`240a:42ea:2012:9913:2dee:d7cc:fead:3cf9`.

The stack still sent **no IMSA 0x26** for it, then went quiet until
**18:55:21**, when it sent 0x33 SUB_DESTROY_INSTANCE (txn 11,
answered) and a fresh 0x20 ask (txn 12, QMI message 51 bytes) naming
**0x10 = 0x64, 0x11 = 0 (V4), 0x12 = 1, 0x13 = 1** -- the same ask as
run 180808's post-ready ask, even though this ready carried this
stack's own word on time.  That settles the earlier correlation: the
ready's word does not set the ask's.  In every HakiOS run the stack asks
0x64/V4/1/1; Android asks 0x65/V6/2/2.  The answer echoed 0x64 (TLV
0x11 = 0x64, 0x12 = 2), and the address indication (79 bytes, profile
0x64, the 39-character address in TLV 0x11) followed about 1.0 s
later.  No IMSA 0x26 came for the address either -- Android's modem
sends one 16 ms after it -- and the final log holds no 0x26 on the
IMSA ports at all (its five 0x26 hits are adreno, haptic and SMMU
registers).

Registration stayed 0 all run (technology 0xFFFFFFFF, SMS 2, voice 1).
The 10099 dial at 18:55:38 -- **after** the address, unlike earlier
runs -- was again refused **result 1, error 48**; the single SMS
submit got no answer, and two IMSA 0x39 indications went unread.

What remains different from Android inside this handshake is the ask's
word and family (0x64/V4/1/1 versus 0x65/V6/2/2) and the missing
0x26 echo.  The ask is the stack's own choice, not an echo of the
ready; what feeds it is still open.  The strongest external lead is
the PDC: run 154832, the only run whose 10099 dial was **accepted**
(result 0, call id 1), was the one with `pcie2_pdc_cmcc=1` (the CMCC
VoLTE MBN selected and activated) -- 163801, 180808 and 185152 all
ran the default "Volte_OpenMkt-Commercial-CBN" config and were
refused with error 48.  The next experiment repeats this build with
`pcie2_pdc_cmcc=1`.

## Run 191905: CMCC -- the dial is accepted; the bind answer is not (2026-11-24 19:19)

Run 191905 (started 19:19:2x, 900 s) ran with `pcie2_pdc_cmcc=1`
only; the SMS submit was still on (the `pcie2_no_sms` gate came
after).  PDC selected and activated config index 1,
"Volte_OpenMkt-Commercial-CMCC", at 19:21:12.

**The IMSDCM exchange, in three pieces.**  The kept log lost the
early lines; the recovery below is from the kept log itself plus the
`.live` snapshots the run kept.

- 19:19:34 (from `.live.15`): the stack's first ask, txn 6, profile
  **0x64**, family words **1/2/2** -- Android's family words -- before
  any bind and before the IMS PDN.  Answered (handle 0x15, profile
  echo 0x64, 2); the stack destroyed that instance in the same second
  (0x33 txn 7, then txn 8).
- 19:21:27, *inside the PDC exchange window*: the stack bound (txn
  10: `01 01 00 64 10 04 00 01 00 00 00`, i.e. TLV 0x01 = **0x64**,
  TLV 0x10 = **1**), destroyed **instance 1** 0.7 s later (txn 11,
  `01 04 00 01 00 00 00`), and asked with the 51-byte "ims"
  descriptor and profile 0x64 (txn 12).  All three frames were only
  *logged* as "IPCR host request": `mhi_pdc_exchange` answers servreg
  and TFTP and drops every other host-service request, so the bind
  went unanswered and the stack timed it out.
- 19:21:34: the stack bound again (txn 13, same words).  The answer
  went out (25 bytes, result 0, `10 01 00 fa`, and **0x11[4] = 2** --
  Android's instance, not the 1 the stack had bound), the ready 150 ms
  later (`02 04 00 00 00 0d 00  01 01 00 64  10 04 00 64 00 00 00
  12 04 00 02 00 00 00`).  No ask followed -- Android's comes 10.4 s
  after a ready -- and no IMSA 0x26 came.  19:22:48: the address
  indication went out through the pending path (handle 0x64, profile
  100, 38 characters); nothing reacted to it either.

**The call.**  The 10099 dial at 19:23:04 was **accepted**: result 0,
error 0, call id 1 -- the second run ever to get that far, and the
second with the CMCC config selected.  The states went 1 (origination)
-> 8 (disconnecting) -> 9 (end) and the end reason was type 257, code
21 (`01 01 15 00`); our End Call of 1 answered result 1, error 41.
Registration stayed 0 all run (technology 0xFFFFFFFF, SMS 2, voice
1), and no IMSA 0x26 appeared anywhere -- Android's modem sends one
14 ms after the daemon's ready.

**The tail.**  The WMS raw send (txn 179, message 0x20, 51 bytes) got
no answer, and IMSP (0x24) was never announced.  The guest console
stopped at 19:23:55 during the SMS submit; xl reported "no answer --
the kernel hung; waiting for the watchdog", the watchdog reset the
phone, and the run exited 2 at 19:36:06.  That hang is why the SMS
submit now has a `pcie2_no_sms` gate: its ~100 s wait is not worth
the post-dial IMS evidence.

**What it says.**  The raw Android traces were read back with
`imsseq.py` on 2026-11-24: both captures bind `{0x01 = 0x15, 0x10 =
2}`, and the daemon's ready, ask answer and address all carry 0x15;
the ready's profile word is 0x64 in both, the ask's 0x65, and the
address repeats the ask's.  (This doc said "bind TLV 0x01[1] = 0"
and had imscap's W/P swapped; neither trace shows that.)

The bind's TLV 0x10 is the **instance**: the stack's own destroy
(txn 11) names the same value, `0x01[4] = 1`.  Android's daemon
echoes it in the answer's TLV 0x11; this firmware binds instance 1
and every HakiOS answer said 2.  The stack destroyed its instance
instead of asking under it -- in 185152 the same silent wait ran 72 s
before the destroy.  That, and the PDC window dropping the stack's
requests, are the two defects this exchange exposed.

Fix in this build: echo the bind's TLV 0x01 into the handle (ready
0x01, ask answer 0x10, address 0x01) and its TLV 0x10 into the bind
answer's 0x11; the ready keeps the fixed 0x64.  The next build also
dispatches IMSDCM from `mhi_pdc_exchange`.  The acceptance signal to
watch for is the IMSA 0x26, 14 ms after the ready, then the ask
0x65/V6/2/2 10.4 s later.

## Run 203752: the echo takes, the stack destroys and re-asks V4, and the run ends inside the watch

Run 203752 (boot 20:38:05, 900 s, kernel 90b9399372b6 -- the build
with the bind echo and the address gate; `pcie2_pdc_cmcc=1
pcie2_no_sms=1`) returned 1441823 bytes of log and handed the phone
back to Android at 20:53:13.

**The echo took.**  The IMS stack's visible exchange ran at 20:50:21,
inside one second of log time:

- 0x33 SUB_DESTROY_INSTANCE (txn 14), TLV 0x01[4] = 1 -- answered,
  result 0;
- the ask 0x20 (txn 15, 51 bytes), words 0x11 = 0 (V4), 0x12 = 1,
  0x13 = 1 -- the stack's retry shape, not Android's first ask
  (0x65/V6/2/2); profile 0x64; the APN trailer `ff ff`; answered in
  the same millisecond with result 0, handle 0x64, the profile echoed
  and 0x12 = 2;
- the address indication (79 bytes) 0.53 s later (the code's 530 ms):
  handle 0x64, profile 100, `01 00 00 00` + "240a:42e6:2012:23e3:
  94e0:2758:f257:7fb2", 0x12 = 2; "the handoff watch starts (1
  handoff(s) so far)".

The bind and ready lines do not appear anywhere: the console was
dropping and merging lines at that moment (the address line itself
is cut mid-word, "...the ask's 0x11 wordpcie2: AM").  The bind ran
all the same -- `mhi_imsdcm_handle` starts at 0x15 and is written
only in the bind branch, the ask answer and the address carry its
0x64, and the address gate needs `mhi_imsdcm_bound` and
`mhi_imsdcm_ready_sent`, which only the bind branch and the ready
set.  The requested acceptance signal -- the IMSA 0x26 echo -- did
not come (neither 14 ms after the ready nor 16 ms after the
address), and registration stayed 0 (SMS 2, voice 1, technology
0xffffffff, the NAS-backed read error 999) in every poll to the end.
The two 0x39 indications (TLV 0x01[1] = 0 then 1) travelled with the
destroy and the ask, the same one-byte flip as before.

**Why no dial.**  The NAS gate (`!cellular_nas.registered`) only
opened at 20:50:13 -- the first IMSA indications after 12 minutes of
silence -- and the handoff came 8 s later.  The four-minute watch
(120 looks) then ran past the end of the 900 s window: no
"cellular: SMS" line appears anywhere in the run, so the PDC survey,
the IMS enable stage, the transport read and the 10099 dial were
never reached.  The PDC lines are downstream of the same gate (grep
"IPCR PDC": 0 hits in the run).

**This build.**  The staged `mhi_pdc_exchange` dispatch is built in
(the IMS data service answers inside PDC windows, where run 191905's
bind and ask were once only logged); the watch is cut to two minutes
so a late attach cannot eat the dial; the NAS gate now says once a
minute that it is still waiting; and the run goes to 1800 s.

## Run 214445: CMCC is activated, the dial is accepted, the handoff is late (2026-11-24 21:44)

The 1800 s run on the staged-dispatch build (kernel 44c92e17bd6d,
`pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=1
pcie2_nr=0 _pcie2_call=10099 _pcie2_calltype=0 pcie2_pdc_cmcc=1
pcie2_no_sms=1`), 21:44:45 to 22:15:08, the phone back on Android at
the end.

**The PDC survey ran in full and selected CMCC.**  The get selected
config (software and platform), the list of software configs and
the 25 config infos, the set of `Volte_OpenMkt-Commercial-CMCC`
(config 1; 61612 bytes, version 0xa01203d), the activate, and the
get selected config after the selection all answered result 0.  The
destroy and the re-ask that arrived inside the PDC window were
answered by the same dispatch.

**The stack's ask still came V4.**  Words 0x11 = 0, 0x12 = 1,
0x13 = 1 -- the retry shape, not Android's first ask (0x65, V6, 2,
2) -- and it arrived while the PDC switch was still running,
plausibly before the CMCC config was the active one.  Whether the
ask follows the PDC state or the PDN's family word is still open;
the run's address was IPv6 all the same.

**The handoff went out two cycles late.**  The IMS PDN came up (APN
"ims", profile 2, family 6; current settings
240a:42e6:2012:1b5b:78d2:3421:8b80:afb3/64, DNS 2409:8080:2000:
3::1 and ::2; packet service 2).  The address reached the stack --
"the IMS data service hands the stack the address ... handle 0x64,
profile 100, 39 characters", the handoff watch starting (2
handoffs) -- but out of the cellular thread's cycle top ("between
the cellular polls"), minutes after the PDN connected.  Android
hands it 0.44-0.53 s after the ask (imscap2 06:17:32.02 -> .56),
with its IMSA going state 1 at 17.78 and state 2 with the URIs at
18.445; ours stayed 0.

**IMS registration stayed 0 all run** -- SMS 2 (available), voice 1
(limited), technology 0xffffffff, error 0 in every poll (an early
0x23 carried error 999), no 0x26 -- and the two 0x39 indications
again carried TLV 0x01 = 0 then 1.

**The NAS registered and the dial was accepted.**  The UI read "up
1 registered 1 bars 3 dbm -101" and showed 3 bars.  The 10099 dial
answered result 0, error 0, call id 1 (TLV 0x15 = 6), and the all
call status followed 4 (CC in progress) -> 1 (origination) -> 8
(disconnecting) -> 9 (end): the network released it.  End call of
1 answered result 1, error 41.

**SMS milestone.**  UIM EF_MSISDN (6F40) read result 0, error 0,
card 90 00, 56 bytes, two 28-byte records, all ff -- no number;
ADF_USIM/EF_ACL reads result 1, error 3; DMS get MSISDN result 1,
error 16; the target stayed +86192991300141.  The new-message
indication came; the submit was skipped (`pcie2_no_sms=1`).

**Also:**  the DSD experiments (system status with the APNs
"cbnet"/"CMNET"/"CMWAP"/"ims"/"SOS", set 0x0069 three ones, the
dial's status change, set 0x0069 zero) all answered result 0; the
IMS vendor probe (WMS at 3:0x3a with client 0x121d and the 0x46
indication "145"/"+8619202900500...", and 0x2a at 3:0x3b, 0x10
instance 2 at 3:0x4b) was accepted; the thermal line said moderate,
cpu-1-1 at 61.1 C, slowed 15 times.

**This build.**  The address now goes out where the fresh client's
PDN connects (`mhi_wds_ims_up`, 530 ms after the packet service
reads connected), the pace Android's daemon uses; the pending path
stays as the fallback when no address is known yet.  The next run
drops `pcie2_pdc_cmcc` -- a persisted CMCC selection would return
in the early get selected config -- and watches whether the late
handoff was the whole of the registration failure.

## Run 222633: CMCC off, the ask stays V4/0x64/1/1, the dial is refused (2026-11-24 22:24)

Run 222633 ran without `pcie2_pdc_cmcc` (22:24:21 -> 22:49:23, kernel
880290f81695).  The PDC get selected config read active index 0,
"Volte_OpenMkt-Commercial-CBN" (China Broadnet) -- run 214445's CMCC
selection did not persist, the restore in `mhi_pdc_restore_own` had
undone it -- with the CMCC config still at index 1.

The order in the log:  the PDC block (the selection is off), then the
stack's bind (0x64, instance 1), the ready (profile 0x64), the PDN
("the PDN is up before the registration", the fresh client's start
accepted), then the ask -- words 0x11 = 0 (V4), 0x12 = 1, 0x13 = 1,
still the CBN shape, not Android's 0x65/V6/2/2 -- and the address
240a:42ea:2012:6a66:94c8:744a:f44b:bea (handle 0x64, profile 100, 38
characters) handed 530 ms later.  IMSA registration stayed 0; the
10099 dial answered result 1, error 48 (invalid argument) with no
call id; end call result 1, error 41.

So the ask arrives after the PDC block, and 214445's ask came inside
the PDC window before the set/activate -- no run has yet had the
stack's ask under the CMCC config.  The next run keeps it: the new
boot arg `pcie2_pdc_keep=1` makes `mhi_pdc_restore_own` skip the
restore for the whole boot, so the bind, ready, ask and registration
run under CMCC.  Android's ask (V6/0x65/2/2) came 0.30 s before its
own WDS IMS start in imscap2, so the stack's family word is its own
config, not the PDN's.

## Run 231129: CMCC selected and kept, the ask is still V4, the dial is accepted then released (2026-11-24 23:11)

Run 231129 (kernel 7131815be26b) armed `pcie2_pdc_cmcc=1
pcie2_pdc_keep=1 pcie2_no_sms=1`.  The PDC block set selected config
1 ("Volte_OpenMkt-Commercial-CMCC", id 8d be 5c 56 ... fd 0a),
activated it (result 0), and the get selected config after the
selection read that id back -- CMCC was active, and
`pcie2_pdc_keep=1` skipped the restore for the whole boot.

**The ask did not change.**  The stack destroyed its instance (0x33)
and asked again while the after-selection read waited: words 0x11 =
0 (V4), 0x12 = 1, 0x13 = 1 -- the CBN shape -- with the CMCC id
already confirmed active.  A runtime PDC switch does not reshape the
stack's ask; if the ask follows the MBN at all, the config must be
active when the modem's IMS stack initializes.

**The handoff went out on the new path.**  The fresh client's PDN
connected and the address 240a:42e6:2012:498f:8826:b745:e8b6:5abe
(handle 0x64, profile 100, 39 characters) went to the stack 530 ms
later -- the immediate-delivery branch, not a cellular cycle.

**IMSA stayed 0, the dial was accepted.**  The IMSA services
indications read SMS 0 / voice 0xffffffff, then SMS 2 / voice 1; the
line before the call said "the IMS registration before the call is
0, the IMS SMS service 2".  The 10099 dial answered result 0, call
id 1, and the all call status read 4 (CC in progress) -> 1
(origination) -> 8 (disconnecting) -> 9 (end): the network released
it, as in run 214445.  CMCC active and dial accepted again line up;
IMS registration is still the missing piece for a surviving call.

**The run ended early**:  xl returned at 23:17:21, ~5.5 min after
the kernel booted (the last guest line is 23:16:55, a NAS poll in
progress, and no panic line reached the log; Android came back in
10 s).  No cause established yet; the next run watches whether it
repeats.  The selection was left in place, so the next boot's PDC
read tells whether the CMCC selection persists in the modem.

## Run 232421: the battery guard cut the persistence check short; the boot-time instance asks Android's shape (2026-11-25 00:04)

Run 232421 (kernel `7131815be26b`, the standard argument set plus
`pcie2_no_sms=1`, no PDC flag) was the boot that was to answer
whether run 231129's kept CMCC selection survives a reboot.  It did
not get there: the guard read 24% at boot ("stop at or below 25%"),
and after three looks at 22% it took the phone back to the loader
~100 s in.  The PDC block had got through the get selected config
and both list exchanges and the 25 Get Config Info names (all
result 0) when the run ended, so the persistence question stayed
open until run 000740.

What its log does record, in full, is the two ask shapes of one
boot (run 000740 has the same pair):

* **The hello-rounds instance -- before any bind -- asks Android's
  shape**: 0x11 = 1 (V6), 0x12 = 2, 0x13 = 2, profile 0x64, APN
  "ims" (23:24:50).  The host answers it provisionally (result 0,
  handle 0x15, profile 0x64, 0x12 = 2) and holds the address, and
  the stack sends SUB_DESTROY_INSTANCE (0x33) about a second later,
  before any PDN exists.
* **The bound instance (word 0x64, instance 1, port 0x3b) asks
  0x64/V4/1/1**, as in every run since 140914.

## Run 000740: the V4 IMS PDN is refused, the V6 fallback hands the address again (2026-11-25 00:05)

Kernel `d451a981e198` (hakios-kernel md5 `6424b66d`), armed with
`pcie2_pdc_cmcc=1 pcie2_pdc_keep=1 pcie2_ims_v4=1`, battery 61% at
launch.  The run reached every stage: PDC, the start matrix, the
handoff, IMSA, the 10099 dial.

**The CMCC selection does not persist.**  The boot's get selected
config read named index 0 ("Volte_OpenMkt-Commercial-CBN") active
with CMCC at index 1 -- 231129's kept selection was gone after the
reboot to Android and back.  The experiment then selected and
activated CMCC again (result 0) and kept it; the after-selection
read still carried the stack's ask with words 0, 1, 1.  A runtime
PDC switch, even one active before the bind, does not reshape the
bound ask.

**The family-4 start is refused.**  The daemon-shape start with
family 4 (APN "ims", profile 2, the rest of the capture's body)
answered result 1, error 14 (CALL_FAILED), verbose reason type 2
208 -- the same internal verdict run 021620 read on subscription 1:
profile 2's IMS call path takes only the IPv6 shape, so a V4 PDN
cannot be obtained this way.

**The designed fallback ran.**  The captured V6 leg on the same
fresh client was accepted (client 0x1006, mux 2, family 6; a retry
client 0x1008 too), the packet service read connected, and the
address 240a:42ea:810:67b:a1df:be78:48a7:eb05 (handle 0x64,
profile 100, 37 characters) went to the stack 530 ms later -- with
family word 1 while the ask's 0x11 word was 0.

**IMSA stayed 0 and the dial was released.**  The registration
getter answered 0 / SMS 2 / voice 1 in every poll; the modem's own
IMSA indications between polls said registration 0, technology 1,
error 999.  The 10099 dial answered result 0, call id 1, and the
all-call status followed 4 (CC in progress) -> 1 (origination) ->
8 (disconnecting) -> 9 (end).

**The two-instance picture.**  Both 232421 and this run show the
hello-rounds instance asking Android's exact shape (0x11 1, 0x12 2,
0x13 2) and dying ~1 s after the host's provisional answer, and the
bound instance (word 0x64, instance 1) asking 0x64/V4/1/1 minutes
later.  The difference is not the PDC config (CMCC was active before
the bound ask here) and not the UIM posture (the switch and the
provisioning run after the bind).  The next kernel tests whether the
word, rather than the address, is what the stack checks:
`pcie2_imsdcm_ask_family=1` hands the V6 address under the ask's
own 0x11 word (0) instead of the address's (1).

## Run 004255: the ask comes before the bind; word 0 lands and registration does not move (2026-11-25 00:43)

Run 004255 (boot 00:42:55, kernel 18d901d6db62, image 6ef0e8eb7b37,
1800 s, battery 49%) was the `pcie2_imsdcm_ask_family=1` experiment,
with no PDC flag -- the boot's default carrier config, CBN, active.

**The stack asked before it bound, once.**  All of the boot's IMSDCM
traffic came from one source port, 0x3d: the hello-round ask (txn 6,
Android's 1/2/2) died with a 0x33 SUB_DESTROY_INSTANCE (txn 7); the
PDC window's ask (txn 9, words 0/1/1, 60-byte request, the APN `ims`
blob, profile 0x64, trailer `ff ff`) was answered; the bind (txn 10,
word 0x64, instance 1) was answered with the 25-byte echo and the
32-byte ready (profile 100) -- and then the stack said nothing: no
0x33, no second ask in that window.

**The address still went out, twice, under family word 0.**  The
pre-bind ask's pending flag survived the bind, and the cellular
thread's pending delivery handed the address at 00:53:46.6
("between the cellular polls", 79 bytes, handle 0x64, profile 100,
39 characters, 240a:42ea:2012:3e9d:b457:3e3f:8469:da7d, family word
0): "the handoff watch starts (1 handoff(s) so far)".  At about
01:04 the stack itself asked again (0x20 txn 14, 51-byte payload,
the same APN blob, words 0/1/1), was answered, and the address went
out at once again in the same shape, family word 0 (2 handoffs).
The ask_family knob did what it was built to do -- the handoff
carries the ask's 0x11 word (0), not the address family (1) -- and
the machine's reaction was exactly the same as with word 1 in
000740: none.

**Registration did not move.**  Every poll to the end answered
registration 0, SMS 2 (available), voice 1 (limited), technology
4294967295, error 0; the modem's own IMSA indications between polls
said registration 0, technology 1, error 0 then error 999, and the
0x39 pair (TLV 0x01 0 then 1) travelled with the second ask.  No
IMSA 0x26 echo appeared anywhere.  The family word is therefore not
the gate the capture suggested it might be.

**The dial was refused in the CBN shape.**  The indication register
(0x03) answered result 0; the dial (txn 150, message 0x20, 52
bytes, number 10099, type 0) was answered result 1, error 48 with
no call id -- yet the modem's own 0x2e all-call status arrived
while the dial waited: id 1, state 4 (CC in progress), voice, MO,
mode 6.  The driver's end call (txn 151, message 0x21) answered
result 1, error 41 (no such call).  The refused-dial-with-a-call-
object shape is the one 152612 recorded; every run with CMCC active
(191905, 214445, 231129, 000740) had the dial accepted, every run
with the boot's CBN config (222633, this one) had it refused, so
the PDC config remains the dial-accept gate even though 154832
showed it is not the registration gate.

**The SMS transport read was refused again** (WMS 0x4a result 1,
error 52, state 4294967295), and the transport before the submit
read 4294967295.

**Changes this run makes:** the kernel accepts a pre-bind ask as
belonging to the bind that follows it when `pcie2_imsdcm_keep_ask=1`
is set and the ask came from the very port that binds; the address
then goes out 530 ms after the ready (the ask path's pace) instead
of waiting for the cellular thread, and a 0x33 SUB_DESTROY_INSTANCE
from that port clears the kept ask (the hello round's shape).  The
knob defaults off, preserving the old behaviour.  Built as kernel
md5 24e64500e27d353ad3378a8a2dd6059c.

**Next run:** `pcie2_pdc_cmcc=1 pcie2_imsdcm_ask_family=1
pcie2_imsdcm_keep_ask=1` -- the accepted-dial config together with
the kept-ask kernel, to see whether the stack reacts to an address
that arrives inside its own bind second under CMCC.

## Run 012858: the kept ask hands the address at PDN-up; the ready still names the capture's instance and the stack destroys it (2026-11-25 01:29)

Run 012858 (boot 01:28:58, kernel 18e4c179a6f7, 1800 s cap, the
guest returned after 10 min, battery 34%) was the keep_ask kernel
with `pcie2_pdc_cmcc=1
pcie2_imsdcm_ask_family=1 pcie2_imsdcm_keep_ask=1`.

**The kept ask survived the bind and the address went out at the
PDN.**  The ask the stack had sent from the very port that binds was
preserved across the bind, so when the family-6 leg's PDN came up
the driver handed the address 530 ms later: "the fresh client's PDN
is up: the IMS data service hands the stack the address
240a:42ea:2012:a144:68ae:31e2:7336:dc4c (family word 0, handle 0x64,
profile 100, 39 characters)", and "the handoff watch starts
(1 handoff(s) so far)".  The 10-minute pending fallback of 004255
did not repeat; the keep_ask path works as built.

**The sequence, in file order.**  Bind txn 10 (word 0x64, instance
1; the answer echoes the instance) -> ready (profile 100) -> a 0x33
SUB_DESTROY_INSTANCE (txn 11) immediately after the ready -> the
stack's ask txn 12 (words 0x11 0, 0x12 1, 0x13 1) answered with the
address held -> PDN up, address handed -> the stack re-binds
(txn 13, same word and instance; answer, ready) -> a later ask is
answered and the address goes out at once.

**Registration did not move and the dial died in origination.**
Every poll answered registration 0, SMS 2 (available), voice 1
(limited).  No IMSA 0x23/0x24/0x39 appeared in this run at all.
The 10099 dial answered "result 0, error 0 (none), THE CALL ID IS
1"; the all-call status followed 4 (CC in progress) -> 1 -> 1
(origination) -> 8 -> 8 (disconnecting) -> 9 (end), and the end
call answered error 41.

**The 0x33 right after the ready is the new lead.**  The bind
answer echoes the instance (the 191905 fix), but the ready still
carried TLV 0x12 = 2 while the bind's instance is 1, and the ask
answer and address carried the capture's fixed 2 while the ask's
own 0x12 is 1.  Every message Android's daemon sends repeats the
stack's instance word (imscap2 carries 2 everywhere because its
instance is 2).  Run 191905's stack destroyed its instance 0.6 s
after an answer that named the wrong instance; 012858's stack
destroys it right after a ready that still names 2.  The words, not
the timing, are therefore what the stack checks next.

**Changes this run makes:** two globals.  `mhi_imsdcm_instance`
records the bind's TLV 0x10 and is echoed in the ready's TLV 0x12;
`mhi_imsdcm_ask12` records the ask's TLV 0x12 and is echoed in the
ask answer and in the address indication.  Built as kernel md5
7eee9dc73700ab6a68f23898b6cf91cb, xnu-usb.img sha256
ef73a1c41247d629c683a1e7ecfbc32c0f51d1a75229e00799c8541e3db5dff7.

**Next run:** the same config with the echo kernel -- if the 0x33
after the ready disappears once 0x12 is 1 everywhere, registration
is where to look next.

## Run 021755: the echo works and IMSA registration moves off 0 for the first time (2026-11-25 02:17)

Run 021755 (boot 02:17:55, kernel 715e2a9b7dde -- the echo kernel,
image ef73a1c4..., 1800 s, battery 34%) was the same experiment as
012858, `pcie2_pdc_cmcc=1 pcie2_imsdcm_ask_family=1
pcie2_imsdcm_keep_ask=1`, on the kernel that echoes the stack's own
instance words.  (Run 014629, the first attempt, died at 01:48:19
after 110 s: battguard read the charge at 25 % and then 23 %
(3755 mV, -197 mA) and took the machine back to the loader before
the CMCC switch had even run -- no usable data, and it is not
counted here.)

**The echo took.**  Every ready frame now carries `12 04 00 01`
(TLV 0x12 = the bind's instance 1), every ask answer carries the
ask's own word, and both address frames end `... 12 04 00 01 00 00
00`.  The capture's fixed 2 is gone from all three.  CMCC was
selected and activated ("CMCC config is selected and activated"),
and the stack's bind word this run was 0x15, Android's word, not the
earlier firmware's 0x64.

**Registration moved off 0, for the first time in any run.**  After
the IMSA bind (txn 198), the indication register (0x22) and the
0x18 indication, the 0x20 status read and the 0x21 services read,
the modem sent its own 0x23 indication and the driver read:
"the registration is 1, technology 1, error 4294967295" -- state 1
(registering).  A second 0x23 followed immediately with TLV 0x10 =
`28 03` (0x0328 = 808) and state 0: "the registration is 0,
technology 1, error 808".  A sweep of every kept log confirms state
1 had never appeared before: every earlier run reported 0 from
first poll to last.  808 is the first non-999 registration error
this modem has given.  (The "808" in the 09:44 dial window above is
the microseconds of the timestamp 22.808, not an error word; it is
not evidence.)  No QMI table in this repo maps 808.

**The rest of the shape is unchanged.**  The 0x33
SUB_DESTROY_INSTANCE still follows the first ready (txn 11, port
0x39); the stack re-binds twice more and both times the address is
handed 240a:42e6:2012:e444:1c0b:519a:b9af:d315 under the ask's
family word 0.  Every later poll answers registration 0, SMS 2,
voice 1.  The 10099 dial is accepted (result 0, call id 1) and the
all-call status runs 4 -> 1 -> 1 -> 8 -> 8 -> 9 as before.

**What this establishes.**  The instance-word echo is a real part of
the contract: with it, the modem's IMS stack attempts registration
for the first time.  What stops it is now a registration-stage
rejection (808) rather than the stack never trying.  The next run
repeats the same configuration to see whether state 1 and 808
reproduce, and pulls the 0x23 frames of both indications to compare
their TLVs.

**The one byte that still differs from Android is the address's
family word.**  Re-reading imscap2's daemon socket (port 0x3e) frame
by frame around 06:17:32: the stack asks at 32.022 (`00 09 00 20 00
33 00 | 01 14 ... "ims" | 10 04 00 65 ... | 11 04 00 01 ... | 12 04
00 02 ... | 13 04 00 02 ...` -- profile 101, family **1**, words 2/2),
the daemon answers at 32.030 (handle 0x15, profile echo 0x65, ask12
echo 2), the WDS start goes out at 32.319 (APN ims, 0x19 = 6),
and the address indication follows at 32.562 with `11 2a 00 01 00 00
00 25 "240a:42e6:810:ebb:..."` -- family word **1** under the IPv6
text -- and IMSA state 1 lands 0.036 s later (32.598).  This run's
asks both carried family 0 (0x11 = 0 at lines 6440 and 7404), and
with `pcie2_imsdcm_ask_family=1` the indication copied that 0 into
the address's family word while the text stayed IPv6 -- a shape no
Android capture has.  The old fixed-2 question is settled the other
way: the bind instance here is 1 (Android's stack binds 2), and the
ready/answer/address echo it (`12 04 00 01`), so the frames differ
from Android's only in that instance number and in the family word.

**Next run (025606, in flight):** the same config with
`pcie2_imsdcm_ask_family` dropped, so the indication derives the
family from the address text as Android's daemon does and the word
is 1.  If 808 persists with family 1, the remaining suspects are
the ask's own family 0 (the stack asking V4 while the daemon starts
a V6 leg) and the profile the stack names (100 first, Android's
stack asks 101).

## Run 025606: family word 1 on the wire, and registration still answers 808 (2026-11-25 02:56)

Run 025606 (boot 02:56:06, kernel 715e2a9b7dde -- the echo kernel,
image ef73a1c4..., 1800 s, battery 33% at boot; battguard took the
machine back at 25%) was the family experiment: `pcie2_pdc_cmcc=1
pcie2_imsdcm_keep_ask=1`, `pcie2_imsdcm_ask_family` dropped so the
address indication derives its family word from the address text as
Android's daemon does.

**The family word is 1 in both address frames, exactly Android's
shape.**  The stack's asks still carry family 0 (`0x11 0`, words
0x12 1, 0x13 1 at both ask points), but the indications now read
`11 2b 00 01 00 00 00` under the IPv6 text -- family word 1, a
43-byte TLV, the same shape as imscap2's frame at 32.562 (there 42
bytes because its text is one character shorter).  The first
handoff names profile 100 (0x64), the second 101 (0x65); the text
is 240a:42ea:810:d9be:8c23:2ffe:b7da:496b both times, 38
characters.  The ready frames end `12 04 00 01` as in 021755.  The
driver's frames now match Android's apart from the stack's own
instance and ask words.

**Registration ran the same arc and died on 808 again.**  The
modem's own 0x23: "the registration is 1, technology 1, error
4294967295", then at once "the registration is 0, technology 1,
error 808".  Every later poll answered registration 0, SMS 2
(available), voice 1 (limited), technology 1, error 808 -- six
polls.  A third bind/ready cycle followed, as in 021755.  The 10099
dial was accepted (result 0, error 0, call id 1) and the call
status ran 1 -> 1 -> 8 -> 8 -> 9 -> 9.

**Verdict: the family word is ruled out.**  808 reproduced with the
word at Android's 1, so it is not what the stack checks.  What
still differs from the Android capture is the ask's family word
(ours 0, Android 1), the ask's 0x12/0x13 words (ours 1/1, Android
2/2), and the profile the stack names (100 then 101, Android 101).

**The IMS settings path is visibly on, so 808 is not an unset
flag.**  The same run's IMS service 0x12 exchange: the supported
bitmap lists 0x8f/0x90/0x98, the Get-shaped Set 0x8f (TLVs 0x11,
0x18, 0x1a) was refused result 1 error 1, the old three-flag shape
(0x10, 0x18, 0x1a) was accepted result 0 error 0, and the SMS
setting read back "on".  VoLTE and SMS-over-IMS are therefore being
set on, and the modem still answers 808.

**A driver bug this experiment surfaced, fixed before the next
run.**  The accepted family-4 branch of the daemon-shape IMS PDN
start left `fresh_families[f]` at its static 0x06, so the
current-settings parse would have looked for TLV 0x26 (absent on a
V4 leg) and never read the IPv4 address through
`mhi_wds_v4_address`.  The accept branch now records
`daemon_family`, the kernel was rebuilt at 03:04:46, and
xnu-usb.img carries sha256
a4d8cdb13eb559ad1a4ec86ddf325567c8a2c823595350cc0902d6dd2eb2b5d8.

**The active MBN is already the SIM's own carrier config: the PDC
lead is closed.**  Config 0 of the PDC enumeration -- the software
config active at boot -- reads `Volte_OpenMkt-Commercial-CBN`
(size 61172, version 0xa011212, kept-025606 line 4145), and this
SIM is China Broadnet, not China Mobile: Android's own registered
IMS URI in imscap2 is
`sip:+8619299130041@sn.ims.mnc015.mcc460.3gppnetwork.org` (MNC 015
is China Broadnet, and 192 is its number range), while the CMCC
config the run selected is a carrier this SIM does not belong to.
The modem auto-selects its MBN from
`/vendor/firmware_mnt/image/modem_pr/mcfg/configs/mcfg_sw/oem_sw.txt`
by SIM -- the same tree HakiOS serves the modem through TFTP -- so
Android and HakiOS boots start on the same CBN VoLTE config.  The
MBN is ruled out as the 808 cause, `pcie2_pdc_cmcc` is dropped from
the runs, and the `Volte_OpenMkt-Commercial-CMCC` name that
motivated the experiment was read off the wrong entry.

**The address text is not the difference either.**  Android's
address indication (imscap2 06:17:32.56) carries
`240a:42e6:810:ebb:4b41:bd55:b67e:6bd4` -- the same 240a:42ex:810
China Broadnet IMS prefix family as HakiOS's
`240a:42ea:810:d9be:8c23:2ffe:b7da:496b` -- with the same family
word 1, handle 0x15, profile 101 and a TLV 0x02 of 0.  Android's
six-frame IMSDCM conversation (bind, bind answer, ready, ask, ask
answer, address) is byte-identical to the driver's modulo the
stack-chosen handle (0x15 here vs 0x64 in this firmware), instance
(2 vs 1) and the transaction counter.

**Next run:** `pcie2_imsdcm_keep_ask=1 pcie2_ims_v4=1` on the
rebuilt kernel -- the daemon-shape IMS PDN started as family 4, so
the stack receives an IPv4 address (TLV 0x1e, family word 0) from
the leg whose family its ask names.  If the modem refuses the V4
start the driver falls back to the V6 leg and logs it; if it
accepts and registration still answers 808, the PDN's family is
ruled out too and the remaining suspects are the ask's 0x12/0x13
words and the named profile.

## Run 033649: family ruled out, and the live phone's IMS PDN is an "ims" PDN on mux 4 (2026-11-25 03:36)

Run 033649 (boot 03:36:49, kernel 2.3.4-hakios-darwin22.5-sm8250-
g37c040ff-dirty, battery 32%, `pcie2_imsdcm_keep_ask=1
pcie2_ims_v4=1`, no `pcie2_uiswitch`) ran the V4 experiment and
answered the family question the other way.

**The V4 daemon leg was refused 1/14 (verbose CM 208) and the V6
one -- the same body on the same client -- was refused result 1,
error 70 (INVALID_OPERATION).**  Error 70 is new: every earlier
refusal of this body carried 14 or the CM ACL's 1042.  The plain
no-APN reference leg (auth none, family 6, call type 1, 0x39 = 1,
0x3d = 1, the ims APN-type mask 2) was accepted on mux 2 and
brought the session up: IPv6 240a:42e6:2012:0fbf:1021:eecf:74c4
:7438/64, DNS 2409:8080:2000:3::1 and ::2, packet service 2.  That
address was what the IMSDCM hand-off gave the modem's IMS stack
(the bind/ready/address conversation ran intact), and IMSA went
state 1 then state 0 with error 808 again, six polls.  The PDN's
family is therefore not what the stack refuses.

**The UIM block never ran in this boot, and NAS registered anyway.**
`uim_port` was 0 when the card stage executed, so none of the
switch/provisioning/probe traffic exists in the log -- the UIM
frames there are the IMS vendor probes and the later SMS reads.
NAS Initiate Network Register still answered result 0, error 0
(twice: initial and "online"), selection preference result 0.  The
earlier "switch + provisioning -> registered" evidence belongs to
run 021620, not here; run 022954's refusal result 1, error 37
(UIM_UNINITIALIZED) is the run where the block did run its status
reads.  Registration is not the UIM block's product.

**The live Android reference: the IMS PDN is an "ims" PDN on mux 4,
subscription 2** (`/tmp/cap.trace`, pid 2103, fds 29/32 to 3:0x36,
while the phone's own IMS registration was up): Bind Mux Data Port
0x00a2 with TLV 0x10 = `03 00 00 00 04 00 00 00` {type 3,
interface 4} and TLV 0x11 = 04 (mux 4); Bind Subscription 0x00af
TLV 0x01 = 2; Set IP Family 0x004d = 6; Start Network 0x0020 with
TLV 0x14[3] = "ims", 0x19[1] = 06, 0x31[1] = 02, 0x32[1] = ff,
0x35[1] = 01 -> **result 0, packet data handle 0xb78c4dd0**; Get
Current Settings 0x002d with mask 0x00008210 -> TLV 0x26[17] =
`240a 42ea 2012 a36e ... b0 40` (240a:42ea:2012:a36e::b0/64),
TLV 0x27/0x28 = 2409:8080:2000:3::1 / ::2, TLV 0x2b = 06.  The
phone's interfaces agree: rmnet_data2 carries 240a:42ea:2002:c351
::/64 (the data PDN) and rmnet_data3 carries 240a:42e6:810:7515::/64
(the IMS PDN, prefix 0810 as in imscap2).  Its APN setting is
`中国广电 (CBN) GPRS ... cbnet ... supl | hipri | default | xcap,
IPV4V6` -- the default data APN, and the IMS PDN is a separate
"ims" PDN beside it.

**So the accepted leg was the wrong PDN.**  Our accepted start is
the default-profile (cbnet) body, so the address the stack received
belonged to a data PDN; Android's stack is handed a PDN started
with the ims APN and profile 2 on mux 4.  The live Start's TLVs are
byte-identical to the daemon body that got error 70 on mux 2; what
was never tried is Android's own **mux (4)** and its **prepared
profile**.  Profile reads are the missing evidence: all of run
033649's 0x002b reads from the shared client came back unanswered,
while Android's identical requests on clients bound to the
subscription are answered (`/tmp/bootqmi.trace`) -- the survey
client now binds subscription 2 first and the reads use the slow
call.

**SMS stage, same run:** EF_MSISDN reads through ADF_USIM answered
error 3 / 7fff on every path, the fallback SELECT-less reads
returned ff data, and the number it finally printed,
+86192991300141, has one extra digit against Android's own IMS URI
sip:+8619299130041@sn.ims.mnc015.mcc460.3gppnetwork.org -- treat the
card-number read as suspect.  The WMS raw send was answered with no
answer at all.

**Next run: the ladder.**  f0 = the data service's own body on
Android's live posture (mux 4, subscription 2, profile 2, APN ims,
family 6); f1 = qcril's accepted data body (auth none, APN inline,
0x39, 0x3c) with APN ims and profile 2; f2 = the daemon body with
no profile index (the modem resolves the APN; `pcie2_ims_v4=1`
still makes this leg try family 4 first); f3 = the mux-2 no-APN
reference leg as the fallback PDN.  The first accepted leg ends the
ladder and its address is the one the stack gets.  The default-PDN
refusal retry moved from mux 4 to mux 5 so mux 4 stays the IMS mux,
and `mhi_hex_line` now dumps 128 bytes so the verbose and
current-settings answers are complete.  If an ims-APN PDN is
accepted and IMSA still answers 808, the APN is ruled out and the
remaining suspects are the ask's family and 0x12/0x13 words.

**Run 052215: the ladder worked; IMSA still 808.**  f0 (client
0x1005, mux 4, subscription 1 -- the UIM block had provisioned the
card into logical slot 1, so `mhi_wds_card_sub` was 1 --, family 6,
APN ims, profile 2) was **accepted**: "THE FRESH CLIENT'S START WAS
ACCEPTED (client 0x1005, mux 4, family 6, packet data handle
3079992112)", and its current settings carried ipv6
240a:42ea:0810:0b7f:d879:eb52:ea33:05ea/64 with DNS
2409:8080:2000:3::1 and ::2 -- the operator's IMS prefix, the same
one Android's live ims PDN had.  So the APN/mux/profile posture is
ruled in and the address handed was from the IMS PDN.  The stack
bound twice (before NAS: profile 100; after NAS: profile 101) and
both handoffs went out (family word 1, handle 0x15, 36
characters); the stack echoed both with IMSA 0x26 flags 0 then 1,
as in run 033649.

IMSA still never reached 2: "the registration is 1, technology 1,
error 4294967295" then "the registration is 0 (not registered),
SMS 2 (available), voice 1 (limited), technology 1, error 808" --
repeated at every 0x20 poll.  The 10099 dial answered result 1,
error 48 (invalid argument) with no call id (a call-status
indication 0x2e state 4 "CC in progress" had arrived just before),
END CALL result 1 error 41, and the dial's own IMSA indications
(0x23 txn 7/8) were state 1 then state 0 + error 808.  A refused
dial's registration attempt is the same 808 as the idle one.

The vendor probe still found no state-4 service: WMS (3:0x3a) now
answers its own 0x4a with TLV 0x10 = 01 ("transport in process",
used by the SMS milestone), which is a different word from the
vendor state; the 0x12 entry (3:0x49) still refuses 0x4a error 3.

**What Android does differently (imscap2 06:17 capture):** all the
vendor traffic -- 0x4a state poll, 0x48 policy read, 0x3d with the
37-byte TLV 0x10, 0x3c with TLV 0x01 = 01 01 -- goes to
**3:0x3b, the modem IMS stack's own port**, not to any announced
service; the same port the stack's IMSDCM bind (0x21) comes from
(on this boot 3:0x39, our client 0x3005).  Its 0x4a answers 0x00
until 06:17:33.967, then **0x04** about 1.5 s after the address
handoff, and the 0x48/0x3d/0x3c writes follow.  Our probe has only
ever asked the announced services, so it never saw that state.

**Next run:** the driver now aims the vendor sequence at the
stack's own port (client mhi_imsdcm_from, node/port from the bind)
as soon as it is known, skipping the WMS prelude there, and the
cellular cycle's catch-up polls the same port; the vendor
configuration is written once 0x4a says 4.  `pcie2_ims_f0v4=1`
makes the accepted f0 posture run family 4 first (V6 retry follows
a refusal), to test whether the PDN family, rather than the
address, is what the stack's 808 depends on (the stack's re-asks
name family word 0/V4; run 191905's early ask named Android's own
1/2/2).  The V4->V6 retry also re-sets the fresh client global
the first start consumed (`mhi_wds_start_client = fresh_ports[f]`),
a latent bug this boot never hit because f0 was accepted on its
first try.  Profiles 1-5 still answer nothing even from the
subscription-bound survey client and profile 7 answers error 81,
so profile 2's content remains unproven.

**Run 055427: the modem fell off the bus 3.5 minutes in; the 808
was heard before it did.**  The final `.log` says nothing but "TX
no completion" (1796 lines) and "the NAS is still not registered"
(30 lines, 780..900 looks), and taken alone it reads like a dead
NAS service.  It is not: every one of those 1796 lines belongs to
`mhi_cellular_poll` (898 serving-system and 898 signal-info
sends), and they all come *after* the modem reset itself.  The
kept snapshots hold the real story: right after the IPCR NAS
selection preference (`.live.107` line 13030, txn 0x60 message
0x34) the modem opened its EFS session ("the modem opens its EFS
session (version 2.1)"), and then, at ~05:58:26 host time,

    pcie2: MHI: fault at AMSS-MHI: status 0xffffffff, execenv 0xffffffff
    pcie2: MHI: fault diag: MDM_ERRFATAL 0, MDM_STATUS 1, LTSSM 0x5: link not L0, config not read

-- the PCIe link left L0 and the fault registers are unreadable
(0xffffffff).  Every later `mhi_send` failed (`mhi_poll` EPROTO on
the dead link, `mhi_post` EBUSY while `tx_pending` stuck), which
is why the NAS register was "not answered" and the poll spammed
the log until the run ended.  The snapshots also show this is a
*new* failure mode: run 052215 had no MHI faults at all.

What the modem said before it died (all in `.live.100`/.live.101):
the ims PDN came up (f0 refused family 4 first -- error 14, CM
208 -- then family 6 accepted, the same posture as 052215); the
stack bound and asked, and both handoffs went out (profile 100,
then 101; family word 1, handle 0x15, 39-character V6 address);
IMSA bound and registered its indications (0x33 result 0, both
0x22 registrations result 0); the registration getter answered
result 0 with TLV 0x12 state 0, TLV 0x11 error **808**, TLV 0x14
technology 1, and no TLV 0x13 string; **no 0x23 indication
arrived** (unlike 052215, where 1 then 0+808 did).  The vendor
probe found the stack's own port 3:0x3b **silent** to 0x4a ("named
no 0x4a state") while the announced 0x12 entry 3:0x39 answered
state 1; the 0x48 policy read answered flag 0x10 = 0, so the
0x3d/0x3c write was withheld, as designed.

The dead-link spam also destroyed the evidence window: 1796 poll
lines pushed the IMS stage out of the rolling log within minutes.
The next run carries four driver changes: (a) the cellular poll
goes quiet after ten silent answers in a row (a link that left L0
does not come back before the next boot); (b) IMSA is bound and
its indications registered as early as the IPCR IMS branch runs,
before the PDN and the vendor settings, matching Android's daemon
order (its 0x23 is the only witness of the modem's own attempts;
the getters answer 808 either way); (c) while the NAS has not
registered, `mhi_sms_milestone` still polls IMSA every ~30 looks
once a handoff has gone out, so a late indication is not missed;
(d) the vendor catch-up follows the endpoint that last answered a
0x4a (the winner 3:0x39) instead of the silent stack port, while
still re-checking the stack port every fifth look for a late
state 4.

One correction that fell out of the same snapshots: the earlier
note (run 052215's "next run") said the vendor traffic goes to
"the same port the stack's IMSDCM bind comes from".  Android's
imscap2 capture contradicts that -- in that one capture the
IMSDCM peer is 3:0x3e (fd 5 receives 0x21/0x20 there) and the
vendor 0x4a peer is 3:0x3b (fd 60), two different endpoints.  The
vendor control surface is the announced service 0x12 entry (the
same one our 0x48/0x3d/0x3c already aim at), which is why
3:0x39 answered the scan and the IMSDCM asker never did; on
Android the port number differs per boot, so only the role
matters.

## qcril's own IMS set-up (qcrild.trace) — replayed from this run on

The qcrild trace's fd 21 talks to the announced 0x12 IMS service
entry (3:0x3c in that boot) and opens it with six requests that
HakiOS's driver has never sent, each answered result 0 with no
error:

  0x54  TLV 0x01 = 1
  0x01  TLVs 0x14 = 1, 0x17 = 1, 0x19 = 1 (the modem also raises
        indication 0x01 carrying 0x19/0x1a/0x1b/0x1c/0x1d/0x1f/0x20)
  0x03  TLV 0x12 = 1
  0x20  no TLV; the answer carries a 4-byte TLV 0x10 = 4 (a
        different field from the 1-byte state 0x4a carries) plus
        the stack's chain descriptions (0x13/0x15/0x1a/0x1e ...)
  0x5e  TLV 0x10 = 1
  0x69  TLV 0x11 = "MIUIOS3.0.10.0.WPACNXM"

Getters follow (0x2d state 0, 0x21 "QUALCOMM INCORPORATED", 0x53,
0x51 build strings, 0x25), indications 0x71/0x72 arrive, then
0x73 (TLV 0x10 = 0, 0x11 = 2, 0x12 = 0xb58, 0x13 = 0x3f, 0x14 = 2)
and 0x74 (TLV 0x01 = 2), also result 0.

Two captures pin the 0x4a state down: imscap2's fd 60 (3:0x3b)
reads TLV 0x10 as one byte, 00 before the address handoff and 04
after it; qcrild.trace's fd 78 (3:0x3d) reads the same one byte
as 00 on all four polls of its capture, which ends before the
transition.  0x20's 4-byte TLV 0x10 = 4 is a separate field and
must not be taken for the vendor state.

Every HakiOS run reads vendor state 1 at 0x4a and its IMSA
registration is refused with error 808; on Android the same modem
sits at state 4.  This trace is the only observed difference: the
AP's qcril performs these steps first, and HakiOS has no qcril.
`mhi_ims_qcril_prelude` (hakios.thyme_pcie2.c; called once a boot
from `mhi_ims_enable` after the bind and the supported-messages
probe, before the 0x4a read) replays 0x54/0x01/0x03/0x20/0x5e/0x69
with the capture's exact TLVs and logs each result, so the next
run shows whether the vendor state moves to 4 and whether the
0x3d/0x3c routes follow.  When any step is refused or unanswered
the replay is retried, up to three times a boot, and the later
attempts follow the endpoint the vendor catch-up has settled on
(the instance that answers 0x4a): the announced 0x12 entry and the
vendor service's own port are different QRTR endpoints in every
capture, so the set-up may belong on the instance the rest of the
vendor sequence uses.  The getters and 0x73/0x74 are not replayed
yet: they only read state, and no run has needed them.

## Run 093352: the six-message set-up is DMS's start-up, not IMS (2026-11-25 09:33)

The run above had replayed the prelude; this one says what it was.
Its fresh IMS client started the captured V6 leg (the V4 shape went
out first and was refused error 14) and got the IMS PDN on mux 4,
family 6, `240a:42e6:0810:291a:b17e:8cd6:2e62:420e/64`, DNS
`2409:8080:2000:3::1` and `::2`.  The supported-messages bitmap the
IMS service (0x12) then reported is 22 bytes, and it carries
0x3c **0**, 0x3d **0**, 0x48 1, 0x4a **0**, 0x8f 1, 0x90 1,
0x98 1: the announced 0x12 service does not implement the vendor
messages at all, which is the same split imscap2's socket table
showed (the vendor surface is a different QRTR endpoint).

The prelude's own results settle what it is.  Sent to the vendor
port (3:0x49 in this boot) the six answered result 1 with errors
58, 57, 57, 3, 0, 1; sent to the stack's own port (3:0x3b) every
attempt went unanswered -- six 5 s timeouts, three attempts,
about 2200 log lines, and the catch-up paid that again on every
poll before reading the state.  The six are qcrild's DMS start-up
(service 2): the same TLV shapes open DMS in the trace, and the
0x69 version string is the DMS version handover.  They were never
IMS traffic.  `mhi_ims_qcril_prelude` and its two call sites are
removed; the vendor set-up that stays is `mhi_ims_vendor_prelude`
(0x3d/0x3c, written only once 0x4a answers 4).

The rest of the run reproduced the earlier arc: IMSDCM bind, the
0x20 ask, the 78-byte address indication and the stack's 0x26
echoes all went through; IMSA registration still went 1 -> 0 with
error 808 (`28 03`), the 10099 dial was refused result 1 / error
48, and the SMS submit got no answer -- all downstream of the
registration.

One posture difference was found in the same log.  Every WDS
bind in the run -- the early pair, all four survey clients, the
fresh client 0x1005 and the five "after the boot" repeats --
carried subscription **1**, while this source's default
(`mhi_wds_card_sub`) reads 2.  The run's kernel was built before
the default was last set; the rebuild checked the object: the
symbol now sits in `__DATA,__data` with the value 2, so the next
run's binds say what actually goes on the wire.  The boot also
logs the posture once:

    pcie2: cellular: the WDS clients bind subscription %u this boot
    (UIM switch %s, pcie2_cardsub %d)

`pcie2_cardsub` forces 1 or 2.  The experiment the next run makes
is Android's own posture: `pcie2_uiswitch=0` keeps the card in
the slot the modem found it (slot 2, subscription 2) and skips
Switch Slot and Change Provisioning Session, so the IMS PDN is
started on subscription 2 and the modem's own IMSDCM instance and
ask words can match Android's (instance 2, family 1, 0x12 = 2,
0x13 = 2) instead of the post-switch 1 / 0 / 1 / 1.  That is the
last structural difference between HakiOS's bring-up and the
working baseline; whether the 808 clears under it is what the run
answers.  `.scratch/run-posture.sh` waits for the phone and
launches the run at battery >= 35% with the 093352 args plus
`pcie2_uiswitch=0`; below 35% nothing runs and a loader/fastboot
phone is nudged to Android at most once every 30 minutes.

## The vendor endpoint is 0x2a, and the WMS winner hid it (2026-11-25 11:16)

The 093352 probe did ask service 0x2a, but not the way Android
does, and the winner rule then threw its answer away.  Both halves
are now pinned down.

**Android's settings client talks to 0x2a instance 1.**  Pairing
the QRTR name-service lookups with their 20-byte replies in
`qcrild.trace` (`/tmp/ns_lookup.py`: sendto to `fe ff ff ff`,
payload word 0 = 10, answered by a recvfrom with word 0 = 4) gives
`fd 39/46/99/102 asked 0x2a/1 answer node 3 port 0x3b`.  The IMS
settings client in `imscap2.trace` (fd 60, through qmi-proxy) does
**all** of its vendor traffic on 3:0x3b: the bare 0x4a poll every
~1 s (answer TLV 0x10 = state, 00 before the handoff, 04 at
06:17:33.955 ~1.5 s after it), the 0x48 policy read, 0x3d
(TLV 0x01 = 01 + the 37-byte table) and 0x3c (TLV 0x01 = 01 01).
That is the same endpoint, so the vendor messages are 0x2a's, not
WMS's.

**qcrild's fresh-client opening on 0x2a** (08:17:50.502 -> .526,
every answer result 0): 0x27 with TLV 0x01 length 4 =
`00 00 00 01`, then 0x38 with TLV 0x15 = 1, 0x18 = 1, 0x1a = 1;
then a bare 0x24 returns 436 bytes whose TLV 0x11 lists the APN
profiles (cbnet, CMNET, CMWAP, ims, SOS).  The driver's existing
`mhi_dsd_settings` stream already replays this opening from its
own clients, which is why 0x2a is called the DSD settings service
there; what the IMS probe lacked was the same opening on **its**
client.

**What 093352 did instead.**  The probe introduced each client by
a name-service lookup, then sent the WMS-shaped 0x4c bind
(TLV 0x01 = 00) and a bare 0x4a to every candidate.  0x2a's bind
was accepted (result 0) and its 0x4a was never answered -- the
bind result 0 did not make the probe's client one the service
serves.  WMS (0x05) did answer 0x4a, with its own semantics
(state 1, never moving), and `mhi_ims_probe_note` picked winners
from the bind with `service == 0x05` hard-coded, so every vendor
message after the probe -- the prelude, the 0x48 read, 0x3d/0x3c
-- went to 3:0x39, WMS, and the state never reached 4.  The
"vendor configuration is withheld: the 0x4a state is 1" line was
this bug, not a modem refusal.

**The change.**  In `mhi_ims_vendor_scan` the 0x2a candidate is
now opened qcrild's way -- 0x27 (TLV 0x01 length 4 = 00 00 00 01),
0x38 (TLV 0x15/0x18/0x1a = 1), then the 0x24 identity read whose
answer says how many bytes of profiles came back -- from the
probe's own client, and the WMS-shaped 0x4c is skipped for 0x2a
(0x4c is not one of its messages).  `mhi_ims_probe_note` prefers a
0x2a 0x4a answer over any winner taken earlier, the WMS bind
included, and the WMS prelude now runs only when the winner really
is service 0x05 (`vendor_state != 4 && !on_stack &&
winner.service == 0x05`).  The rest of the sequence -- 0x48 at
state 4, 0x3d/0x3c -- is unchanged and will now go to 0x2a, which
is exactly where Android sends it.

The catch-up poll had the same blind spot: while no endpoint had
answered yet (`answered_port == 0`) it asked the IMS stack's own
client port on **every** look, and run 093352's stack port never
answered a 0x4a -- so the endpoint that does answer (the probe's
winner) was never polled and a move to state 4 could not be seen.
It now follows the endpoint that answered last and asks the
other one every fifth look; before any answer it prefers the
winner and gives the stack port the every-fifth look.  With 0x2a
as the winner, the state that turns 4 after the IMS PDN is read
from the service Android reads it from.

**IMSA error 808 is not the register's shape.**  Android's
register on 3:0x49 (qcrild fd 69) is message 0x0022 with eight
one-byte TLVs 0x10/0x11/0x12/0x13/0x19/0x1b/0x1e/0x1f = 1, then
0x0033 with TLV 0x10 = 00 00 00 00; `mhi_imsa_wait` already sends
those (plus a second 0x22 with TLV 0x18 = 1).  The remaining
suspect for 808 is the subscription posture, which is what the
queued `pcie2_uiswitch=0` run changes.

The watcher's gate is now battery >= 35% (the phone died at 1%
at 11:13 and waits for charging; reboots of a loader/fastboot
phone are at most one per 30 minutes), and the kernel with this
change is built (`hakios-kernel`, 11:16) and exported to
`devices/thyme/drivers/modem/`.

## The next run's diagnostics: 0x23's own words, 0x2d's every TLV (2026-11-25 11:57)

Error 808 (0x0328) still has no public meaning, and this firmware
has never sent the one field libqmi's IMSA IDL names as its
explanation: `qmi-service-imsa.json` (1.38.0-1, fetched from
sources.debian.org) makes TLV 0x10 of the 0x23 indication a raw
guint16 error code and TLV 0x12 an optional **string**, "IMS
Registration Error Message".  `mhi_imsa_note` now reads that
string (printable bytes, at most 47) and logs it whenever the
indication carries it, and on any error != 0 it dumps the whole
0x23 QMI payload (64 bytes, two hex lines) -- so the next failed
registration's packet is readable in the log even if the reason
sits in a TLV id this vendor firmware added.

The two Get Current Settings answers (0x2d: the fresh client's
after its start, and the started call's) are now also run through
`mhi_qmi_tlvs`: every TLV id, size and first twelve bytes.  The
driver reads only 0x26/0x27/0x28/0x2b (and 0x1e for V4), and the
run log has never shown whether this modem volunteers a P-CSCF
(TLV 0x2a in libqmi's WDS IDL) or PCO companion in the same
answer; Android's IMS stack finds its P-CSCF through the IMS APN,
and if the answer carries one, handing it on is a candidate the
808 investigation has not had yet.

Built 11:57: `hakios-kernel` md5 `0d457ec05dbf6d7fcb1d1e10051513fd`,
exported to `devices/thyme/drivers/modem/` (both copies identical).
The 0x2a-endpoint fix from 11:16 is in this kernel too -- its
11:23 build was never run on the phone (the phone died at 1% at
11:13 and has been waiting for a charge since; at 11:50 adb said
status 3, USB powered false, so it is not on a charger).  The
watcher (35% gate) still holds the queued posture run:
`pcie2_uiswitch=0 pcie2_imsdcm_keep_ask=1 pcie2_ims_v4=1
pcie2_ims_f0v4=1`.

## Android's own 0x4a timeline, decoded (2026-11-25 12:25)

The imscap2 capture settles how the vendor state reaches 4.
Android's settings client (fd 60) sends its **first** frame at
06:17:33.955 -- the bare 0x4a, txn 0x30 -- and the answer at
33.967 carries result 0 and TLV 0x10 = 04: state 4, only 1.40 s
after the IMSDCM address handoff (32.562).  No 0x5c/0x32/0x47
prelude precedes it from that client, so the state climbs **by
itself** once the modem's IMS stack has taken the handoff; the
host's only job is the handoff.  After it: 0x48 at 33.970, the
44-byte 0x3d at 34.023, repeated 0x4a/0x48 pairs, 0x3c at
36.336 (TLV 0x01 = 01 01), then the other 0x3d shape (TLV 0x01 =
00, TLV 0x11, 27 bytes) at 37.954 and 38.459.

A different Android client (fd 37) was refused 0x4a with error
0x34 (52, DeviceNotReady) at 33.037 -- the same refusal our bare
probes got from unregistered clients -- and at 36.148 sent 0x4a
with TLV 0x01 = 00 00 01, answered result 0 but with **no** state
TLV.

For HakiOS this pins the remaining difference: run 093352 did
hand the address over (V6, family word 1, profile 100/101) and
its 0x4a still read state 1, where Android's read 4 within 1.4 s.
The one structural difference left is the IMSDCM instance word
(ours 1, Android's 2) and the ask family it induces (ours
0/1/1, Android's 1/2/2) -- exactly what the queued
`pcie2_uiswitch=0` changes.  For the full 0x2d dump, libqmi's
WDS IDL puts the P-CSCF in TLV 0x22 (using PCO), 0x23 (server
address list) and 0x24 (domain name list); 0x2a is the domain
name list and 0x2c the IMCN flag.

## The call experiment's later attempt (2026-11-25 12:36)

The milestone's dial goes out immediately after the IMS stage, and
the modem's registration can land after that pass: the vendor state
must reach 4 and 0x3d/0x3c follow before the stack registers.  (In a
boot without WMS the milestone never reaches its dial at all.)  So
`mhi_voice_retry` now runs each cellular cycle after the vendor
catch-up: when `_pcie2_call` is armed and the first attempt was
placed unregistered and never got as far as ringing or talking
(new `voice_call_progressed`, set by the 0x2e states 3 conversation,
5 alerting, 6 hold, 7 waiting), and the modem's own 0x23 indication
says registered (2), it confirms the state with one IMSA getter and
places the call once more from the registered state, marking the
experiment done so the milestone never dials twice.

The arming is parsed by its own `mhi_voice_armed` (not the dial's
static number) so the retry can tell in a boot where the milestone
returns before its dial step.

Built 12:39: `hakios-kernel` md5 `96227bbad19b44dc15017ff7c7722ec8`,
the driver exported to `devices/thyme/drivers/modem/` (both copies
md5 `2847e463db52c0652a3990e1411b47d8`).  This kernel carries the
0x2a-endpoint fix and the new 0x23/0x2d diagnostics as well, and is
what the queued run will boot once the battery holds >= 35 %.

## Run 205643: the standard posture registers, the vendor state never reaches 4, and the stack that never binds (2026-11-25 21:30)

The evening's `pcie2_uiswitch=0` posture never registered at all (no
operator in the status bar ten minutes in), so the 20:56 run went back
to the standard posture -- no `XL_EXTRA_ARGS`, kernel md5
`2c8d005a1f13bbbb3719aa1ffc6a17d3`.  LTE registers and the signal
indicator appears one to two minutes after boot; everything above the
NAS layer stayed where run 093352 left it.

**The PDN is up.**  The default cbnet PDN (family 6, mux 3) is
accepted, and the daemon-shaped fresh client (0x1005, mux 4, family 6)
gets "THE FRESH CLIENT'S START WAS ACCEPTED (client 0x1005, mux 4,
family 6, packet data handle 999853616)"; its leg carries
`ipv6 240a:42e6:0810:6f36:41e8:8326:690f:c695/64`, DNS
`2409:8080:2000:0003::1`, and packet service 2 (connected).  The
address is held for the IMS data service.

**IMSA.**  Bind 0x33 (TLV 0x10 = 00) result 0, register 0x22 in
Android's shape result 0.  Get 0x20 answers registration 0 (TLV 0x10 =
00, TLV 0x11 = 0); Get 0x21 answers TLV 0x16 = 2 (SMS) and TLV 0x17 = 1
(voice).  The modem's own 0x23 indications arrive late and say
registration 0, technology 1, error 0 then **999**; the dumped payload
is `04 08 00 23 00 17 00 01 01 00 00 10 02 00 e7 03 11 04 00 00 00 00
00 13 04 00 01 00 00 00` -- TLV 0x10 = 0x03e7 (999), so this boot's
failure is not the 808 of the earlier 2026-11-25 runs.  Two 0x39
indications (TLV 0x01 = 00, then 01) are messages the driver still
does not read.

**The vendor state.**  The scan asks 44 of the 49 announced services;
the winner is WMS (0x05 instance 1) at 3:0x3b, whose 0x4c bind is
accepted and whose 0x4a reads error 52 in every round.  The 0x2a
endpoint answers 0x27 with error 48, 0x38 and 0x24 with result 0, and
0x4a with error 57.  The prelude (0x5c, 0x32, 0x01, 0x47, 0x4d, 0x5c)
is accepted, but 0x48 and 0x4a stay 52: "the vendor configuration is
withheld: the 0x4a state is 4294967295, not 4".  0x1f reads ids
0x8f/0xa2 result 0 and the Set Config 0x2a result 0.

**The call.**  "THE DIAL TO 10099 says result 1, error 48 (invalid
argument); no call id came back", while the modem's own 0x2e
indication names call id 1, state 4 (CC in progress), type 0 (voice),
direction 1 (MO), mode 6: the modem starts the call and never
advances.  The 60 s watch records "the refused call's watch is over:
it stayed in CC set-up; the call is still up", and "THE END CALL OF 1
(from the indication) says result 1, error 41 (error)".

**The deadlock underneath: the stack never binds.**  Only ~15 minutes
after boot did the modem's IMS stack (node 3 port 0x3d) first write to
the host's IMS data service (port 0x3005), and its conversation was
0x33 SUB_DESTROY_INSTANCE (txn 10, 12) then 0x20, the APN ask (txn 11,
13) -- never the 0x21 bind Android's stack sends first.  imscap2
(06:17) pins the reference order: the stack binds at 21.452 (0x21, TLV
0x01 = 0x15, TLV 0x10 = instance 2); the daemon answers 0x21 with 0xfa
and instance 2 eleven milliseconds later; the ready follows 143 ms
after the answer with profile 0x64 and instance 2; the ask comes 10.4
s after the bind, carrying the APN "ims" in TLV 0x01, profile 0x65 and
words 1/2/2; the address handoff 533 ms after the ask's answer is TLV
0x11 = family word 1 + the 37-character V6 text.  Our ask's words are
0/1/1 (the stack thinks V4), and `mhi_imsdcm_indicate`'s gate (address
&& asked && bound && ready_sent) withheld the address all boot because
bound was never set.

Kernel `e7d3930e07bfe8a8268591b9e9ae280e` (driver
`c12f45fb983a6a27a5b349a6d27499f9`) adds `pcie2_imsdcm_nobind=1`: an
ask that arrives without a bind sends the ready (handle 0x15, instance
2 -- Android's words) and then the address, in the reference's own
shapes.  Off by default; the next run arms it.

## Run 213235: the stack binds by itself and asks V4; a faithful V6 handoff still leaves registration at 0 (2026-11-25 21:32)

The nobind arm boots with `pcie2_imsdcm_nobind=1`, kernel
`e7d3930e07bfe8a8268591b9e9ae280e` (driver
`c12f45fb983a6a27a5b349a6d27499f9`), battery 67% at start.

**The PDN is up.**  The fresh client (0x1005, mux 4, family 6) start
is accepted: "THE FRESH CLIENT'S START WAS ACCEPTED (client 0x1005,
mux 4, family 6, packet data handle 3868786848)"; its leg carries
`ipv6 240a:42e6:0810:172b:a450:e58b:0f41:5ead/64` (the handoff text
prints it unpadded, `240a:42e6:810:172b:a450:e58b:f41:5ead`), DNS
`2409:8080:2000:0003::1`, and packet service 2 (connected).

**The stack binds by itself.**  Unlike the previous run, this boot's
stack (node 3 port 0x3d) sent its own bind: 0x21 transaction 10, TLV
0x01 = 0x15, TLV 0x10 = instance 1 (Android's binds the same word with
instance 2).  The answer echoes instance 1 with 0xfa, the ready
follows with profile 0x64 and instance 1, and then the ask: 0x20
transaction 11, TLV 0x01 (20 bytes) = `03 "ims" 00 ...`, TLV 0x10 =
0x65, and words 0x11/0x12/0x13 = **0/1/1** -- "the ask's words: 0x11 0
(the family: 0 V4, 1 V6), 0x12 1, 0x13 1; Android's working ask has 1,
2, 2".

**The handoff is Android's shape.**  The ask's answer echoes handle
0x15, profile 0x65 and 0x12; 530 ms later the address indication goes
out (transaction 7, 77 bytes) with TLV 0x11 = `01 00 00 00` (family
word 1), `0x25`, and the 37-character V6 text -- byte-for-byte
imscap2's handoff.  The PDN-up path had already handed profile 100
(the default) 0.53 s after the leg connected; the boot's first ask
predates the window on this snapshot.  The stack's answer to every
handoff is a fresh bind (transaction 12, word 0x15, instance 1) and the
same ask again (words 0/1/1): it never takes the V6 address.

**The vendor state and the call.**  The 0x48 read says state 0; the
0x4a reads (rounds 0 and 1) say state 1 -- up from the error-52
answers of run 205643, but still not 4: "the vendor configuration is
withheld: the 0x4a state is 1, not 4 (asked 3:0x39)".  IMSA:
registration 1 then 0, technology 1, error 808; the registration
getter says 0, SMS 2, voice 1.  "THE DIAL TO 10099 says result 1,
error 48"; the modem's own 0x2e indication names call id 1, state 4
(CC in progress); the 60 s watch records "it stayed in CC set-up; the
call is still up"; End Call answers result 1, error 41.

**What differs from Android.**  imscap2's ask is 1/2/2 with instance
2; ours is 0/1/1 with instance 1.  The family set is internally
consistent (V6 -> 1/2/2, V4 -> 0/1/1), so the family word is the
stack's own configuration speaking, not something the daemon sets.
Android's daemon answered with a V6 address because the stack asked
V6; ours is handed V6 against a V4 ask and never leaves state 0.
Kernel `e7d3930e` already carries `pcie2_ims_f0v4` (leg f0 requests
family 4 and reads TLV 0x1e as dotted-quad text) and
`pcie2_imsdcm_ask_family`; the next run arms both, so the stack gets a
real V4 address when the network grants one on the "ims" APN, and a
refused V4 start still isolates whether the word alone or the address
is what the stack checks.

## Run 220741 (f0v4 arm): the network refuses the V4 IMS start, and a word-0 handoff of the V6 text still leaves registration at 0 (2026-11-25 22:07, stopped at the user's request)

The f0v4 boot runs `pcie2_ims_f0v4=1 pcie2_imsdcm_nobind=1 pcie2_imsdcm_ask_family=1`
on kernel `e7d3930e07bfe8a8268591b9e9ae280e`.

- The driver's own "ims" start in its family-4 form is refused: result 1,
  error 14, verbose reason type 2 / 208 ("the family-4 form of the IMS
  data service's own start is refused").  The set-ip-family 4 before it
  is accepted (result 0), so the refusal is the answer to an IPv4 IMS
  PDN, not the modem refusing the request shape.
- The fresh client's family-6 start is accepted as before (client
  0x1005, mux 4, handle 3784908912, address
  `240a:42ea:2012:4aa7:9c06:e47b:b62a:7af0`, 39 characters).
- With `pcie2_imsdcm_ask_family=1` the PDN-up handoff carries family
  word 0 with that V6 text (TLV 0x11 = `00 00 00 00`, `0x25`, 39
  characters), and the vendor-state path repeats it under profile 101.
  The ask words this boot are still 0/1/1, and this boot the stack
  asked without binding at all ("no 0x21 this boot").
- IMSA still goes 1 then 0 with error 808; the 0x48 state is 0; the
  registration getter says 0, SMS 2, voice 1.  The run was stopped
  before the 10099 dial.

What this means: the word alone is not the stack's check -- a V6
address under word 0 changes nothing -- and the family the stack asks
for (V4) is one the network will not grant on "ims" (error 14).  The
next lever is the stack's ask itself: what makes Android's stack ask
V6 (1/2/2) while this one asks V4 (0/1/1), rather than satisfying a V4
ask the network refuses.

### THYME SDX55M CommCenter Bridge, 4G LTE Data & Two-Way VoLTE RTP Media (2026-11-26)

- **Bridge & SIM provisioning**: `devices/thyme/drivers/modem/hakios.thyme_baseband.cpp` bridges Apple's `AppleBaseband` / `ApplePDPHelper` / `pdp_ip0..3` stack onto THYME's PCIe2/MHI SDX55M transport (`hakios.thyme_pcie2.c`), auto-switching physical SIM slot 2 to logical slot 1 (`UIM 0x0046`/`0x0038`), arming Qualcomm DSD (`0x2a`: `0x0065`/`0x0069`), and injecting `WDS 0x00af=1` immediately after `WDS 0x00a2` (`qcrild` order).
- **China Broadnet (`46015`)**: `sysimage/select-system.py` symlinks Apple-signed `CMCC_cn.bundle` for `46015` (so Settings -> Cellular renders native rows under `中国广电`), and `hakios.thyme_baseband.cpp` rewrites `cmnet` -> `cbnet` in-place on WDS APN TLVs.
- **Two-way VoLTE RTP**: `umi_ims.inc` publishes `IMSRTP` (`0x28`) `INIT`/`CONFIG`/`START`/`STOP`/`UNINIT`/`DTMF` events to `/dev/hakios-imsrtp`. Because `CommCenter` holds a placeholder UDP socket on `local_port` (`49120`), `/dev/hakios-imsrtp-data` taps inbound `pdp_ip1` (`mux 2`) RTP packets directly in the kernel (passing RTCP `local_port+1` untouched) and injects outbound IPv6/IPv4 UDP RTP/RTCP packets directly into `bb_pdp_output(pdp_ip1)`. `userland/src/volte_rtp.c` (`com.hakios.volte-rtp.plist`, `ProcessType=Interactive`, built with Apache-2.0 AMR-NB/AMR-WB static libs via `tools/mk-amr.sh`) waits for the QCI-1 RX gate (`rx_pkts >= 5`), decodes RFC 4867 AMR-NB/AMR-WB to `/dev/hakios-audio` (48 kHz stereo -> CS35L41 speakers), and encodes `/dev/hakios-mic` (48 kHz mono) back over `pdp_ip1`.
