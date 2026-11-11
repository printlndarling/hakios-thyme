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
