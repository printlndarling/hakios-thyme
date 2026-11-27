## Chronological Verification Logs & Evidence

## Native app and daily-use audit

- The camera kernel driver has S5K4H7 (ultra-wide) and OV02B10 (macro)
  profiles and a raw sensor cycle scene. The AVFoundation bridge in
  `userland/src/camera_device.c` creates two streams, rear main and
  front. The bridge accepts `ZoomFactor` and records the ISP-applied
  crop ratio in capture metadata. Native auxiliary lens controls,
  gestures, zoom dial and saved framing still require verification.
- The prior cellular run used one SIM in physical slot 2 mapped to
  logical subscription 1, with LTE data and two-way VoLTE. The WDS
  bridge explicitly binds to that primary subscription. That test
  deferred SMS. Dual active lines, switching defaults, cellular data
  controls, airplane mode recovery and real Messages delivery have
  separate acceptance steps.
- The historical manual brightness sweep tested 265 to 971 permille
  and lock blanking. Live front and rear ALS streams alone leave the
  lowest visible setting, low-light stability and auto-brightness open.
- The ringer state listener and ADSP playback/capture paths give
  coverage for hardware audio. System HUD, sound effects, ringtones,
  Messages alerts and silent behavior need their own system tests.
  Simplified Pinyin is configured in the preferences seed. The
  existing Voice Memos scene reaches recording. Candidate selection
  and switching keyboards, and the full stop/save/reopen/play cycle
  still require native app acceptance.

## Wi-Fi shutdown during CDSP diagnostics

- In `logs/113128-run.log`, the QMI driver event worker reached
  `ath11k_core_pre_reconfigure_recovery` after radio teardown started.
  Shutdown now sets ath11k's unregistering flag and drains queued QMI and
  recovery work before dismantling the radios.
- The 90 s `cdsp=4` retest with kernel `b2adc35dd11c` and root
  `7d8ba86fdb50` completed the bidirectional FastRPC channel open and
  shut down Wi-Fi and CDSP. The log still contains a WLAN mode-off
  timeout. The watchdog quiesced, the loader reported `1441823 0`, and
  the phone returned to Android. The previous panic did not recur.
  Local evidence: `logs/143946-run.log`, SHA-256
  `8ad4888b40c879a9c39b8e2693f9ace924d82e477f092eaff0d27cbf8822bd79`.

## CDSP GLINK transport

- Thyme now allocates the AP-owned GLINK transmit ring in the CDSP-private
  SMEM partition. Its queue, indices and IPCC client are independent from
  SLPI's. An active inherited queue is left untouched. The probe follows
  [Qualcomm GLINK's version negotiation](https://raw.githubusercontent.com/torvalds/linux/master/drivers/rpmsg/qcom_glink_native.c)
  and validates ring bounds and alignment before accessing packets.
- The physical Mi 10S's 90 s `cdsp=3` run with kernel `87bb69f69db9`
  and root `7d8ba86fdb50` found both 16 KiB rings. The AP sent GLINK
  version 1 with optional features zero. The DSP sent version 1 with
  features `0x7`; the AP acknowledged it, and the DSP returned
  `VERSION_ACK` 1 with zero features. Both directions completed.
  CDSP shut down and the watchdog quiesced before the kernel
  returned to the loader and rebooted to Android. Local evidence:
  `logs/112646-run.log`, SHA-256
  `f3be5b98a0650175c60a340e678453416161054d11bfb30e6123543a082266b7`.

- A second 90 s run, `cdsp=4` with kernel `8a7573d443b7` and root
  `7d8ba86fdb50`, repeated negotiation and opened
  `fastrpcglink-apps-dsp`. The AP used channel 1 and the DSP used channel 3.
  Both OPEN messages were acknowledged. The run exposed a Wi-Fi shutdown
  panic after CDSP had shut down. Local evidence: `logs/113128-run.log`,
  SHA-256 `2de66f5dce70b6b5a09a152f47be02ac09ce8cbf7bc25350e935a61e0884fe69`.
  The CDSP transport is ready for receive-intent handling and the first
  DSP invocation. HVX and Core ML acceleration remain open.

## CDSP reports ready over SMP2P

- The [SM8250 binding](https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/qcom/sm8250.dtsi)
  and the thyme DT agree on APSS/CDSP host IDs 0/5, inbound/outbound
  items 94/432 and SMP2P notification through IPCC client 6, signal 2.
  The probe reuses the existing SMEM parser and validates the processor IDs,
  version and bounded entry table before reading `slave-kernel`.
- The 90 s `cdsp=1` control (`d5788eee504b`) authenticated and shut the
  DSP down. Its two-second read-only observation found no valid inbound
  state. The run also logged a backboardd SIGSEGV after GPU teardown started
  and SpringBoard was relaunched. The second boot did not reproduce that
  exit. Local control evidence is `logs/111521-run.log`,
  SHA-256 `66c37382ecf4a67dae007ac1f3d2a11107186979cfeb7e34a64a568c642cd424`.
- The 90 s `cdsp=2` run (`6753dcaaaf1a`) published the AP-owned
  `master-kernel` entry before PAS, authenticated the CDSP and notified
  it over IPCC. The CDSP then created item 94 and reported
  `slave-kernel=0x6` (ready and proxy handover). In its 128 KiB private
  partition, a 32-byte GLINK descriptor and a 16 KiB DSP-to-AP queue
  appeared. The AP-to-DSP queue was still absent. GLINK version negotiation,
  FastRPC execution and application acceleration remain open.
- SpringBoard started, SLPI linked with eight services, both DSP shutdown
  paths completed, the watchdog quiesced, and the loader returned
  `1441823 0` and rebooted to Android. Local evidence is
  `logs/112001-run.log`, SHA-256
  `d98a7a9192c27e8796841cc15b1e80b570a6f44c2ea6f5a8b153fc7276986a42`.
  Nine inbound and five outbound host fixtures also passed. The outbound
  cases verify version-last publication and refusal to overwrite invalid
  inherited items.

## PAS relocation before split firmware load

- The [Qualcomm MDT loader](https://raw.githubusercontent.com/torvalds/linux/master/drivers/soc/qcom/mdt_loader.c)
  sets up the relocation range following image initialization and before
  writing firmware segments. Thyme now uses that order. It validates the
  ELF32 little-endian program-header table and the full memory extent
  before sending firmware to the secure monitor.
- Eight host cases exercised the actual loader code with stubbed hardware:
  a valid shifted program-header table, wrong byte order, out-of-range or
  truncated headers, invalid segment sizes, and relocation failure before
  the first carveout write. Local harness is `logs/codex-pas-layout-test.py`.
- The physical Mi 10S ran `venus=3 cdsp=1` for 90 s with kernel
  `01866010b7d4` and compact root `7d8ba86fdb50`. For both images, the
  log records successful `INIT_IMAGE` and `MEM_SETUP` before copying
  any segments. Venus copied 926,324 bytes across 14 loadable segments and
  CDSP copied 5,793,254 across 11. CDSP authenticated and shut down.
  SpringBoard ran, the watchdog quiesced, and Android booted after the
  loader returned `1441823 0`. No kernel panic appears in the final log.
  Local evidence is `logs/110825-run.log`, SHA-256
  `755b8da36a7d0d224f22f14a740967af84241f585cef25ebce15a055ef857dba`.
  Venus firmware execution and HFI remain pending.

## Venus core clock and RAM payload headroom

- The first `venus=3 cdsp=1` run (`b5087c04a781`) returned normally, but
  the clock test stopped before applying the RCG rate. VIDEO_CC AHB reported
  `0x0`: halt clear, software enable clear. Requiring the software vote on
  the inherited bus clock prevented the core test and gave a misleading
  restore failure. Log `105705-run.log`,
  SHA-256 `170ea2a26306bd1f0ed03090a20bbda82b36d84fb242ea8cc4b319d8e43a74ac`.
- The corrected 90 s run (`83e791adb999`) used the
  [SM8250 MVS0 RCG crystal entry](https://raw.githubusercontent.com/torvalds/linux/master/drivers/clk/qcom/videocc-sm8250.c)
  with the inherited divider of two. MVS0C moved from `CBCR=0x80000220`
  to `0x221`, then returned to `0x80000220`. RCG CFG moved from
  `0x100000` to `0x100001` and back, and GCC AHB/XO restored to
  `0x80000000`. The expected core rate is 9.6 MHz from the crystal and
  divider configuration. The test verified the hardware branch state.
- The same run used the 48,467,968-byte root image with 128 free APFS blocks
  (SHA-256 `7d8ba86fdb501b832356476bb1f082f60d55e235b22e59ff2ee4c320f6db50b3`).
  Both root variants were clean in `apfsck -cw`, and an independent reader
  matched all 992 entries against the staging tree in each image. The compact
  image saved 524,288 bytes, leaving 534,976 bytes in the loader's payload
  window. SpringBoard ran, CDSP shut down, the watchdog quiesced, and the
  loader returned `1441823 0` and rebooted to Android without a kernel panic.
  Log `110144-run.log`,
  SHA-256 `eddfa3076e210f23c922417d0f3c755284925133781eb0db98c3c429b5092228`.
- Venus firmware execution and HFI remain open. For the PAS path, the
  [vendor Kona clock driver](https://android.googlesource.com/kernel/msm.git/+/48a207df39b112b995317644ca051838d6f93617/drivers/clk/qcom/videocc-kona.c)
  couples the PLL-backed core rate to MMCX voltage. Check the stock PLL,
  MMCX/MX, and PIL interconnect votes before retrying `AUTH_AND_RESET`.

## Cross-platform RAM launcher validation

- On native Windows, `ram-boot.bat` and Python transferred the local
  four-file bundle to a physical Mi 10S and reached the running SpringBoard
  service. The loader returned `1441823 0` after `oem stop` and accepted the
  reboot to Android. Local evidence is `logs/codex-native-windows-thyme.log`
  (SHA-256 `d9e9c1f88652fb290d1667041302274ff15a1b3185af7916aa8f7e94f61fb4fb`).
- The Linux shell wrapper passed `--check` and the generated kernel payload was
  66,033,064 bytes. The Windows and Linux downloads of Google Platform-Tools
  ran their version commands on the respective hosts. The macOS download
  contained universal x86_64/arm64 executables. Linux and macOS USB boot
  were not exercised on native hosts.
- Host fixtures verified fresh manifest downloads, cache reuse, and rejection
  of mismatched hashes and another device's manifest. The project has no
  public release containing the complete thyme bundle.

## Venus power probe follow-up

- The inherited `venus=2` experiment entered the PAS authentication path
  after enabling the GCC AHB and XO branches and setting MVS0C GDSC
  `PWR_ON`. Its live log stopped after successful `TZ MEM_SETUP` without an
  authentication result or a normal return. Local evidence is
  `logs/090135-run.log.live` (SHA-256
  `1c02b8e31592aa1445eec8e57c52140ad57ee71339f7dc2a8ff0d478ef45b396`).
- The 90 s `venus=1 cdsp=1` RAM control returned `1441823 0` and rebooted to
  Android. It recorded the Venus 14-segment load and `MEM_SETUP`, the CDSP
  `AUTH_AND_RESET` and `SHUTDOWN`, and watchdog quiescence with 23 pets.
  Kernel prefix `cd013feee0ae`. Local evidence is `logs/103533-run.log`
  (SHA-256 `6c4200b18c355662fe52d923c12aaf2e486533bd73c58b4c2b9ac51d9f82bbf8`).
- The first isolated power-probe build (`83027f04d3ca`) read a halted
  MVS0C core branch (`CBCR=0x80000220`), then panicked as it tried to free a
  permanent MMIO mapping. That run had a loader return record, so the
  return record alone cannot be used as a pass condition. Local evidence
  is `logs/104019-run.log` (SHA-256
  `514f44cb08112effb294479092672a290e6b047380973607e337c9bdc0ad161f`).
  Temporary probes use page-aligned, explicitly unmap-capable MMIO mappings.
- The stock thyme `qcom,venus@aab0000` node describes PAS ID 9, the same
  reserved-memory carveout, a vdd proxy through `mvs0c_gdsc` with an
  MMCX parent, XO/core/AHB clocks, and an interconnect bandwidth vote.
  The [SM8250 video clock driver](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/videocc-sm8250.c)
  identifies the MVS0C core CBCR at offset `0xc34`. Reaching the GDSC
  power-on state leaves core clock sequencing and interconnect votes to
  validate before attempting PAS authentication again.

- The corrected 90 s `venus=2 cdsp=1` RAM run used kernel `e460dd7666e0`.
  MVS0C moved from `0x226001` to `0xf8226000` (PWR_ON) and then
  `0x70226001` (PWR_ON cleared, software collapse restored). AHB and XO
  both went from `0x80000000` to `0x1` and back. The core branch still
  reported `0x80000220` (halted). Venus completed `TZ MEM_SETUP` for its
  14 segments. CDSP authenticated and shut down, and the watchdog quiesced
  after 23 pets. The loader returned `1441823 0` and rebooted to Android.
  No kernel panic is present in the final log. Local evidence is
  `logs/104323-run.log` (SHA-256
  `67e0ba3b386bd9d0d380b3501408ce729b69a12c5f4016cda0cf97024c7f695c`).
  The controlled power probe passed. Venus firmware execution and HFI
  encode/decode remain partial.

## Batch 11 Autonomous Audit (2026-11-27, `logs/085419-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `bd69fa3dbeed`, `result: returned 1441823 0`):

- **Hexagon CDSP (`pas-id=0x12`, `0x8d400000`) Full TrustZone PIL-TZ Boot & Shutdown (`PASS`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Built generalized `thyme_pil_boot_one()` loading split ELF firmware directly from UFS `modem_a/image/` via `hakios_modem_fat_read()` + TrustZone `SCM_PAS_INIT_IMAGE` / `MEM_SETUP` / `AUTH_AND_RESET` / `SHUTDOWN`.
  - Live `cdsp=1` boot verified: `UMI cdsp: ELF segments=13 loadable=11 reloc=1 span=0x1400000 (base=0x87800000)`, `TZ INIT_IMAGE ret=0 ans=0`, `segments loaded (5793254 bytes total)`, `TZ MEM_SETUP ret=0 ans=0`, **`TZ AUTH_AND_RESET ret=0 ans=0`**, and clean exit **`TZ SHUTDOWN ret=0 ans=0`** (`kern.hakios.cdsp_ready=1`).
- **Venus VPU (`pas-id=9`, `0x8ca00000`) TrustZone Header/Memory Auth & Clock Diagnosis (`PARTIAL`)**:
  - Live `venus=1` verified `UMI venus: ELF segments=20 loadable=14 reloc=1 span=0x500000 (base=0xf500000)`, `TZ INIT_IMAGE ret=0 ans=0`, `segments loaded (926324 bytes total)`, `TZ MEM_SETUP ret=0 ans=0`, and diagnosed gated Video CC buses (`UMI venus: gcc_video ahb=0x80000000 xo=0x80000000`) explaining why `AUTH_AND_RESET(9)` requires enabling `GCC_VIDEO` clocks + `mmcx`/`mvsc` first.
- **Artifacts**: `hakios-kernel` (`17,040,232 B`, sha256 `bd69fa3dbeed0d89448183005dcaa3fedfb3b96b0dae8790f15ce35a05faa29f`), `userland/root.apfs` (`48,992,256 B`, sha256 `6393155cc2be9631641de8ec96657b232a117926121efd4ef70eee375ca74f99`; total `66,032,488 B < 66,060,288 B`), `logs/085419-run.log` (sha256 `0e678805a928b7ec5180464b8d29fccb57f04db27e745e530e67c0f91fd06cad`).

## Batch 10 Autonomous Audit (2026-11-27, `logs/083725-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `b40358b23b80`, `result: returned 1437936 0`):

- **VFAT LFN (`0x0f`) Firmware Loader Over `modem_a` (`venus.*` & `cdsp.*` Verified)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Added VFAT LFN decoder (`fat_lfn_feed()`, `fat_format_83()`) across sector/cluster boundaries inside `fat_find_image_dir()`, `hakios_modem_fat_read()`, and `thyme_modem_fat_scan()`.
  - Decoded `UMI ufs_fw: root[0] 'image' attr=0x10 clus=2` and all `514` VFAT firmware files directly on-device without consuming 63 MiB RAM rootfs budget:
    - `image/venus.mdt` (`8220 B`, `UMI ufs_fw: VENUS header (8220 B): 7f 45 4c 46 01 01 01 00`) + `venus.b00` (`692 B`), `venus.b01` (`7528 B`), `venus.b02..`
    - `image/cdsp.mdt` (`7660 B`, `UMI ufs_fw: CDSP header (7660 B): 7f 45 4c 46 01 01 01 00`) + `cdsp.b00` (`468 B`)..`cdsp.b11` (`59156 B`)
  - Exported `hakios_modem_fat_read(image_file, dst, max_len, out_len)` ready for TrustZone PIL authentication of `venus` (`pas-id=9`) and `cdsp` (`pas-id=0x12`), alongside `wdt=1` (`pets=16`, `UMI wdt: quiesced`).
- **Artifacts**: `hakios-kernel` (`17,039,280 B`, sha256 `b40358b23b80958106b46a3a57b390df6062e1cba1ff1348e2608f462f0ddbec`), `userland/root.apfs` (`48,992,256 B`, sha256 `6393155cc2be9631641de8ec96657b232a117926121efd4ef70eee375ca74f99`; total `66,031,536 B < 66,060,288 B`), `logs/083725-run.log` (sha256 `95eea1d00bdadb8ba5370b828060ff2f45c69e2dcc66f57bfdf066e493894737`).

## Batch 9 Autonomous Audit (2026-11-27, `logs/083127-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `367234565f2e`, `result: returned 1441823 0`):

- **`modem_a` Subdir Descent (`clus=2`, 514 VFAT LFN Firmware Segments)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Descended past `'PCAFUI~2   '` (`clus=2`) and enumerated `514` files with `.B00`..`.B07` extensions (`EPUZJI~2.B00` 692 B, `47MKHH~2.B01` 7528 B, `NK7HJZ~2.B02` 82424 B, `7T66QN~2.B03` 358784 B, `EU4PDT~2.B04` 1947772 B, `OJAB2Y~2.B05` 2487860 B, plus 2 subdirs `X5LAEE~2` clus=3 & `WMLSPW~2` clus=6). Confirmed Mi 10S stores firmware filenames in VFAT LFN (`attr=0x0f`) entries with `~2` hash-generated short names; adding VFAT LFN decoding next unlocks `venus.*` & `cdsp.*`.
- **`dsp_a` Ext4 Partition & `GCC_CE1` Clock Readback**:
  - Verified `dsp_a` superblock via `hakios_ufs_read_partition`: `UMI ufs_fw: dsp_a magic=0xef53 blocks=16384` (64 MiB ext4).
  - Read SM8250 `GCC_CE1` clock registers safely inside `thyme_qrng_init`: `UMI qce: gcc_ce1 cbcr=0x80004220/0/0x80000002/0x80000000` (clocks gated off in `0x41004` bit 31 until voted).
- **Artifacts**: `hakios-kernel` (`17,039,248 B`, sha256 `367234565f2e88e76a062863bece135a1c4175249ea04fdbf5cc57022faf8bca`), `userland/root.apfs` (`48,992,256 B`, sha256 `6393155cc2be9631641de8ec96657b232a117926121efd4ef70eee375ca74f99`; total `66,031,504 B < 66,060,288 B`), `logs/083127-run.log` (sha256 `53cedcd5148257031b4626cfa887ab2e5e6fb77efae1b043e7f9da2e04f1d557`).

## Batch 8 Autonomous Audit (2026-11-27, `logs/082446-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `f83aaf4f138d`, `result: returned 1441823 0`):

- **Read-Only UFS `modem_a` FAT16/FAT32 Firmware Reader & Root Discovery** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Implemented bounds-checked read-only FAT16/FAT32 parser (`fat_init_geom()`, `fat_find_image_dir()`, `hakios_modem_fat_read()`, `thyme_modem_fat_scan()`) over `hakios_ufs_read_partition("modem_a", ...)` and exported `kern.hakios.venus_fw_size` / `kern.hakios.cdsp_fw_size`.
  - Live Mi 10S probe read `modem_a` BPB (`UMI ufs_fw: modem_a FAT16 bps=4096 spc=4 fat_sz=15 root_ent=512 root_clus=1095639119`) and discovered the single HyperOS 3 root directory entry `UMI ufs_fw: root[0] 'PCAFUI~2   ' attr=0x10 clus=2 size=0` instead of a standard 8.3 `IMAGE` folder (`UMI ufs_fw: modem_a/IMAGE not found`). Inspecting `clus=2` and LFN/subdirectories next to locate `venus.*` / `cdsp.*`.
- **Artifacts**: `hakios-kernel` (`17,039,248 B`, sha256 `f83aaf4f138d6befe9766cdebe40854f4780b52642322b6ac923973c8ff0d8b4`), `userland/root.apfs` (`48,992,256 B`, sha256 `6393155cc2be9631641de8ec96657b232a117926121efd4ef70eee375ca74f99`; total `66,031,504 B < 66,060,288 B`), `logs/082446-run.log` (sha256 `52e2f39246eb76363b0d2189891d8b87b69bb03142fe3955f9032b217aa5bfd9`).

## Batch 7 Autonomous Audit (2026-11-27, `logs/080948-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `9d4866517785`, `SCENE=daily` rootfs `6393155cc2be`, `result: returned 1441823 0`):

- **Rear Camera OIS (`0x1c`) Live CCI Verification (`PARTIAL` — HW Readback `PASS`)** (`devices/thyme/drivers/camera/hakios.thyme_umi_camera.c`):
  - Added opt-in `cam_ois_probe=1` hook in `umi_camera_ios_prepare()` running one clean rear `1920x1080` `umi_camera_ios_start()` + `umi_camera_ios_stop()` cycle (`err=0`, all 3 camera regulators released `error=0`).
  - Live log confirmed CCI `0x1c` answers (`UMI ois: 0x1c ok=1 r0=0xf0 r2=0xf0 r4=0xf0 r6=0xf0 r7000=0xf0`, `UMI camera: cam_ois_probe start err=0 ois_chip_id=0xf0`), matching `umi_ois` in `com.qti.sensormodule.umi_semco_s5khmx.bin`; active OIS gyro/servo init remains `TODO`.
- **`HakiosMetalCA.m` `metal_apps=1` Window-Server Fix** (`userland/src/HakiosMetalCA.m`):
  - Exempted `window_server` (`backboardd`) alongside `/var/containers/Bundle/Application/` when `metal_apps=1` is set (`if (!application && !window_server)`) and restricted `hm_draw_on=1` to user applications only. Note `SCENE=daily` builds default OpenGL ES unless `METAL_PLUGIN=ca` + `metal=1` are explicitly enabled; `backboardd` and `SpringBoard` launched cleanly (`[BootUI] updateOverlay: Dismissing overlay: shell launched`).
- **`pcie2_nr=1` & `wdt=1` Interaction Check**:
  - Confirmed under `pcie2_mhi=2` (`AppleBaseband` / `CommCenter` IPCR handoff), kernel `pcie2_nr=1` NR-first NAS request is bypassed while `Mav17` SIM slot 1 `USIM` initializes cleanly (`AID a0 00 00 00 87 10 02...`), and `wdt=1` completed 20 pets + `UMI wdt: quiesced (ctl=0 pets=20)` across the 75 s run.
- **Artifacts**: `hakios-kernel` (`17,005,904 B`, sha256 `9d4866517785417e7e24a707682b62d00eff8a42a53fc31991ab536c16a9c6e0`), `userland/root.apfs` (`48,992,256 B`, sha256 `6393155cc2be9631641de8ec96657b232a117926121efd4ef70eee375ca74f99`; total `65,998,160 B < 66,060,288 B`), `logs/080948-run.log` (sha256 `b6959bc77329bc3d2bdc09ab363c0bb95227fc173682d44ea7ef60e2c09d8113`).

## Batch 6 Autonomous Audit (2026-11-27, `logs/075911-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `be4a673bce03`, `result: returned 1441823 0`):

- **Active SM8250 Hardware Watchdog (`qcom,wdt@17c10000`, `PASS`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`, `xnu/files/osfmk/arm64/loader_return.c`, `devices/thyme/ram.args`):
  - Added opt-in `wdt=1` arming (`bark=524287` / 16 s, `bite=1048575` / 32 s, `WDT_CTL=0x1`), 4.0 s `keys_thread` pet loop (`UMI wdt: pet #1..#3`), `kern.hakios.wdt_armed` / `kern.hakios.wdt_pets` sysctls, and `hakios_wdt_quiesce()` wired into `loader_return()` prior to `hakios_ufs_quiesce()` so panics and normal exits disarm the WDT before handing back to `xnuloader`.
  - Live 60 s run confirmed: `UMI wdt: armed (bark=16s bite=32s ctl=0x1 sts=0)`, survived past the 32 s bite window with 16 pets, and exited cleanly with `UMI wdt: quiesced (ctl=0 pets=16)`.
- **Live 10 s QUP15 `bq25970` / `fsa4480` Telemetry Refresh** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Extracted shared `thyme_qup15_poll(bool initial)` helper called on boot (`initial=true`) and every 10 s in `keys_thread` (`initial=false`, silent on QUP15 changes), refreshing `kern.hakios.fastchg_*` and `kern.hakios.usbc_switch_status` (`UMI fastchg: refresh vbus=5112 mV vbat=4407 mV ibus=0 mA usbc_st=0x5`).
- **Artifacts**: `hakios-kernel` (`17,005,904 B`, sha256 `be4a673bce039f0d4cd5480f30529c280eb3320aaea93bf33b92a3f1d944e1b5`), `userland/root.apfs` (`48,992,256 B`, sha256 `99594b1ddac37ac810aacf62d6619f35ca8a0e99aa49946eda233e69c269bc8a`; total `65,998,160 B < 66,060,288 B`), `logs/075911-run.log` (sha256 `d3eeb36a20552a1c8ef9af7198cc570ad7c358fc3f0891caa1ecab15fa681e86`).

## Batch 5 Autonomous Audit (2026-11-27, `logs/034840-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `3b2ca5f1ba82`, `result: returned 1414621 0`):

- **NFC NCI 2.0 Char Device (`/dev/hakios-nfc`) & Exact-Length NCI Reader (`PARTIAL` -> HW+cdev `PASS`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Replaced fixed-size NCI reads with header-first `nfc_read_packet()` (reads 3-byte NCI header then exact `hdr[2]` payload length over QUP1 `0x28`), eliminating trailing padding/overlap: `CORE_RESET rsp (16 ms irq=0 len=4): 40 00 01 00 00 00`, `CORE_RESET ntf (len=13 ver=0x20): 60 00 0a 02 01 20 04 05`, `CORE_INIT rsp (4 ms len=33): 40 01 1e 00 1a 3e 06 00`.
  - Registered `/dev/hakios-nfc` (`nfc_cdevsw`: `open` asserts `VEN` wake pulse, `write` sends NCI frame to `0x28`, `read` polls `IRQ` `gpio111` up to 200 ms and returns exact packet, `close` restores saved `VEN`/`FIRM` state) and exported `kern.hakios.nfc_dev_ready=1`. Userspace NCI daemon / CoreNFC bridge remains `TODO`.
- **Synopsys DWC3 (`dwc3@a600000`) Read-Only Hardware Telemetry Probe** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Added strictly read-only `thyme_usb_dwc3_probe()` (`0x0a60c000`, zero writes so `xnuloader` live USB stays intact) and exported `kern.hakios.usb_dwc3_id` / `kern.hakios.usb_dwc3_dsts`: observed `id=0x33313130 hw0=0x4020400a hw3=0x10420086 dsts=0x82cdf0 speed=0 link=0` with clean USB log return (`result: returned 1414621 0`).
- **Artifacts**: `hakios-kernel` (`17,005,336 B`, sha256 `3b2ca5f1ba82b2cf4cd4d3aae1411672f44bffd51672521b36fbf7200fb69ec2`), `userland/root.apfs` (`48,992,256 B`, sha256 `99594b1ddac37ac810aacf62d6619f35ca8a0e99aa49946eda233e69c269bc8a`; total `65,997,592 B < 66,060,288 B`), `logs/034840-run.log` (sha256 `b057b2da5fb619119e9234cb4b1d49a291a002501007e1517f5b744800b78fd8`).

## Batch 4 Autonomous Audit (2026-11-27, `logs/033757-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `ae72b13386b0`, `result: returned 1441823 0`):

- **NFC Controller (`nq-nci@0x28`) Real NCI 2.0 Bring-Up**: Replaced read-address probe (which NXP PN553/SN100 NACKs when IRQ=0) with direct NCI `CORE_RESET_CMD` write (`20 00 01 01`) after pulsing `VEN` (`gpio83`), achieving live hardware handshake: `write ACK`, `CORE_RESET rsp (19 ms irq=1): 40 00 01 00`, and 30-byte NCI 2.0 `CORE_INIT rsp (4 ms): 40 01 1e 00 1a 3e 06 00` (`kern.hakios.nfc_present=1`, `logs/033757-run.log`).
- **IR Blaster (`ir-spi@0` / `gpio116`) 38 kHz NEC TX Driver**: Verified `gpio115..118 cfg=0x1` in NORTH TLMM and registered `/dev/hakios-ir` (4-byte NEC frame writer with 38 kHz software carrier and save/restore idle GPIO state, `kern.hakios.ir_ready=1`).
- **Under-Display Fingerprint (`goodix,fingerprint`) GPIO Survey**: Read-only SOUTH TLMM check logged `pwr(10)=0/0 irq(23)=0/0 rst(24)=0x1/0`.

## Batch 3 Autonomous Audit (2026-11-27, `logs/033017-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `3008e95abefa`, `result: returned 1441823 0`):

- **PM8150 SPMI RTC Unix Sync**: Added `epoch=` boot-arg offset sync and live `kern.hakios.umi_rtc_unix_time` (`uint64_t`) sysctl in `hakios.thyme_keys.c`; verified `UMI rtc: synced unix=1795721417 (rtc=28413334 offset=1767308083)` and `delta=2 s` tick (`logs/033017-run.log`).
- **USB-C Audio Switch (`fsa4480@0x42`) Extended Telemetry**: Added live register readouts (`en=0x98 sel=0x18 st=0x5/0 jack=0x1`) plus `kern.hakios.usbc_audio_id` and `kern.hakios.usbc_switch_status` sysctls.
- **NFC (`nq-nci@0x28`) VEN/FIRM/IRQ & NCI Probe**: Expanded `UMI_TLMM_NORTH_SIZE` to `0x80000`, read `ven(83)=0x200/0x3 firm(110)=0x1/0 irq(111)=0x1/0`, pulsed `VEN`, and probed `0x28` (`NACK (VEN pulsed)` — likely PMIC NFC supply/clock gated by UEFI); added raw I2C NCI `CORE_RESET` handshake ready for powered probe.
- **Camera OIS (`0x1c`) Probe Hook**: Added 16-bit read-only `uois_read16()` probe and `kern.hakios.ois_chip_id` sysctl in `hakios.thyme_umi_camera.c` (runs when rear camera opens).

## Batch U2 Field-Test Fixes (2026-11-27, `logs/030913-run.log`, `logs/031720-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `6ed64103c961`, `result: returned 1441823 0`):

- **Rear Camera Smooth Preview & AWB Tuning**: Switched `devices/thyme/ram.args` `cam_mode=1` (`6016x4512`) -> `cam_mode=5` (`1920x1080` binned @ `65.5 fps`), initialized `camera_isp.c` `wb_state` to `1.6f` with frame-0 instant lock (`w=1.0f`) and `MAX_BLOCKS=256`, and throttled steady `AF_WATCH` sharpness checks to 1 in 3 frames in `camera_device.c`. Verified `S5KHMX` `mode 5 (1920x1080)` streaming smoothly at ~51 fps in userspace (`frame 2` -> `frame 1200` in `23.5 s`, reaching `frame 3899`, `logs/030913-run.log`). Front camera (`S5K3T2`) updated to `res2` (`1280x720 @ 42.7 fps`) in `hakios.thyme_umi_camera.c` and `hakios.thyme_camera.c` (user interactive flip test pending).
- **Compass & SLPI Magnetometer Calibration**: Added `"mag_cal"` SUID subscription + `msg_id=1022` bias subtraction in `hakios.thyme_slpi.c` (raised `mag` to `25 Hz`), raised `inject.c` sensor poll to `50 Hz` (`usleep(20000)`), set `ReportInterval=20000` on `mag_svc`, and added dynamic rotation min/max hard-iron centering when `norm > 95 uT`. Verified `mag_cal` enabled (`SUID 1aadaaf8...`, `msg_id=1022`) and `Compass[211]` launching cleanly (`logs/030913-run.log`).
- **Microphone Continuous Capture & `VoiceMemos` Stability**: Stopped resetting `ring_primed` on partial reads (`avail > 0`) in `hakios.thyme_mic.c`, added 16-sample fade-to-zero on short reads in `audio_plugin.c`, and added `VoiceMemos` to `libhakios.app.dylib` in `userland/build.sh` (plus `BaiduMap.app/PlugIns/*.appex/*` chmod+sign fix). Verified `VoiceMemos[235]` running cleanly across Record/Stop tap sequence and `/dev/hakios-mic` capturing `23.80 s asked for, 23.61 s from kernel, 0 reads short` (`logs/031720-run.log`).

## Batch U1 Field-Test Fixes (2026-11-27, `logs/024327-run.log`)

Verified on physical Mi 10S (`5290780a`, kernel `baabb30643b4`, `system.apfs` `ec88c4a5757a`, `result: returned 1441823 0`):

- **Cellular & Native `CommCenter` (`pcie2_mhi=2`)**: Switched `boot-hakios-ram.sh` from `pcie2_mhi=1` to `pcie2_mhi=2` so `xnuloader` emits `/baseband` (`Mav17`, `63 MB`), `AppleBaseband` publishes all `22 of 22` services (`pdp_ip0..3` mux 1..4), and `com.apple.CommCenter[31]` + `CommCenterMobileHelper[67]` run without `error = 138` (`SetConfig taken`).
- **Thermal Reduction / Runtime CPU DVFS**: Added 200 ms `cpufreq_dvfs_thread()` using `ml_cpu_idle_time()` in `hakios.thyme_cpufreq.c` while preserving `hakios_cpufreq_cap()` thermal ceilings; verified live transitions (`group 2 -> 1977 -> 3187 -> 844 MHz`, `group 1 -> 1574 -> 710 MHz`, `kern.hakios.cpu_dvfs_switches`).
- **CS35L41 Audio Keep-Alive & Retry Recovery**: Raised `QUIET_BEFORE_OFF_US` to `15 s`, widened wake window to `500 ms`, and replaced single-error permanent `given_up` latching with 5-attempt / `500 ms` retry hysteresis in `hakios.thyme_sound.c`.
- **Clean Daily Startup & Detached Launcher**: Added `userland/rc.jobs.daily` (default `SCENE=daily` in `devices/thyme/build-root.sh`, only `loc_test` + Wi-Fi/DHCP setup, no Safari/`prefs:`/`rwtouch` redirects), `XL_DETACH=1` in `xl`, and detached default mode (`loader_return_after=0`, optional `--follow`) in `boot-hakios-ram.sh`.
- **Temp BaiduMap Preinstall (`BAIDUMAP=1`)**: Added opt-in `BAIDUMAP=1` extraction + signing in `userland/build.sh` staging `BaiduMap.app` (`367 MB`) at `/Applications/BaiduMap.app` in `system.apfs`.

## The Settings Location row, and Wi-Fi location in Maps (2026-11-16)

Location itself is usable: in the 01:37 RAM session Maps resolved **Chang'an,
Xi'an** with the Wi-Fi positioning provider (`logs/locrow-100.png`, 100 s into
the run, Wi-Fi connected), the same place Weather showed the user earlier.  The
Settings switch UI is now real too.

The kernel's inject parser (`bsd/kern/kern_exec.c`, `exec_inject_env`) takes
`LIBRARY NAME...` groups separated by `;`, and a program named in more than one
group gets every library, joined with `:` in the one `DYLD_INSERT_LIBRARIES`.
Preferences therefore carries both `libhakios.tweaks.dylib` and
`libhakios.prefs.dylib`; the run's log shows the tweak loader running inside
Preferences (`hakios: tweaks: Preferences (com.apple.Preferences): 1 of 1
opened`).  The first prefs dylib wrote only to stderr, which Settings does not
put in the kernel log; it now writes `/dev/console` and names every list
controller that loads or appears.

The 01:37 run (`logs/013721-run.log`, kernel `ee62d18cb881`, scenario image
`dc704c9b5021`) then shows the whole chain:

- `hakios-prefs: load PUIPrivacyController`
- `hakios-prefs: PUIPrivacyController: Location row added at 1 of 26`
- `hakios-prefs: appear PUIPrivacyController`
- `PUILocationServicesListController` loaded and appeared, then
  `PUILocationSystemServicesListController` and
  `PUILocationServicesAuthorizationLevelController` as the page was entered --
  the stock Location Services page, master switch and per-app list, not a
  stand-in.

The user saw the row appear in the same seconds and tapped into the page.
Two scene bugs had kept every automated attempt away from it: the deep link
`prefs:root=Privacy&path=LOCATION` was unquoted, so the rc shell split it at
`&` and only `prefs:root=Privacy` was ever opened; and a `prefs:` URL that
starts Settings from cold lands on the Settings root page rather than the
named page.  `rc.jobs.locrow8` now quotes the URL and opens it three times,
twelve seconds apart, with no taps (`devices/thyme/locrow.sh` runs it and
prints the dylib's lines from the run log).

One open point: an earlier appearance of the Privacy page in the same session
(the one the 80 s screenshot caught) still showed the empty group where the
row belongs; that process's console lines fall inside the part of the kernel
log the loader drops, so whether it inserted is unknown.  The dylib now also
re-checks in `viewDidAppear:` and after `reloadSpecifiers`, and does not insert
into an empty specifier list, so a page that builds or reloads its list after
the first appearance cannot silently lose the row.

The row's title and page match were narrowed afterwards (commit `5ebc320c`):
the specifier is named `Location Services`, and only a class whose name
carries `PrivacyController` is treated as the page -- the alerts subpage,
`PUILocationServicesPrivacyAlertsLevelController`, had been getting a row too
and its title showed as "Location" with a "Location Alerts" back button.  A
`prefs:` URL neither starts Settings nor navigates a running one in this
userland (`logs/032631-run.log`, `logs/035357-run.log`: only
`PSUIPrefsListController` loads), so the page was reached by scrolling the root
list (`rwtouch swipe`, one half-screen) and tapping the row at 0.54/0.36.  In
the 09:00 run the tap did reach the page with this dylib:
`PUIPrivacyController: Location row added at 1 of 26`, twice (the appearance
and the reload) in `logs/090043-run.log`.  A clean screenshot of the corrected
title was still not obtained: at about 100 s into a boot the system's Wi-Fi
password sheet for "1701" covers the screen, and in the runs that dismissed it
first the list had scrolled to a different row (Battery).  The label and the
narrower match are code and build; the row, its page and its master switch are
the device-verified part above.

## Weather's data, kvsd, and what the app waits for (2026-11-16)

The stock Weather app is where the last user-visible question sits: the place
name appeared once for the user (Xi'an, Chang'an) but never the forecast
values, and Maps' canvas stays empty.  This is what the device said about both.

**The location consent is real, and can be answered without a person.**  An app
launched by bundle id (`uiopen --bundleid com.apple.weather`) asks for location
about a minute later; the alert's buttons sit at 0.50/0.64 (Allow Once),
0.50/0.69 (Allow While Using App) and 0.50/0.75 (Don't Allow) of the screen
(`logs/weather-60.png`).  A tap at the middle one, after the alert is up,
answers it: the alert goes and `weatherd` is spawned the same second
(02:42:03, `logs/024022-run.log`; 02:50:59, `logs/024929-run.log`).  A tap
before the alert leaves it on screen.  This is the automation the Weather
scenes use; no person touches the phone.

**`com.apple.kvsd` is `syncdefaultsd`, and mkjobs' DENY kept it out.**  Weather
and its widget look up `com.apple.kvsd`; the service is one of
`syncdefaultsd`'s MachServices (`System/Library/LaunchDaemons/com.apple.syncdefaultsd.plist`),
and `userland/mkjobs.py`'s DENY names `syncdefaultsd` as one of the jobs that
"start, crash and are asked for again, over and over".  Let through
(`ALLOW=syncdefaultsd`), launchd registers `com.apple.kvsd` and the app's failed
kvsd lookups stop -- but `syncdefaultsd` itself dies at once every time:

    NSInvalidArgumentException -[NSURL initFileURLWithPath:isDirectory:]: nil string parameter

(four threads raise it, then SIGABRT and a 10 s respawn throttle,
`logs/024929-run.log`; the backtrace the kernel prints is the libc++abi abort
path, so the thrower is not in it).  Swizzling `-[NSURL
initFileURLWithPath:isDirectory:]` themselves and printing the stack when the
path is nil named the caller (`logs/033241-run.log`): the daemon calls
`sandbox_container_path_for_audit_token` into a 1024-byte buffer, converts the
path to a string and builds the URL from it; with this userland's container
manager (personas answered "not supported", `src/inject.c`) the string comes
out nil and the initializer throws.  An interpose of that C function -- print
the real path, then answer "no container" so the daemon takes its fallback --
is not usable: the call sites are inside the shared cache, and dyld dies with
SIGBUS patching them (`logs/034142-run.log`).  The fix, if the store matters
later, is a container manager that hands out a path, not a hook.  Weather
carries the ubiquity-kvstore entitlement, so the store is part of its startup.

**`weatherd` starts, and the numbers still do not come.**  With the consent
answered and the kvsd service registered, WeatherKit's daemon starts and stays
up (`weatherd[217]`, `logs/024929-run.log`), and Weather stops reporting kvsd;
it reports `fairplayd` 24 times in that run (189 in the run where only the
consent had been answered, `logs/022549-run.log`).  `weatherd`'s own strings
name a `fairplay-client` and `com.apple.fairplayd.versioned`, and WeatherKit
carries a fairplay feature flag; `fairplayd.H2` launches and exits with 0
because the port has no `com_apple_driver_FairPlayIOKitUserClient`
(`docs/ru/notes.md`, the App Store account).  The forecast values are behind
FairPlay/SEP, not behind location: the consent is answerable and the place name
is a geocode, which works.

**Maps.**  The empty canvas is the Metal wall already recorded: Maps' VectorKit
draws through Metal, and this port's Metal render path does not run yet (the
Air/Metal probe and the TURNIP/M2V path are the open work, `docs/air.md`,
`docs/official-metal-probe.md`).

### Maps on the Metal path: the window server's device is refused, the A650 is not the driver's A642L (2026-11-16)

`metal=1` makes the accelerator name HakiosMetal as its Metal plug-in (the
kernel's MiniIOMobileFramebuffer; no thyme change was needed), and the
CoreAnimation plug-in (`METAL_PLUGIN=ca`) is built from the tree's hakios-metal,
rebuilt here with `make ios MESA=1` (fui()'s sign bit no longer uses
`__builtin_signbit`, which this clang refuses in a constexpr function), and
linked with UniXNU's metal2vulkan entry points allowed undefined (`-Wl,-U`),
which the hakios-metal route does not call.  QuartzCore's `default.metallib`,
which the iPhone X image lacks, is made from the image's own four uber shaders
with `tools/merge_metallib.py` (104,784 bytes) and carried in the image as
`/redwood/share/cameta/QuartzCore-default.metallib`; inject answers
CoreAnimation's lookup with it (`hakios: metallib lookups in backboardd are
said; default.metallib is ...`, `logs/132946-run.log`).

The window server then asked for the device and was refused:

    HakiosMetal[backboardd 18]: no device: the GPU's chip ID is 0x6050002: the
    tables of this driver are the Adreno's of chip ID 0x6030500 (a6xx.hpp),
    another's would hang it

thyme's GPU is the Adreno 650 (0x06050002: a6xx gen3 in Mesa's
freedreno_devices.py, num_ccu 3, tiles 96x16, fibers_per_sp 128*2*16), while
hakios-metal's tables are the Adreno 7c+ Gen 3's (0x06030500, a6xx gen4).  The
plug-in refuses any other chip on purpose ("another's would hang it"), so
CoreAnimation fell back to OpenGL ES: the Maps screens of that run
(`logs/weather-80.png`, its "What's New in Maps" sheet) are the GL path's, not
Metal's.  The plug-in also shows its device to the window server alone unless
`metal_all=1`, and Maps was told so (`HakiosMetal[Maps 202]: Maps is not shown
the device ...`).  Enabling Metal on the phone therefore needs hakios-metal's
A650 (gen3) chip tables -- the kChipId, kFibersPerSp/kNumSpCores and kMagic*
constants in a6xx.hpp, the compiler's chip id in compiler.cpp/texture.cpp, and
whatever the gen3 render sequence differs in -- and `metal_all=1`, not only
the boot argument.  The A650's row as Mesa has it (the `__info13` entry the
chip id resolves to) and the constant-by-constant delta are in
`docs/metal-a650.md`.

The port's side of that is now two steps in (`docs/metal-a650-plan.md`):
`a6xx_chip` carries what differs, `hakios_metal_open()` selects the row from
the GPU's own chip id and both ids are accepted (`b6b5f880`); the CCU helpers,
the static state's three differing DBG_ECO entries and the compiler's chip id
all follow the selected row (`77affe4d`), with the 642L's static_assert
replaced by host tests over both rows (54 tests, 0 failed checks).  What is
left before a device run is the register-level audit the plan lists (whether
`kMaxSets`, `kRegSizeVec4`, `kHas8bppUbwc`, `kHasLpac` and
`kHasEarlyPreamble` are consumed under other names; the preamble bit in
`SP_FS_CTRL_REG0`; the 1 MiB GMEM), and then `METAL_PLUGIN=ca` with
`metal=1 ca_metallib=... metal_all=1` on the phone.  No A650 Metal frame has
been submitted yet.

On the night of 2026-11-16 the first A650 Metal boot ran (kernel `c98ee588`,
scene `f99fe23e` -- `METAL_PLUGIN=ca`, the minimal QuartzCore metallib at
`/redwood/share/cameta`, boot args `metal=1 metal_fps=1 ca_metallib=...
gpu_recover=1 anim_max=1 metal_all=1`):

* **The window server accepts the A650's device now**: "HakiosMetal[backboardd
  104]: device: Adreno (hakios-metal, chip 0x6050002) on IOAcceleratorES
  service 0x6f07; the compiler is in this build; feature profile 1 (Apple1,
  Apple2 and Common1)".  It goes on to open CoreAnimation's pipeline library,
  answer `targetDeviceInfo`, and take textures and command queues through the
  plug-in -- the "no device: the GPU's chip ID is 0x6050002" of every earlier
  `metal=1` boot is gone.
* **The screen is black, and the window server is why**: backboardd crashed
  27 times (SIGABRT, "crashed 6 times in a row"), every screenshot is the
  bare framebuffer, and the log has the assertion that ended it:
  `getLibraryByIndex_block_invoke, line 292: error 'MTLLibraryErrorDomain
  Code=6 "library not found"'`.  CoreAnimation's pipeline library names a
  `default.metallib` that this image does not carry; the plug-in's mirror of
  it looks for the stand-in at the fixed path
  `/redwood/share/ca-share/QuartzCore-default.metallib`
  (`userland/src/HakiosMetalCA.m`, the `ours` string), while this run put it
  under `/redwood/share/cameta` and pointed the kernel's `ca_metallib=` at
  that -- the boot argument satisfies the framework-bundle lookup, not the
  pipeline library's.  So the mirror had no metallib, the library lookup
  missed, and the window server aborted in a loop.  The fix is the path (or
  the plug-in reading the boot argument); the next run rebuilds with
  `EXTRA_SHARE` naming a `ca-share` directory.
* The boot line is 607 characters (`BOOT_LINE_LENGTH`) and the run's ended
  at "metal_" -- **`metal_all=1` was cut**, so Maps was not shown the device
  in this run even if its window had drawn.  The next Metal boot puts
  `metal_all=1` first or trims the daily arguments.

This is the first time the accelerator has accepted thyme's GPU; it is not a
working Metal path yet.

### The stand-in lacked CoreAnimation's other shaders; the image's own library was in rootfs.dmg (2026-11-16)

With the metallib path fixed (`EXTRA_SHARE` naming `ca-share`), the same A650
boot (kernel `c98ee588`, scene `74d4bf8b`, log `logs/221405-run.log`) got
further: CoreAnimation's pipeline library opened through the mirror and the
plug-in linked its first pipeline ("linked
fixed_vert_lph_gen+fixed_frag_lph_cpf ... for pixel format 81"), then
backboardd aborted in a loop with

    deserializeFunction, line 532: error 'Function downsample_blur_vert_lph
    could not be found in library'

The stand-in built earlier held only this image's four uber shaders (451
variants): `default.pipelinelib/functions` has no other names, while
`pipelines.desc` names 38 distinct functions (`downsample_blur_vert_lph`, the
`narrow_blur_*`, `path_*`, `inplace_copy`, `std_vert*`, ...) -- 34 of them not
in `functions`.  `tools/ca-metallib.py` had taken that to mean the rest must
come from a later CoreAnimation library (a Mac's, or iOS 17's out of an IPSW).

They did not: **the image's own
`QuartzCore.framework/default.metallib` is in `sysimage/rootfs.dmg`** --
1 014 208 bytes, 141 functions, `downsample_blur_vert_lph` and `edr_scale_lph`
among them -- and `sysimage/select-system.py`'s rule that leaves `.metallib`
out of framework directories (`SKIP_EXT`) is why `sysimage/rootfs` does not
carry it.  Extract it with

    7z e -so sysimage/rootfs.dmg "System/Library/Frameworks/QuartzCore.framework/default.metallib" > /tmp/QC-real.metallib

(3.7 s), and carry that as `/redwood/share/ca-share/QuartzCore-default.metallib`:
no download, and it is exactly what this CoreAnimation was built with.  The
plug-in no longer forces `edr_scale` to `inplace_copy`'s module: that was for
a later library whose edr_scale reads buffers iOS 16.5 never binds; the
image's own function is the one its pipelines were made for
(`userland/src/HakiosMetalCA.m`; the luma_boost/edr_scale rows in
`hm_stand_ins` now only apply to a library that lacks them).  Scene
`metal650c` carries that file; the device run is the next evidence.

This leaves a wider gap to know about: every `.metallib` inside a framework
directory is absent from the system image by that rule.  Any framework
component that loads one (this QuartzCore; others may follow) needs its file
carried the same way.

### The first pipeline with the image's own library; MPS's; the 64 MiB a scene has (2026-11-16)

With the real `default.metallib` (scene `metal650c`, kernel `c98ee588`, log
`logs/223636-run.log`), backboardd opened CoreAnimation's pipeline library,
chose its variants, and read the first function's specialisation script
("fixed_vert_lph_gen was specialised by a script of 152 bytes giving 3
values"); the earlier 4-function stand-in had no constants and linked
("linked fixed_vert_lph_gen+fixed_frag_lph_cpf ..."), and no such "linked"
line followed this time.  The log is the 1 MiB kernel ring with its middle
dropped under launchd and Wi-Fi spam, so whether that compile hung or its
line was dropped is not settled; the next boot runs the same image with
`metal=1` alone (the window server on Metal, applications still on GL), which
is quiet enough to say.  The IOSurface refusals at the end of every run
("IOSurface 2: the driver gave no buffer for it: Input/output error", after a
request 0x7e answered 0xe00002d8 = kIOReturnNotReady) are the run's own end:
`hakios_gpu_stop()` gives the GPU back (`gpu_kept = false`), and the window
server's unblank and framebuffer allocation come after it.

The next gaps, run by run (scene `metal650d`, log `logs/224450-run.log`):
SiriUICore's pipeline library no longer ends in `getLibraryByIndex ...
library not found`, and SpringBoard's next abort is MetalPerformanceShaders'
--

    MPSDeviceComputePipelineState.mm:156: failed assertion `metallib not
    found: default.metallib for identifier com.apple.MetalPerformanceShaders.
    MPSImage, .../MPSImage.framework`

-- another `.metallib` the system image leaves out.  The plug-in now answers a
missing framework URL with the copy carried under `/redwood/share/ca-metallibs`
(`newLibraryWithURL:error:` in `HakiosMetalCA.m`), and a nil URL (what
mediaserverd hands it) is logged with its caller instead of Metal's
assertion.  For MPS the files themselves are now added to the system image's
sysroot and manifest (`MPSCore`, `MPSImage`, `MPSFunctions`: 46 040 512
bytes), which the next system image build carries.

**The scene image cannot grow past the payload area.**  The `metal650e` scene
with those three MPS libraries in `/redwood/share` (84 287 488 bytes) failed
to stage: `xl: upload failed` after the system image's 2447 MB, and the
loader's `PAYLOAD_MAX_SIZE` is 0x04000000 (64 MiB, kernel and cmdline
included; `xnuloader/loader.c`).  The scene root image must stay at about the
49 MB the running scenes have; anything large (MPS's libraries, other
framework resources) belongs in the system image, not `/redwood/share`.  The
scene share keeps only the 3.6 MB of pipeline-library siblings.

The boot line is cut again: the kernel reports 100 characters that did not fit
the 607 (`logs/223636-run.log` line 13), so the last extra arguments
(`gpu_recover=1`, `anim_max=1`, the run's `battery_stop=10`) are not in force.

### The first UI frame the A650's Metal window server composed (2026-11-16, owner-observed)

With the image's own CoreAnimation library (scene `metal650f`, kernel
`c98ee588`, the rebuilt system image of 628 760 blocks, `metal=1` alone so
that only the window server takes the device and the applications stay on
GL), the window server linked CoreAnimation's fixed pipelines -- 20 "linked
fixed_vert_lph_spc+fixed_frag_lph_cph ... for pixel format 80" lines in the
live log (`logs/230236-run.log.live`).  That settles the earlier question:
the missing "linked" line in `metal650c` was the 1 MiB log ring dropping its
middle, not the compile.

And it drew: `logs/metal-frame-230236.png` (2 887 212 bytes) is the home
screen -- wallpaper, icons, dock, status bar -- composed by the A650's Metal
window server: the first complete UI frame on this path.  The next screenshot
is not: `logs/metal-partial-230236.png` (89 089 bytes) has one blue tile at
the top left and black everywhere else.  The owner, watching the phone during
the run, reported exactly that: "异常闪烁，无法触控，频繁黑屏" (2026-11-16)
-- the display alternates between a full frame and a partial/black one, and
touch does nothing while the window server is in that state.

So composition and the pipeline path work.  What remains is how the frame is
presented (damage rectangles, or the surface's present, only part of the
frame reaching the display) and input under the Metal window server.

### The desktop Weather widget has no data (2026-11-16, hardware-verified)

The Today View (a right swipe from the first home page; scene `weatherwidget2`,
kernel `c24ac306`, log `logs/232926-run.log.live`) shows the stock Weather
widget as "No weather data" beside a working Calendar widget
(`logs/weather-widget-232926.png`).  That is WeatherKit's data, the FairPlay
gate the stock Weather app also stops at; the native HakiOS weather app
(Open-Meteo) and the district-level location it uses are unaffected (both that app and the
map screen were removed on 2026-11-18: existing apps only).  The same
run is also a GL-path stability check: the home screen and the Today View drew
normally, with the window server on its verified OpenGL path and no Metal
arguments.

### The Metal compositor's command buffers time out; the CP is wedged (2026-11-17)

The idle home screen with the window server on Metal (scene `metal650g`, kernel
`79f7a8f1` with the framebuffer's diagnostics, `metal=1` alone, log
`logs/235940-run.log.live`) shows what the flicker and the black screens are:
the compositor's command buffers fail, each about 2.3 s apart --

    2026-11-17 00:01:43.533 backboardd[18:2554] [Metal] MTLCommandBuffer
    0x10109b2d0, protection 0x0, label com.apple.coreanimation.command-buffer,
    error = the driver refused the submission: Operation timed out

-- and `metal_fps` counts "0.0 frames/s over 2.3 s (0 presented, 1 command
buffer)" with a negative GPU-busy figure and 2.3 s from commit to completion.
The layer-0 surface a whole-screen swap names (`fbshow` diagnostics through
`loader_log_line()`, which the loader's log keeps: `IOLog` and `printf` do not
reach it) is 1125x2436 BGRA, rows of 4608 bytes, and at swap 360 and swap 480
alike **1310 of 2046 sampled pixels are black** (about 64%): the frame the
compositor drew into it is the partial one, and it is the same twice.

At the run's end the 3D core says why:

    adreno: 0: the 3D core before it is stopped: status 0xc00905,
    interrupts 0, ring read up to 1088 of 1063; the GMU sees it busy: 0x70c00905

-- the command processor has read past the words the driver submitted and is
wedged, so later submissions get no fence (`hakios.thyme_gpu.cpp` returns
`kIOReturnTimeout` from `request->fence == 0`) and the compositor drops them.
The first composed frame's commands partly execute (a tile, the rest black)
and then stop the CP; the next work is to capture the first submissions'
command words and the CP's state at the timeout, and find the packet or
shader that wedges it.

### The window server's first frame stops at a WAIT_FOR_IDLE inside its third pass (2026-11-17)

The quiet runs (kernel diagnostics through `loader_log_line()`, `touch_lat` off so the loader's
log keeps them) show the command stream of the first composited frame: a command buffer at
`0x21780000` of 11 487 words whose offset 3985 is a `CP_WAIT_FOR_IDLE`; the CP's report ("in the
first command buffer ... 7423 words to go") is that packet plus its prefetch, and every later
submission waits behind it (fences 37-41; "the core has done 36-40").

The packets before the wait (offsets 3808-4160, read back with `tools`-style scanning of the
kernel's `fbword` lines) are three repeated passes: type-4 register writes (0x8890, 0x88d4,
0x88d6, 0x88d7, 0x8b307, 0x8b4d1, 0x8f0, 0x840a, 0x800, 0x80a1, 0x88d3), `EVENT_WRITE 0x1e`,
a `WAIT_FOR_IDLE`, `EVENT_WRITE 0x31`, and `CP_INDIRECT_BUFFER` to `0x2399c000` of 0x1e44
words -- a shared program buffer, the same every pass.  The wait that never completes follows
the third pass's event write (0x1e at 3983), so the work that hangs is inside that shared
program buffer's draws, with the state the pass set.

The function-constants hypothesis was tested and does not hold: scene `metal650h`, whose pipelines
hand the compiler no function constants, stops in the same place (offset 4064 of 11 387 words,
7323 left).  The framebuffer's `fbshow`/`fbLine` diagnostics live in the working tree's
`xnu-src/.../MiniIOMobileFramebuffer.cpp` and need `xnu/export.sh` before a kernel commit; the
GPU driver's are in `devices/thyme/drivers/gpu/hakios.thyme_gpu.cpp`.

The cause: the uber fragment shader uses Metal's `air.point_coord`, which the front end made
`nir_load_point_coord`; ir3 has no lowering for it and called `ir3_context_error("Unhandled
intrinsic type: load_point_coord")`.  In the release build `unreachable()` is nothing, so the
device's compiler went on and built a shader the GPU stopped in -- the CP's `WAIT_FOR_IDLE` above
is that stop.  It was found by building the host Mesa (`tools/air/mk-host.sh`; the system's
`pkg-config` was missing, so meson never saw libdrm) and compiling `fixed_frag_lph_cpf` with
`ir3_compiler`, which aborted there once `ir3_context_error` was made to say its reason.  With the
coordinate lowered to the constant (0, 0) (`tools/mesa/air/air_to_nir.c`, `a478c5cd`) the same
module compiles with no error (68 951 lines of listing).  A sweep of the pipelinelib's variants
(13 sampled across the four uber functions, each compiled for chip IDs 0x06030500 and 0x06050002)
leaves no other refusal: 13 of 13 compile.  Scene `metal650j` (`cb144ed5`) carries the fix; the
device run waits for the phone to be back on USB.  Scene `metal650maps` (`9897e319`,
`rc.jobs.maps`) is the next one after it: it opens com.apple.Maps with `metal_all=1` first in the
boot arguments, so Maps, SpringBoard and the window server all take the device.

The project's AIR CI was run for the front-end change (Python 3.13, which the system lacked: its
3.12 has no `math.fma`).  `docs/air-report.md` is byte-for-byte the committed one -- reader
456/456 identical to llvm-dis, translate and ir3 321/456 as before -- and, run phase by phase,
the folded intrincheck and the executed one both exit 0 (their 30 and 57 differences are the known
softpipe ones), as do every corpus cpuref (0 differ), the atomic and the texture checks.  The
script's own exit is 1 in this environment while none of those phases accounts for it; what it
printed is the report the tree already had.

To name that shader: `hakios_metal_program_stage_words_copy()` (`hakios_metal.h`, `c_api.cpp`)
gives a stage's first instruction words, the plug-in logs them once per pipeline
("fbstage: vs+fs ... vs N: ... fs N: ..."), and the driver's failure path logs the first words
and address of every other buffer the stopped submission names ("fbbuf: ...").  Scene
`metal650i` (kernel `4b60a1ae`, scene `09e36f62`) carries them; the device run waits for the
phone to be back on USB (it was not there at 01:41).

### Weather: the app's own fairplayd gate, and a stand-in that owns the names (2026-11-16)

`weatherd`'s WDSAuthenticatorFactory takes `isJWTEnabled` from
`weatherdaemon.enableJWTTokenAuth`, and `weatherprefs` now writes it false with
`weatherdaemon.fetchesWeatherData` true into the mobile user's
`com.apple.weatherd` and `com.apple.weather` preferences; the write and the
read-back are in `logs/131642-run.log`.  It changed nothing the owner sees:
weatherd never spawned, Weather made no TCP connection and never looked up
`com.apple.weatherd.weatherkit`; the app itself looked up `com.apple.fairplayd`
and `.versioned` once a minute and retried.  The app carries the
`fairplay-client` entitlement, and its own WeatherKit client is the gate --
the daemon's switch is not on the app's path.

`fairplaystub` (`src/fairplaystub.c`, `FAIRPLAYSTUB=1`) registers the two names
as a launchd job's MachServices and answers with empty XPC replies.  With it,
the app's fairplayd lookups stop failing -- no "No such process" lines where the
run before had 15-24 -- and launchd spawned the stub on demand (`user/501/
com.hakios.fairplaystub`, "launching: ipc (mach)", `logs/133948-run.log`): the
app did send it something.  The stub's own account was lost twice: a launchd
job's standard output is /dev/null, and its `dlopen("/usr/lib/libxpc.dylib")`
failed because this image's dyld cache carries the library as
`/usr/lib/system/libxpc.dylib` (its program says so on the console,
`logs/135006-run.log`).  It now writes to /dev/console (its plist names the
path too) and opens the right path: the next run's log has it listening on both
names (`logs/140045-run.log`), the app's fairplayd lookups still succeed, and
**the app connects to neither service and sends no message**.  What the app
asks of fairplayd is therefore not a presence check it acts on over XPC -- it
looks the names up and, with them there, still asks weatherd for nothing and
makes no connection.  The stock forecast path stays behind FairPlay/SEP in the
app's own WeatherKit client; making the Weather app show numbers means putting
a data source in its place (fetch the forecast from a service that needs no
device identity and hand it to WeatherKit's model), which is the project's next
step, not a daemon switch.  The pieces and their order --
`WeatherKit.Weather` is public and Codable, Open-Meteo needs no identity, the
injection mechanism is the kernel's -- are in `docs/weather-data-shim.md`.

An injected `libhakios.weatherlog.dylib` (`WEATHERLOG=1`, the kernel's
insertion list) was meant to say which URLSession requests Weather makes.  Its
first three runs produced no line while the same image's other insertions work
at boot; the answer was the channel, not the insertion: NSLog from these apps
does not reach the run's log, and the library now writes to /dev/console as
the prefs and tweaks libraries do.  With that, one run has both canaries and
the requests (`logs/151121-run.log`):

    hakios: weatherlog: loaded into Preferences
    hakios: weatherlog: loaded into Weather
    request PUT https://weather-edge.apple.com/v1/configs (Authorization: no)
    answer -1 from configs, 0 bytes, The Internet connection appears to be offline.
    request GET https://c.apple.news/AgEd... (Authorization: no)

The seam is therefore reachable: Weather's own WeatherKit client fetches its
edge configuration through URLSession in the app's process, and the injection
puts code in that process.  Every request in that run failed because the boot
had no Wi-Fi (the Settings screen said "Not Connected").

With the link up the picture changed (`rc.jobs.weatheronline`: `net_test
joinfile /redwood/fw/network`, then Weather; `logs/151953-run.log`):

* Weather resolved the place and drew "Chang'an, Xi'an" (screenshot
  `logs/weather-140.png`), but **the app process made no HTTP request at
  all**.  The only weather request in the boot was WeatherWidget's
  `PUT https://weather-edge.apple.com/v1/configs` -- 200, 7095 bytes; the
  endpoint answers an empty JSON body with
  `{"userInfo":{"treatmentIds":[],"segmentSetIds":[6767,8872]}}`, so it is an
  experiment-assignment call and not where the data is.
* `weatherd` was spawned on demand again (15:21:19) and stayed silent; it
  made no network connection.
* Weather kept looking `com.apple.fairplayd` up and failing, about every
  15 s, through the whole run.

The next run's logger (`WEATHERLOG=1`) interposes the delegate-based
`dataTaskWithRequest:` as well, because WeatherDaemon's `WDSURLSession` uses
it (only the completion-handler entry points were caught before), and
`NSXPCConnection`, to see which service the app connects to and what error
comes back.  Its live log (`logs/152821-run.log.live`) shows Weather's only
request is the App Store bag (`bag.itunes.apple.com`, product
`com.apple.weather`); weatherd loaded the library and made no request.  So the
stock path is gated **before** any forecast fetch: the app sits on FairPlay,
and the daemon is not asked for data.  The following run adds `fairplaystub`
(so the lookups succeed and the stub answers) to see whether the app then
reaches weatherkit.authservice and weatherd, and the XPC log says where it
stops.

Two run-mechanics notes.  The final log fetch races the scene's own
screenshots: `logs/152821-run.log` is 10,108,800 bytes of raw framebuffer
(1080x2340x4), not text, and `logs/145727-run.log` was 9.8 MB of the same.
The 15 s `.live` copy is what saved the run's text (`weather.sh` now follows
the log), and `weather.sh` stops screenshots at `SECS - 150`.  The loader's
clock starts at its own boot, about 117 s (the boot image upload) before the
kernel's, so `SECS=380` is about 260 s of kernel, and the earlier `SECS=260`
runs returned at about 130 s.

With `fairplaystub` in the image (lookups succeed, replies empty) the app
still does not fetch weather: the boot's only Weather connection is its App
Store bag (`bag.itunes.apple.com`, 17471 bytes in / 1017 out,
`logs/153505-run.log`), the fairplayd lookup failures are gone, and weatherd
is spawned again (15:36:33) without a single request of its own.  Two things
are now known about the gate's plumbing: this Foundation has **no
NSXPCConnection** -- every `class_getInstanceMethod` for its selectors fails,
and WeatherDaemon's Swift client goes through libswiftXPC on the libxpc C
API -- and `com.apple.weatherkit.authservice`, the service that mints the
JWT (`WDSJWTAuthenticatorService`, `generateTokenWithUrl:completion:`,
`WDSJWTMescalSigner`), is declared by no launchd job in `/redwood/
LaunchDaemons` or in the iOS rootfs's weatherd plist (checked:
`sysimage/rootfs/System/Library/LaunchDaemons/com.apple.weatherd.plist` has
five services, not this one).  A lookup of it gets launchd's "Service stub
created" (NewsTag's at 15:35:43) with nothing behind it.  What the app sends
over XPC and what comes back is now visible: the logger carries a
`__DATA,__interpose` table for `xpc_connection_create`/`..._create_mach_service`
and the send and reply calls, so the next run names the service the app
connects to and prints the request and reply it exchanges -- the token, or
the failure that stops it.

The first run with that table (`logs/154426-run.log.live`) showed it works,
and also that logging everything is wrong: WeatherWidget polls some service
in a tight synchronous loop and 1058 of its messages filled the kernel's 1 MB
log, rotating the Weather app's own lines out before the run ended.  The
logger now flattens each description onto one line and skips preference
traffic -- the app and the widget both poll cfprefsd in tight synchronous
loops (898 and 1058 messages in two runs), which was the flood -- and says
everything else.  The same run's final log was framebuffer again even though
screenshots had been stopped: the loader does stage the combined log at the
kernel's return (`write_logs` -> `compose_logs` in `xnuloader/loader.c`), but
a screenshot in flight can stage the screen after that and before the host's
`get_staged`, and the fetch then takes the screen.  The 15 s `.live` copy is
the run's reliable text; the next runs set `XL_FOLLOW=5` for a finer last
copy.

The short runs were not the loader's clock at all: at 24-26% charge, with
`ram.args`' `battery_stop=25` and its 30 s poll, the power guard ended the
run about 100 s in -- "battguard: the charge is 24%, at or below
battery_stop=25 three looks in a row: back to the loader"
(`logs/155121-run.log.live`).  That is just before `weatherd` is normally
spawned (it did not spawn at all in that run), so the probe never saw it.
`PE_parse_boot_argn` does **not** stop at the first match: the `=` branch
parses the value and then `gotit` skips the token and the loop goes on, so a
later occurrence overwrites (`pexpert/gen/bootargs.c`).  A first try that put
`battery_stop=10` before `ram.args` still ran with 25
(`logs/155909-run.log.live`); `weather.sh` now takes `BATTERY_STOP=<percent>`
and appends it after `ram.args`: a watched long run uses 10, the default image
keeps 25.

The logger's second flood is the Weather widget's relationship to
runningboardd: it retries `handshakeWithRequest:` synchronously 1444 times in
one run (`logs/155909-run.log.live`), which fills the log on its own.  That
is a separate failure of the widget's assertions on this port; the probe now
skips `rbs_selector`/`bsx_class` traffic as it skips cfprefsd's.

The first run with both weather libraries exposed a limit, not a result:
`init: kern.exec_inject: error 22` (`logs/160615-run.log`).  The kernel's
insertion list is a `SYSCTL_STRING` over `exec_inject_conf[512]`
(`bsd/kern/kern_exec.c`), and `root/etc/inject.conf` had grown to 541 bytes
with weatherlog's four programs and weatherjwt's one; the write failed with
EINVAL and **no library was inserted at all** in that boot (no weatherlog,
no tweaks, no prefs).  The buffer is now 2048 bytes
(`xnu/hakios.patch`), enough for the next probe; the 16:06 run itself lasted
its full ~240 s -- `battery_stop=10` appended after `ram.args` did take
effect -- but with the injection refused, it only showed the app's normal
screen and then the panel's idle blank from about t=120.

### HakiOS Weather: the native screen, and the battery the runs cost (2026-11-16)
> **Removed 2026-11-18** on the owner's instruction: no self-built replacement apps. This
> section and the maps section that follows are kept as the record of what was measured;
> the app sources and scenes are out of the tree.

The stock app's wall was followed to its end and stands: with the logger in
Weather, the application and widget load it; with `fairplaystub` in the
image, launchd's lookups stop failing and the stub logs its listeners -- but
**no peer ever connects** to either name, `weatherd` is never spawned, and
the app makes no connection of its own; its WeatherKit client gives up
inside itself, before any IPC that libxpc interposition or `bootstrap_look_up`
interposition can see (the two probes in `inject_weatherlog.m`; the
`Mescal*` symbols in the dyld cache belong to JetEngine's media path, not
WeatherKit).  WeatherKit's token needs a Mescal/FairPlay signature that only
the SEP can make, and no switch, stub or preference found so far moves that.
So a weather screen that does not ask WeatherKit was built and is what the
port ships:

* `userland/src/HakiosWeather.m` + `HakiosWeather-Info.plist`: a UIKit
  application (no Metal, no WeatherKit) that asks CoreLocation for the
  position, reverse-geocodes it with the district first ("长安区, 西安市",
  the precision the stock screen had), falls back to Xi'an after thirty
  seconds, fetches Open-Meteo (no key) for current conditions, twelve hours
  and seven days, and **retries the fetch every 15 s up to eight times** --
  the link comes up beside it.  It logs to `/var/jb/hweather/weather.log`.
* It is installed like LocProbe: the scene copies the bundle from
  `/redwood/fw/hweather` into `/var/jb/Applications`, runs `uicache -a`, and
  opens `hakiosweather://` (`userland/rc.jobs.hweather`).

First hardware run (`logs/171...`): the application launched, drew its
Chinese title, the fallback city (西安), the loading spinner and its own
error line "获取失败：请求超时" -- the fetch timed out because the Wi-Fi join
had not finished when it went out (~t=25 of the run).  The scene now waits
35 s after the join before opening and closes the system low-battery alert
first; the app retries by itself.

The next run (image `d56d774e`, `logs/weather-100.png`, 19:29) is the one
that **works**: the screen reads **"Chang'an, Xi'an"** (the reverse geocoder's
district, the precision the stock app had), **7° 毛毛雨**, 体感 6° · 湿度 95% ·
风速 2 km/h, a twelve-hour strip (19时 7° 毛毛雨 … 06时 4° 晴), the seven-day
list (周一 毛毛雨 9°/6° … 周日 阴 9°/-2°) and "数据：Open-Meteo".  So the
native screen is verified end to end on the phone: CoreLocation fix,
reverse geocode, Open-Meteo fetch over the phone's own Wi-Fi, and the
render.  Its first UI was a centred text column; the redesign (header at
the left, SF Symbol condition icons, a horizontally scrolling hourly strip,
daily rows, a gradient sky and an app icon) renders too (19:53 screenshot),
with two defects found and fixed on the way:

* The link's first request can lose the race with the Wi-Fi association.
  The retry timer that was meant to cover that was created inside the
  URLSession completion -- a timer made on a URLSession queue belongs to
  that thread's run loop, which never runs -- so the screen sat at
  "正在重试（等待网络）…" for the rest of the run (`logs/195021-run.log`,
  19:52).  It is scheduled on the main queue now, and the retry window is
  twenty attempts.  The 20:01 run's log then shows real retries
  (`logs/200034-run.log`).
* Every request in that run failed `NSURLErrorDomain -1001` (timed out,
  20 s) while the name resolved -- and because the fallback fetch had gone
  out before the first real fix arrived, its retries kept using Xi'an's
  point although the screen said Chang'an.  A `fallbackPending` flag now
  drops the fallback request when a fix lands and fetches the fix's point.
  The next scene also asks `net_test resolve` and an IPv4 `net_test tcp
  api.open-meteo.com 443` at the same moment, to say whether the timeout is
  below or above TCP.

The run after those fixes is the one where everything holds together
(kernel `c98ee588`, scene `0dde7cbe`, `logs/weather-180.png`, 21:19): the
application opens, Open-Meteo answers over HTTPS, and the screen reads
**7°**, 毛毛雨 with its SF Symbol, 体感 6° · 湿度 90% · 风速 0 km/h, the
twelve-hour strip (21时 7° … 02时 6°, each with its symbol), the seven-day
list (周一 毛毛雨 9°/6° … 周日 阴 9°/-2°) and "数据：Open-Meteo" -- the
redesigned UI, at last with data, and the battery's 44% did not move (the
DSP-only fix above).  Two notes from that run: `uiopen` by URL scheme was a
no-op in the boot before it, so the scene now opens by bundle id with one
retry; and the reverse geocode failed before the link was up, leaving the
title at "当前位置" -- the app now asks Apple's geocoder once more, ten
seconds later.  The plain-HTTP wttr.in fallback stays in the app for the
boots where Open-Meteo's TLS times out, and names its source when it is used.

The run after that retry closes the loop (kernel `c98ee588`, scene
`6a0ba9a6`, `logs/weather-180.png`, 21:30): the title reads **"Chang'an,
Xi'an"** -- the district the owner remembered -- above the same verified
screen (7° 毛毛雨, 体感 6° · 湿度 90% · 风速 0 km/h, the twelve-hour strip,
the seven days, 数据：Open-Meteo), with the battery steady at 42%.  The
native weather screen is therefore done for daily use: location to the
district, live Open-Meteo data over HTTPS, a plain-HTTP fallback, and an
application icon; what it cannot do is the stock app's WeatherKit path,
which stays behind the SEP (above).

The battery guard is real but the runs are expensive: a 4 GB boot image
uploaded at 24 MB/s, four minutes of kernel and the display on took the
phone from ~28% to 0 across five sessions, and at 0 the phone disappeared
from adb and fastboot entirely (it charges only in Android, at the USB
port's 500 mA, and the loader does not charge at all).  Henceforth: check
`adb shell dumpsys battery` before every session and run only well above
20%, prefer fewer, shorter runs, and let Android charge between them.

## Personas, and the container path kvsd cannot get (2026-11-16)

launchd's log carried "Unable to find persona with type 6" on every job spawn
(207 in one run).  The kernel is built with `CONFIG_PERSONAS` and the syscalls
exist (`bsd/kern/sys_persona.c`), but allocation and lookup are gated by
`com.apple.private.persona-mgmt`.  The port's stand-in AMFI
(`libkern/amfi/miniamfi_entitlements.cpp`) reads the entitlements property
list from the program's own signature without validating it, so a program
signed with ldid (the Theos toolchain has one) carries the entitlement.
`persona_test` (`src/persona_test.c` and `.entitlements`) allocates
`PERSONA_SYSTEM_PROXY` (6), `PERSONA_SYSTEM` (4) and `PERSONA_DEFAULT` (5): on
the phone all three succeeded (ids 501, 511 and 521) and `kpersona_find_by_type`
answered 1 found for each; the same calls fail unsigned.  The type-6 persona
is made **for the mobile user (uid 501)**, not uid 0: launchd asks for that
uid on application spawns ("No matching personas of type 6 uid 501"), which
the uid-0 proxy leaves at 1 a boot; with the uid-501 proxy it is **0**
(`logs/095656-run.log`).  Two type-6 personas are worse, not better: launchd's
own by-type lookup then says "No matching personas of type 6 uid -1, found 2".
That `uid -1` line (7-8 a boot, during the system domain's setup) is there in
every configuration, the uid-0 proxy included, so it is launchd's request
shape, not the personas' number.  `rc.launchd` now runs
`persona_test alloc` before handing process 1 to launchd, and the type-6 lines
went from 207 to **0** in the run that did it (`logs/044045-run.log`).

`personas=1` among the kernel arguments leaves the container manager's
`+[MCMUserIdentityCache personasAreSupported]` alone (`src/inject.c`), so MCM
may use the personas; without it the port keeps answering "not supported", as
before.  The switch was taken and logged in that run.

A daily soak on the same tree (`logs/081814-run.log`, kernel `ee62d18cb881`,
scene `f8f50e0c83ee`, 726 s) carries the boot allocation in its log, has **no**
"Unable to find persona with type 6" line, no panic and no jetsam, brings en0
through DAD, has the H4 transport ready, stays thermal-nominal (52.9 then
51.4 °C, three slowdowns) and shuts the audio DSP down cleanly at 726 s.

A second soak on the final set -- the single uid-501 system proxy
(`logs/100410-run.log`, kernel `ee62d18cb881`, scene `3016cb869d3f`, 719 s) --
allocates the three personas, has no "type 6 uid 501" and no "Unable to find
persona with type 6" line, no panic and no jetsam, submits 189 `hakios-gl`
frames and stays thermal-nominal at 54.9 and 54.5 °C (24 slowdowns).

The kernel that carries the SNS diagnostics of the sensor work was then run on
the same daily scene (`logs/114009-run.log`, kernel `ca09f4e2c186`, scene
`3016cb869d3f`, 419 s): the boot allocation is there, the uid-501 line is 0,
no panic and no jetsam, 164 `hakios-gl` frames, thermal nominal at 53.3 and
54.1 °C (30 slowdowns) -- the sensor code is inert without `slpi=1`.

The kvsd crash is not the persona API.  `syncdefaultsd`'s nil URL comes from
`sandbox_container_path_for_audit_token` -- the sandbox container, not the
persona's path -- and it still dies the same way with the personas present and
MCM unhooked.  The bytes behind the nil string are stack garbage
(`80 80 7f 00 01 00 ...`, `logs/044724-run.log`): the sandbox call answers
success and writes no path.  The diagnostic library now substitutes
`/private/var/mobile` for a nil URL path (`src/inject_exc.m`), so the daemon
can build its store instead of dying; that is code and build, not verified on
the phone yet -- the app's kvsd lookup is not deterministic (one run asks, the
next does not), and a direct `com.apple.kvsd` ping wanted `xpc/xpc.h`, which
the Theos SDK does not carry.  `syncdefaultsd` stays in mkjobs' DENY: without
`ALLOW=syncdefaultsd` it never runs, and the substitution only matters in the
scenes that allow it.

## The Wi-Fi password sheet, and the keychain's slow start (2026-11-16)

About a hundred seconds into some boots the system puts "Enter the password
for ..." over the screen.  The kernel's Wi-Fi driver joins with the network's
name and passphrase from `fw/network`, but iOS's own `wifid` does not see them:
its keychain item (service `AirPort`, account = the network name) is missing.

`wifipw_test` -- signed with wifid's name and the system-keychain entitlement
(`src/wifipw_test.entitlements`) -- shows the keychain working from a program
started later in the boot: at 60 s it sets a made-up network's password, reads
it back ("THE SAME PASSWORD", service `AirPort`, access group
`com.apple.wifid`, protection class `ck`), removes it and reads nothing
(`logs/092647-run.log`).

`wifipw_seed` (`src/wifipw_seed.c`) does the same with `fw/network`'s two
lines, so the sheet would not need to ask.  As a launchd RunAtLoad job
(`com.hakios.wifipw.plist`) it does not get an answer: the process spawns at
about 10 s, waits its 50 s, and its first keychain call is still waiting when
the run ends (no exit, no answer, `logs/091858-run.log`, `logs/093307-run.log`
-- the second with the delay), while the same call from the scene-60 s program
returns at once.  `securityd` never appears in either log, so the difference is
the job's context (a launchd job in the user domain), not the moment; the
Security framework's call does not time out either.  The job is therefore
**not installed** by the build; the tool and the plist stay as the experiment,
and the fix is a keychain call that can time out and retry, made from the same
context the working call has.

On the boots held on the home screen the sheet did not appear in the
screenshots; it has been seen over Settings and Weather, so its appearance
depends on when wifid asks.

## The sensor probe, the DSP's DIAG channel, and 189 of 189 (2026-11-14)

The sensor DIAG probe never reached its measurement because its scene -- the
daily `root-wirelessfull` -- has no `fw/slpi.*`, so step 2 stopped with
"nothing written" (no image found); the 13 November note had read that as a
loader-session failure. The probe now boots the DSP's test scene
(`root-slpitest.apfs`, built without the audio firmware; 31.7 MB, and 48 MB of
payload with the kernel, under the loader's 64 MiB), and the DSP comes up on
every run since (`logs/002337-run.log`, `logs/022234-run.log`).

The DIAG channel is still not open, and the boot log's middle is gone (the
loader keeps the first 256 KB and the last 1 MB, and the bring-up lines fall
between), so the driver now counts the GLINK handshake and the SNS results and
prints them at teardown. The 02:22 run (`logs/022234-run.log`, kernel
`dc059dbe0ce7`, scene `634f5a7281a7`): versions agree, the DSP **acks** our
"DIAG" open but reports no channel id (`theirs 0`) and sends no intent on it,
so the channel stays closed; the DSP also opens `mhi_sat` and
`LOOPBACK_CTL_DSPS`, which this driver does not speak. The registry upload now
sends **189 of 189** groups, the empty `sensors_registry` index included; the
client probe still gets four non-zero results, so the index alone does not open
the gate. With the upload and the client probe both on, the first refusal is
named: QMI 514 (the registry write) answered 1 while writing the group
`ccd_ttw`, so the writes are not refused wholesale (`logs/024810-run.log`,
kernel `75e99555e03f`). The four refusals are not yet separated from the
client probe; the next run keeps all of them. The full account and the next
steps are in `devices/thyme/docs/sensors-status.md`.


The SNS answer and the QMI answer are two layers: the QMI result (TLV 0x02)
answers 0 to almost anything, the SNS result (TLV 0x11) is the framework's own,
and the gate was always the second. With the upload, the client probe and the
accelerometer config on, the teardown summary now separates them
(`logs/034413-run.log`, kernel `3a5eb2dd35d6`): message 1 and the config both
answer **5** at the SNS layer, and **187 of the 189 registry writes** answer 5
too -- "189 of 189 written" counts sends, not acceptance. The registry service
itself is the one without an instance, so the framework's own start is the
gate; the next step is a summary of the file requests the sensor process makes
while it initializes, with the index present as it now is.


The FastRPC file service now counts what the sensor process asks for and
prints the last opens at teardown: without the listener order nothing is asked
(`0 file requests`); with it the DSP reads `/fw/sensors/soc0/hw_platform`
(6 bytes) -- the fusion sensor's init, not the registry's. The registry's index
file is empty, as on the phone, and could never have been served: `apfs_file()`
and the sensor's file service both refused zero-length files; both now serve
an empty file as an empty file (kernel `be6ebd2aaca2`). The listener order,
the full daily set and the probe tags fit the loader's 607-character line; the
hashes xl appends are what get cut.


The listener order and the grown heap still read only
`/fw/sensors/soc0/hw_platform` (6 bytes): the registry's index read does not
appear, so the tunnel being up is not what the registry waits for; the client
manager's own start is. xl gained `XL_NO_HASHES=1` for probes that need the
loader's 607-character line: with the hashes out the line arrives whole
(589 of 607, `logs/062852-run.log`, kernel `be6ebd2aaca2`), and xl's `inputs:`
line still names the kernel, image and loader.


The first per-transaction reading (400 s, listener + registry upload,
`logs/122016-run.log`, kernel `82d113234e63`) confirms the four QMI-level
refusals are `ccd_ttw`, `sns_diag_sensor_datatyp`, `sns_reg_config` and
`sx933x_0_platform.regis`, and shows 185 of 189 writes answering 5 at the SNS
layer per transaction: the registry service is really without an instance. The
DSP asked the AP for one remote module, `apps_std`, four times, and never for
`sns_registry`; with one file read (`hw_platform`), the framework's
registry/property path is never reached -- what stalls is the sensor process's
own start, before the tunnel or the registry files.

## The window server's SIGSEGV at the run's end, named (2026-11-13)

`backboardd` dying with `signal 11 at 0x18` was called the return's teardown
artefact, and it is: **47 run logs carry this stack** (`grep -l 0x1d0cd50ec
logs/0*-run.log`), and in the 46 of them whose run's end can be measured the
fault comes within ~12 s of the last kernel timestamp (median 12 s; measured
with the `tl:` microsecond lines around the fault, since the instrument lines
between the last kernel stamp and the crash carry no stamp of their own). It
was, however, an unnamed artefact. It has a name now.

The stack, resolved: a worker thread's `thrd_start` (Mesa's C11 threads,
`threads_posix.c`) called into the `u_threaded_context` layer, which was in
`freedreno_draw` -> `a6xx_fd6_draw` -> `a6xx_fd6_emit` ->
`fd6_build_bindless_state` -> `fd_bo_mark_for_dump`. The slide was solved, not
guessed: a BL's return address is the call's address + 4, so every frame fixes
the slide to (printed - call site - 4); over the shipped library's `__text`
(109,424 BL/BLR sites in 2,024,369 instructions) the six return addresses meet
in exactly one page-aligned value, **0xb71f4000**, and the offsets then match
`xnu-deps/build/mesa/mesa-25.0.7`'s objects byte for byte. The fault is
`fd_bo_mark_for_dump(NULL)`: the function is `bo->reloc_flags |= FD_RELOC_DUMP`,
offset 0x18 of the buffer object -- the address the kernel printed.
`fd6_build_bindless_state()` calls `fd_bo_new()` and passes the result straight
on, so an allocation that fails is a store to 0x18 on the render thread.

Why the allocation fails is the run's ending: `hakios_gpu_stop()` gives the GPU
back (`gpu_kept = false`, "the run is about to end") a couple of seconds before
the shutdown finishes, the UI is still drawing, and the next `kMsmGemNew` is
refused with `kIOReturnNotReady` -- the one refusal the DRM shim mapped to a
plain EIO with no line in the log, which is why the runs showed a NULL buffer
and none of the driver's own allocation-failure lines (`no memory for a
buffer`, `no handles left`, `a buffer ... cannot be entered ...`). Handing the
GPU back earlier than the clients stop drawing is also why the fault lands
where it does in every run.

That also corrects the Sileo reading of the same evening: the 01:01 run's GET
tap and its `apt-get -sqf ... install --reinstall tree=2.0.1` happened 17 s
before this fault, i.e. in the run's last seconds -- the run ended mid-scene,
before the scene's own apt simulation could print. The queue did not "clear on
an apt error", and nothing restarted the UI mid-run; the run simply ended.
`devices/thyme/sileo.sh` gives that scene 900 s now (its taps come ~737 s of
kernel time; the battery guard is at 10 %, since 25 % had cut a 600 s run short
at 316 s before).

The fix, in two halves. (1) `tools/mesa/fd6-bindless-null-bo.patch`: a NULL
from `fd_bo_new` leaves the state group out (`fd6_state_emit()` already
disables an empty group, and the next frame retries) instead of dereferencing
it; a failed `fd_bo_map` is handled the same way. (2)
`tools/mesa/hakios-drm/hakios.drm.c`: every kern_return_t the shim did not name
became a silent EIO; it now prints `hakios-drm: method 0x.. refused: 0x..`
once per method and code (and says once when its 16-pair table is full and
further refusals go unnamed), so `kIOReturnNotReady` has a line next time. The
renderer's fault line is still named for real crashes:
`say_the_fault()` (`userland/src/inject.c`) notes every loaded image that is
not the shared cache's -- dyld lists the cache's 2,710 first, and those
addresses are already named by the unslid number -- at the end of
`hakios_init()` (after `opengl_es()`, so OSMesa is in the list), and prints
`pc libOSMesa.8.dylib+0x7462e4` (this crash's own offset) with one
`hakios:   from NAME+0xOFF` per frame, read without calling dyld from the
signal handler. The guard is in the built object, not only the source: in
`a6xx_fd6_image.cc.o`'s `fd6_build_bindless_state` the call to `fd_bo_new` is
followed by `cbz x0, <cleanup>` at 0x1f8 and the call to `fd_bo_map` by the
same at 0x208, both to a block that decrements the ringbuffer's reference and
returns NULL.

The images carrying both: `out/thyme-boot/root-wirelessfull.apfs`
`61be860be080bda8223fae0b892dae03f6a337026eb653eb95ca93915e509fe2` and
`root-sileo.apfs`
`7865e51ec69c78cba4de91135a9a66f5a4016932fc4e938b6e4f3236e8ef2bdf`
(Mesa via `tools/mk-mesa.sh AIR=1`; without `AIR=1` the rebuild leaves out
`libair.a`, which `build.sh`'s HakiosMetal step then asks for). An image's
hash is of that build only: `/etc/motd` carries the build's date and time
(`build.sh` line 181), so two builds of the same tree differ. Both images were
read back with `tools/apfs/check-image.py IMAGE userland/root`: apfsck `-cw`
clean, and every one of the 924/925 entries matches its source file's contents
(FNV-1a) -- the image is what was built, not only what was meant.

The Sileo scene now presses the queue's Confirm too: the popup bar opens the
downloads sheet, whose Confirm is the footer's full-width button (20..355 of a
375-wide view, 50 points high), so `rc.jobs.sileo` taps the sheet's lower
heights (0.93, 0.88, 0.84) after the bar -- a tap that misses falls on the
sheet's list and does nothing -- waits 45 s for the download and the install,
and then prints apt's term.log, dpkg's record of `tree` and the binary itself.
Before this, no run had ever asked Sileo to install anything; the queue was
only queued and looked at.

### The first run on the named fault line, and the second NULL it found (2026-11-13)

`logs/050709-run.log` is the first run whose window-server fault names itself:

    hakios: signal 11 at 0x18, pc libOSMesa.8.dylib+0x746660, lr libOSMesa.8.dylib+0x747a7c
    hakios:   from libOSMesa.8.dylib+0x745db4
    ... (eight frames, all in libOSMesa)

The bindless NULL is gone (its guard holds), and the crash is a **second**
unchecked allocation in the same teardown window. The offsets, byte-matched
back to the build's objects, are `fd_bo_heap_alloc` (freedreno_bo_heap.c) ->
`fd_ringbuffer` -> `fd_batch` -> `fd_batch_cache` -> `fd_context`: the flush
after the GPU is given back allocates a ringbuffer from the heap, the heap's
`fd_bo_new` for its block fails, and `fd_bo_mark_for_dump(heap->blocks[idx])`
writes to 0x18 on the NULL -- the same fault address as the first, from
freedreno_bo_heap.c:237 rather than fd6_image.cc. `fd_bo_ref` on the next line
would have followed.

The fix is `tools/mesa/freedreno-bo-heap-null.patch`: when the block's
`fd_bo_new` fails, the range goes back to the vma heap, the `sa_bo` is freed,
the lock is dropped, and the allocation returns NULL. Built, not yet seen on
the phone.

The run also showed two things about the runner itself. The Sileo scene did
nothing at all: `/var/jb/bin/sh: No such file or directory` -- the system
image it uploaded was `devices/thyme/prebuilt/system.apfs`, which has no `/jb`,
so init had no prefix to graft. The image that carries one is
`out/thyme-boot/system-jb.apfs` (built with `SYSTEM=1` from `jb/`), and
`devices/thyme/sileo.sh` now defaults to it. And the end-of-run summary died
on its first empty section: `set -e` aborts on a failing `$(grep ...)`; each
capture now ends `|| true`. (adb itself had to be given a second server port,
`ANDROID_ADB_SERVER_PORT=5039`: the one running since the day before answered
no client.)

### The scene's long tap lines were being cut at 62 words (2026-11-13)

The 05:31 run, with the prefix in place, reached Sileo's package page -- the
300 s screenshot shows `tree` with the notification permission alert still up
-- and never pressed GET: the alert was dismissed by nothing, and the taps
after it landed on a home-screen search. The cause was in init, not in the
alert's timing: `run_command()` parses into `argv[64]` and stops at 62 words
without a word, and the scene's "two and a half minutes of taps" lines held
thirty taps, so each ran its first twelve and lost its trailing `&` -- about
48 s of dismissal, not 132, and the second line's `&` went too, so it ran in
the foreground and delayed everything after it.

`run_command()` now prints `init: a line has more than 62 words; the rest of
it is not run` when that happens. The scene was rewritten within the limit:
eleven taps a line, lines staggered 40 s apart from +15 to +515 so one is
tapping whenever the alert appears, and the GET is alternated with a
dismissal round so a tap swallowed by a late alert costs one round instead of
the run. Both scene images rebuilt and read back (apfsck clean, 924/925
entries): `root-wirelessfull.apfs`
`61be860be080bda8223fae0b892dae03f6a337026eb653eb95ca93915e509fe2`,
`root-sileo.apfs`
`7865e51ec69c78cba4de91135a9a66f5a4016932fc4e938b6e4f3236e8ef2bdf`.

### GET pressed six times, and the lock screen took the Confirm (2026-11-13)

The 08:40 run is the first whose scene reached its end, and it settled three
things at once.

**The teardown faults are gone.** `logs/084005-run.log` has no
`signal 11 at 0x18` at all: the bindless guard and the heap guard hold, and
the run ends with an orderly return.

**GET works.** The helper's log shows Sileo running `apt-get -sqf ... install
--reinstall tree=2.0.1` six times -- once per GET round -- each against its
own `sileolists` index, i.e. the tap queued the package and the resolver
answered. `dpkg` was never asked to install, and `/var/jb/var/log/apt/term.log`
does not exist: the Confirm was never pressed.

**The screen had locked.** The 600 s and 690 s screenshots are the lock screen
(8:51 and 8:52), so everything after the GET rounds -- the sheet dismissal,
the queue's bar, the Confirm taps -- landed on it. The taps were still being
delivered (their log lines are there); the idle timer had blanked and locked
the device regardless.

`libhakios.app.dylib` (injected into Sileo) now sets
`UIApplication.idleTimerDisabled` as soon as the application object exists,
repeating the ask on the main queue until it does -- an app in front that says
so keeps SpringBoard from locking. The scene's last lines also came out
mangled (`dpkg-query -W -f__`, then a stray ` tree` line that ran and failed):
the rewrite was generated inside a double-quoted shell string, which expanded
`${binary:Package}` and the rest to nothing. Restored with the editor, which
writes the text as it stands; `root-sileo.apfs` rebuilt and read back:
`7865e51ec69c78cba4de91135a9a66f5a4016932fc4e938b6e4f3236e8ef2bdf`.

### The queue holds the package; the page closes on a notification (2026-11-13)

The run with the idle timer (09:02) settled two things: **the screen stayed
awake** (the 780 s screenshot is 9:16 with the page in front, not a lock
screen), and **the button says QUEUE** -- the package is in Sileo's queue.
`dpkg-query` reports "no packages found matching tree", the honest answer at
that point; the log has no fault.

What did not happen is the sheet closing, and the 09:23 run showed three drags
do not move it either (a slow grabber drag, a drag of the page's top, another
grabber drag; its 780 s screenshot is the same sheet with QUEUE). The reason is
in Sileo's source: the page is `NativePackageViewController`, whose modal
"Done" button is commented out (`// if isModal { ... dismissImmediately ... }`),
so the sheet can only be dragged away -- and injected touches do not drive its
presentation controller.

The application now closes it itself: `libhakios.app.dylib` listens for the
Darwin notification `com.hakios.close-sheet` and dismisses what is presented
over the window's root (`close_presented`); the scene posts it with
`rwtouch notify com.hakios.close-sheet`, waits, taps the queue's popup bar and
the Confirm heights, and repeats the round once. `root-sileo.apfs`
`7865e51ec69c78cba4de91135a9a66f5a4016932fc4e938b6e4f3236e8ef2bdf` (rebuilt
and read back: apfsck clean, 925 entries).

### The refused ring, and the faults behind it (2026-11-13)

`logs/223305-run.log` is the 22:33 run -- the first with both earlier guards --
and it carries a third fault in the same teardown window:
`signal 11 at 0x1c, pc libOSMesa.8.dylib+0x747160`. The offsets, resolved
against the unstripped build in
`xnu-deps/build/mesa/build-softpipe-freedreno/src/gallium/targets/osmesa/`
(the shipped library is that file after `llvm-strip -x`), name the whole stack:
`util_queue_thread_func` -> `tc_batch_execute` -> `fd_context_flush` ->
`fd_batch_from_fb` -> `alloc_batch_locked` -> `fd_batch_create` ->
`subpass_create` -> `fd_submit_new_ringbuffer` -> `fd_submit_sp_new_ringbuffer`
-> `fd_ringbuffer_sp_init` -> `fd_bo_map+0`. The first load in `fd_bo_map` is
`bo->alloc_flags`, at 0x1c: the ring's buffer object was NULL.

The refusal is the run's end again, and this time it is named:
`hakios-drm: method 0x42 (16 bytes) refused: 0xe00002d8` (kIOReturnNotReady),
the line the previous entry added. `fd_bo_new_ring()` answered NULL and every
caller between it and the map passed the NULL on; `fd_ringbuffer_sp_init`
mapped it (its `assert(fd_ring->ring_bo)` compiles out with NDEBUG).

`tools/mesa/freedreno-ringbuffer-null-bo.patch` makes a refused allocation a
failed call instead of a fault, along every path a run's ending walked. **The
ring and the batch:** a ring's fields that `destroy()` reads are set before an
allocation can fail; a NULL `ring_bo` and a failed map fail the ring; the
submit's failure path returns the slab ring, and a streaming ring's second
reference, to the pool; a refused primary ring is not referenced; a failed
subpass or batch is destroyed and the flush reports no batch; the suballoc
block object rings are cut from is checked before `fd_bo_ref()`.
`fd_bc_invalidate_batch()` clears a cache slot only if it holds that batch, so
a batch that failed before `alloc_batch_locked()` put it in the cache (`idx`
still 0) is destroyed without evicting a live one. **The draw, the clear, the
blit:** `fd_draw_vbo()`, `fd_clear()` and `fd_launch_grid()` return when the
batch is refused (on the first ask and where the loop re-asks after a flush),
as do `fd_emit_string_marker()`, `fd6_clear_buffer()` and
`fd6_clear_texture()`; `handle_rgba_blit()` fails the blit, and `fd_blit()`
falls back. **The shader variant:** `upload_shader_variant()` reports a refused
`fd_bo_new()` instead of calling `fd_bo_mark_for_dump()` on the NULL
(`bo->reloc_flags`, 0x18), and `ir3_shader_variant()` answers NULL for a
variant that was not uploaded. `ir3_cache_lookup()` and every caller of
`get_program_state()` already treat a NULL variant as a failed compile and
drop the draw, so nothing above needed to change.

Four runs walked the refusal out of the paths that ask Mesa for memory, one
path per run, each ending where it always does -- the GPU given back, the next
allocation refused (`0x46`, `0x48`, `0x9`, `0x42` and `0x47`, all
`0xe00002d8`), the caller using the NULL it was handed:

| run | fault | where |
|---|---|---|
| `223305` | `0x1c libOSMesa+0x747160` | `fd_ringbuffer_sp_init` -> `fd_bo_map` |
| `231821` | `0x48 libOSMesa+0x6bba58` | `batch_draw_tracking`, from `fd_draw_vbo` |
| `233042` | `0x8 libOSMesa+0x709b10` | `handle_rgba_blit<6>`, from `fd_blit` |
| `234443` | `0x18 libOSMesa+0x7464cc` | `fd_bo_mark_for_dump`, from `upload_shader_variant` |

The fifth run is the first without a fault: `logs/235606-run.log` (23:56,
image `794049bd87ff8bc39bed01274fcb9a03c56dabcb1aac07607301c72d7c21e373`,
kernel `e94dc3019303`, loader `615d9a67c677`) has no `signal 11`, no `exited
due to SIGSEGV` and no crashed job. It still refuses five methods
(`0x46`, `0x48`, `0x9`, `0x42`, `0x47`) when the GPU is given back at 299.2 s,
and the window server keeps rendering -- 1,320 hardware frames logged, the
last before the give-back -- then loses the frames the device refuses, as it
should. The run ends with the audio DSP shut down and back to the loader:
`XL_SECS=185`, the complete `devices/thyme/ram.args`, `root-wirelessfull.apfs`
built by `devices/thyme/build-root.sh` (Mesa via `tools/mk-mesa.sh AIR=1`; the
build is not reproducible byte for byte, so the hash names this file only).

### The install sheet at 750 s, and the join that was refused (2026-11-14)

The first Sileo scene with the close-sheet notification and the idle-timer fix
(`logs/123501-run.log`, kernel `82d113234e63`, scene `7865e51ec69c`, 900 s)
reached the end of the UI flow: the 750 s screenshot is Sileo's **Install**
sheet with `tree` in it and "Error: The Internet connection appears to be off"
(`logs/sileo-750.png`; the 810/870 shots are the same sheet). So GET, the
queue, the sheet's dismissal, the popup bar and Confirm all work now; the
install is stopped by the network, not by the scene.

The run's own Wi-Fi says why: `lx: station driver join returned 61` at both
join attempts (`net_test: joinfile: attempt 1/2 result 2`), while the same
kernel's earlier runs joined the same AP successfully (`join returned 0`,
`logs/235830-run.log`, `logs/235606-run.log`). The radio has no MAC from
Android (`wifi_mac` absent; the stable local address `02:54:48:59:4d:45` is
used everywhere), so the MAC is not the difference. The join is flaky: the
Sileo TCP connections sat in `SYN_SENT` (`192.168.0.105:49278 <-> 38.233:80`)
and the sheet's download could never start.

The next thing to chase is the join's 61: what the station driver refuses and
when, across a successful and a refused boot — a daily-use issue in its own
right, and the last thing between the scene and an end-to-end install.

The join was then put to an A/B on the same scene (`SILEO_SECS=200`,
`EXTRA_ARGS="wifi_wide=0"`, `logs/125826-run.log`, kernel `82d113234e63`): both
joins on that boot reached the same 2.4 GHz BSS and **the access point
accepted** authentication and association (`join returned 0`), while the
12:35 boot had refused authentication twice with status 1. The refusal is
therefore intermittent, not a property of the channel. One oddity is left to
settle: the A/B asked for 20 MHz and the log still says "the channel is taken,
40 MHz wide", although `wifi_wide=0` is in the kernel command line -- either
the option is not read where it looks (a runtime print will say) or the width
is chosen elsewhere.

Next, in order: a runtime print of `lx_os_option("wifi_wide")` at the join to
settle the oddity; then the intermittent authentication refusal itself -- the
AP answers status 1 to our open-system authentication on some boots and 0 on
others, with the same MAC and the same frames, so the next step is to repeat
joins within one boot and, on a refusal, try the other BSS of the same name
(the 5 GHz one, whose joins have never been refused).

The runtime print settled the width (`logs/133626-run.log`, kernel
`18d1d62be6bd`):

```text
lx: the width: wifi_wide 1, the HT element there, its byte 3 0x7
lx: the channel is taken, 40 MHz wide around 2427 MHz: 0
```

The option reads **1 although the command line says `wifi_wide=0`**. The reason
is the port's naming: `lx_os_option()` asks `umi_wifi_is_board()`, which is
`umi_wifi_is_umi()`, and that function checks the device tree for the board
string **"thyme"** -- true on this phone. Its UMI/thyme branch answers
`wifi_wide`, `wifi_ba` and the other driver options from the board's defaults
before any boot argument is parsed, so `wifi_wide=0` (and every other option
that branch names) cannot take effect on thyme. The width is a board default,
not a decision from the AP or from the argument.

The joins in this run and its retry were accepted (status 0) on the same
2.4 GHz BSS; the refusal of 12:35 therefore stays intermittent and
unexplained, and the width is now out of the list of suspects.

The board branch now asks the boot arguments first (`lx_os_option()`,
kernel `870017e8`): a name the command line gives is answered with that value,
and only what it does not name comes from the board's defaults. The run with
`wifi_wide=0` then printed `the width: wifi_wide 0`, took the channel **20 MHz
wide around 2437 MHz**, and both joins were accepted (`logs/135413-run.log`).
The option now does what it says; an ordinary run, which names none of these,
keeps the board's defaults exactly as before.

A refusal is now asked again rather than returned at once: `station_join()`'s
authentication and association loops run up to three times whether the answer
was silence or a refusal, 200 ms apart (`wifi/lx/src/lx_80211.c`, kernel
`48755a68`). Built, not yet seen on the phone: the 14:11 run's scene did not
reach a join (its log has the driver set up and the station ready, but no
`lx: iOS join reached`), and the run before this change joined on its first
answer at 20 MHz (`logs/135413-run.log`).

The retry itself had a fault, found by the 15:42 run of the same Sileo scene
(`logs/154241-run.log`, kernel `48755a68`): `lx_os_sleep()` is called **with the
layer's lock held** (`wifi/lx/include/lx_os.h`), and the two sleeps added with
the retry did not take `lx_os_lock()`. A join refusal came at 144 s, the code
slept without the lock, and the kernel panicked in
`station_join -> lx_os_sleep -> lck_mtx_sleep_deadline -> lck_mtx_assert`:
`Mutex 0xfffffff008051b10 is unexpectedly not owned by thread` (`lck_mtx.c:168`;
`sta_runs -> boundary_join -> station_join` in the backtrace). Both sleeps now
run under `lx_os_lock()`/`lx_os_unlock()` (kernel `bac1b423`), and the same
scene is the test: it met the refusal at 144 s before.


### Black boxes in text and icons: the texture a detector watches (2026-11-14)

Reported by the owner: fonts sometimes become black boxes in Settings, the
keyboard and other UIs, and sometimes home-screen icons do too. The UI's text
and icons are not drawn by a font stack of ours: CoreAnimation renders them
through `libhakios.gl.dylib` and Mesa, and the window server fills its textures
from IOSurfaces (`userland/src/eagl.c`, `surface_to_texture()`). A black box is
therefore most plausibly a texture filled with nothing -- the same surface the
icons and glyph atlases arrive on.

The fill path now watches for that: after an image's first fill, an upload
whose sampled bytes are **all zero** prints

```text
hakios-gl: frame N: texture T filled from surface S with ZERO bytes only (W x H, pixel format 0x..., R bytes a row)
```

(once per upload, at most 20 times). The first run with it -- the full
wireless scenario, which opens the stock Wi-Fi Settings page and its list of
networks (`logs/142224-run.log`, kernel `48755a68`, image `b0b2b43f`) -- printed
no such line, and its 145-second screenshot (`logs/wirelessregress-wifi.png`)
shows every label, lock and signal icon drawn correctly: the trigger was not
exercised. Two boot arguments are ready for the reproduction: `glstill=0`
(look at every still surface every frame instead of sampling one in eight) and
`gltrace_place=x,y,w,h` (the non-zero byte distribution of each fill in the
traced frames).

The zero-fill count now rides in the periodic frame line
(`... N textures refilled from IOSurfaces (KB), Z zero fills; ...`, image
`b781fd1f`). A ten-minute soak with a screenshot every 120 s ran with it: as far
as its log survives, frames 1-760 (about 255 s, including the stock Wi-Fi
Settings page and a join at 80 MHz around 5775 MHz at 254 s) report **0 zero
fills**, and the 120-second screenshot (`logs/soak-120.png`) is clean.

The log's text stops at 254 s and the rest of `logs/143257-run.log` is binary:
the soak's screenshots are fetched through the loader's one staged buffer
(`oem screen` replaces the log in it), so a screenshot taken while the log is
being followed leaves the framebuffer in the log file. A soak that wants both
should take its screenshots with no live log fetch (`XL_FOLLOW=`).


### The black boxes, named: a table of 256 textures that runs out (2026-11-14)

The owner sent photographs of the phone while the Sileo scene ran: Settings ->
Safari with black rectangles where the switch labels and the switches' knobs
are, and Settings' root list with black squares where the row icons are
(Battery, Privacy & Security, App Store, Wallet, Passwords, Calendar, Notes,
Reminders, Freeform) while the text and some icons render. A display
screenshot at another moment (`logs/blackbox-now.png`, Settings -> News) has the
same shape: three toggle images and the top-left back area black, the text
intact. Both photographs were taken in a session whose log
(`logs/144629-run.log`) has **221** lines

```text
hakios-gl: more textures with IOSurfaces than there is room for
```

`struct image images[256]` (`userland/src/eagl.c`) is the table from a texture
to the IOSurface it shows; `-texImageIOSurface:` and `-attachImage:` answer NO
once it is full, and the layer's texture is never filled -- which is a black
box, and one that stays until the layer is made again (scrolled away and
back). That matches the report exactly: what was drawn before the table filled
is fine, what is made afterwards is black.

The table is now `IMAGES` = 1024, and `summary()` says how full it is: `... N
of 1024 images live, M forgotten`. Two more things came with it:

- a surface audit (`glaudit=N` frames, default 60, 0: never) samples every
  texture the frame drew with, copied or shared, and prints the ones whose
  source surface adds up to nothing:

  ```text
  hakios-gl: frame N: a drawn surface is all ZERO: surface S, texture T, W x H, R bytes a row, its seed X (ours Y), copied|its memory, F fills
  hakios-gl: frame N: AUDIT Z of D drawn surfaces all zero, U drawn textures never filled
  ```

- a runner and a scene for the owner's reproduction:
  `devices/thyme/blackbox.sh` and `userland/rc.jobs.blackbox` -- Safari opened
  first (a web page makes many surfaces), then Settings opened, flung to the
  bottom six swipes at 60 ms apiece, closed with the home button, four times,
  then the search field brought down and tapped (the keyboard); screenshots
  every few seconds, taken with no live log fetch.

Runs: 300 s and 600 s, kernel `48755a68`, images `77ff3175` and `8a60c017`. No
black boxes in any screenshot (`logs/blackbox-90.png`, `-204.png`, `-480.png`
are clean), no zero fills, and the table never ran out. Its census at 600 s:
**124 of 1024 images live, 1283 forgotten** -- the table churns, it does not
leak, and 256 was small enough for the heavier Sileo session to exhaust. The
audit's one finding every 60 frames is a single 24 x 132 surface (seed 3, no
fills) that is all zero while drawn; small and all-zero, it looks like a
transparent overlay (an all-zero surface drawn with blending is invisible, not
black), and it was there in both runs.

So: the failure path is real and matches the photographs, and the enlarged
table removes it for sessions up to 1024 live surfaces (eight times the worst
live count seen); that the owner's black boxes were this failure is not yet
proved -- the reproduction did not make one. The next heavy session that
previously ended with hundreds of these lines should end with none, and the
owner's fast scroll then decides it.

The same run also ended with a new instance of the window server's crash at the
run's end: after `MESA: error: flush_submit_list:135: submit failed: -5
(Operation not supported)` and `hakios-drm: method 0x42 (16 bytes) refused:
0xe00002d8`, backboardd took `signal 11 at 0x0, pc libOSMesa.8.dylib+0x71cea0`
(`logs/152739-run.log`; launchd restarted it). The stack is
`libhakios.gl.dylib+0x10bd8 -> libOSMesa ... -> 0x71cea0`; not the earlier
`fd6_build_bindless_state` NULL (that one is still quiet), and not the audit,
which calls no Mesa. Written down for the next investigation.

The same Sileo scene that issued those 221 lines ran again with `IMAGES` = 1024
(`logs/155640-run.log`, kernel `bac1b423`, image `4f922b92`): **no** "there is
room for" line, and the table's highest live count was **264** of 1024 (1080 to
1197 entries forgotten) -- this very workload goes past 256, which is exactly
why the old table ran out. No panic in that run (its joins were accepted on the
first answer, so the retry-sleep path was not exercised), and the window
server's new end-of-run crash (`libOSMesa.8.dylib+0x71cea0`) came again, at the
same place.

**The owner's verdict, using that build themselves: the black boxes are gone**
-- no font or icon turned black in their session (2026-11-14). Two things
remain from it: a felt drop in frame rate, and Sileo *sometimes* opening black
after it was closed. The frame lines (`glframes=1`, `tools/frames.py`) are the
tool for the first; the launch picture above is the subject of the second.

The automated Sileo scene caught the black reopen itself (`logs/204041-run.log`,
image `b72a9e73`): Sileo[911] was killed with 0x8badf00d at 20:56:26 and
Sileo[912] spawned at once; at 20:58:41 (the 1080 s screenshot,
`logs/sileo-1080.png`) the screen was black but for the battery and the home
bar, and Sileo[912] itself exited `exit(0)` at 20:59:14. The window server's
audit could not see that black: the surface is *opaque black*, not zero, and
the check counted zero bytes only. The display check now also reads the surface
as pixels and says `THE DISPLAY IS OPAQUE BLACK` after three audits of at
least 95% black pixels.

The same run's pacing, from `tools/frames.py`: over the retained gestures 430
frame steps, 5% over 16.7 ms and 2% over 33 ms; the slow frames are mostly
"idle before" (25 to 74 ms) -- the compositor waiting for the application's next
commit, not drawing slowly (5.3 ms a frame on the big cores, 82-90 frames a
second while commits come). The owner's felt frame drops are therefore more
likely the applications' commit cadence than the window server's drawing; the
next measurement wants a scroll in Settings with `glframes=1` to separate the
two.

The language-change path (the owner's other repro) is scripted as far as the
alert gate allows: Sileo's notification alert must be dismissed before its
avatar (top right) can open the settings list -- a tap at the avatar with the
alert up is swallowed (`logs/launch-130.png`). The next scene taps
`Don't Allow` (0.32, 0.601) first.

The language path is scripted end to end now (`rc.jobs.langtest`): the alert's
buttons are side by side, **Cancel at (0.32, 0.565) and Restart Sileo at
(0.677, 0.565)**, and the right-hand one fires. On image `e684c024`-era build
(run `logs/223147-run.log`): Sileo[178] exited `exit(0)` at 22:33:44, Sileo[211]
was spawned at 22:34:04, and at the 170-second screenshot its Featured page was
drawn (white, content still loading). **No black gap in this instance.** The
black seen before -- and by the owner -- belongs to the instance that follows a
*hung* termination: in run `logs/204041-run.log` Sileo[911] was killed with
0x8badf00d and its successor [912] was still black 2 min 15 s later. A SIGKILL
control needs a kill utility (`/usr/bin/killall` does not exist in this image);
a small `killby` on libproc is the next step, and the snapshot filter (skip a
nearly black save so the last good picture stays) is what to try if a black
snapshot is being shown.

A second termination reason explains the black reopen better than a black
snapshot: runningboardd killed Sileo with **0xdead10cc** -- "was suspended with
locked system files:
/var/jb/var/mobile/Containers/Data/Application/.../Library/Caches/Sileo/Database/sileo.sqlite3,
not in allowed directories: /var/mobile/Containers/Data/Application/.../tmp,
/var/mobile/Containers/Data/Application/..., /var/mobile/Library/Caches/org.coolstar.SileoStore"
(`logs/230240-run.log`, 23:07:03). The application's data container here is
under **/var/jb**, while that check allows only the real /var/mobile paths: every
backgrounding of Sileo while its database is open is a dead10cc kill, and every
reopen is a cold launch -- the owner sees the app close by itself and then black
until the cold launch draws. The owner's black with the network on fits an
application busy with repository work (its database open) when it leaves the
screen. The next probe is the container path itself: whether /var/jb/var/mobile
is a real directory in the running root (a symlink to /var/mobile would put the
containers where the check expects them), and whether every scene joins Wi-Fi
by itself -- the owner noted the scenes often did not; `rc.jobs.netopen` now
does (the same net_test lines as rc.jobs.sileo), and langtest/killtest should
follow. After the owner's retest, the language path drew without black once;
with the network on, opening Sileo was black again.

The container layout was then read directly (`rc.jobs.probe`, run first so the
log keeps it): `/var -> private/var`, `/var/jb` a real directory, `/var/jb/var`
real, **`/var/jb/var/mobile` a real mobile-owned directory** (empty in the image),
`/var/mobile/Containers` real -- and `/var/jb/var/mobile/Containers` did not exist
until an application of the prefix was installed and launched. So the dead10cc
paths came from containers created under the prefix.

`init` now makes the prefix's mobile corner the real one: after the graft it
empties `/private/var/jb/var/mobile`, removes it and links it to `/var/mobile`
(log: `  /var/jb/var/mobile -> /var/mobile: linked`), and skips the ownership
loop and `own_tree` for that corner when it is a link (the real one is already
mobile's). Verified on the phone (scene `rc.jobs.symtest`, image `a6acdeb1`):
`ls -ld` shows `lrwxr-xr-x /var/jb/var/mobile -> /var/mobile`, the containers
directory resolves, and `/var/mobile/Containers/Data/Application` holds 175
application directories, Sileo's among them. What is left to see is the
suspension: opening Sileo, leaving it and opening it again should no longer log
0xdead10cc, and the warm relaunch should draw without the black cold start.

The suspension was then exercised on the phone (`rc.jobs.suspendtest`, scene
`9feb8ac`, image with the link; Wi-Fi joined by the scene, Sileo opened, sent to
the background twice and opened again each time): **no 0xdead10cc at all**, one
`Sileo` process for the whole run (`pid 184`, one spawn line), and that process's
connection to the repository stayed open **225.5 seconds**, across both
backgroundings (`logs/002521-run.log`). With the containers on the real
/var/mobile, runningboardd suspends the app instead of killing it, and the
reopen is a warm resume -- the cold launch that came with every dead10cc kill,
and the black that went with it, is gone for this path. The near-black snapshot
filter (`a black snapshot was not saved`) saw nothing to do in these runs.

The felt frame drops were measured where they are felt (`rc.jobs.frametest`,
run with `glframes=1`, `touch_lat=1` and XL_FOLLOW=5 XL_FOLLOW_KEEP=1 so the
live logs keep the middle; `logs/003917-run.log` and its `.live.*`): the stock
Settings list scrolled up and down eight strokes each way ran at **88 frames a
second**, with 1-2% of frame steps over 16.7 ms and 0-1% over 33 ms; the window
server's own drawing was 2-3 ms a frame (3.3 ms on average over the run, big
cores), the GPU 1-2 ms, the copy 1 ms -- and the slow frames' time sat in **"idle
before", 13 to 40 ms**: the compositor waiting for the application's next
commit, not drawing slowly. The unlock animation is the one heavier spot
(57 frames a second, 23% over 16.7 ms, its slow frames drawn in 16.6 ms). So a
frame drop in ordinary use is more likely the application's commit cadence (or
a particular animation) than the compositor; a per-application measurement
(Sileo, WebKit) is the next one.

The same run ended with **no window-server SIGSEGV** (the earlier
`libOSMesa+0x71cea0` crash); the ring guards in
`tools/mesa/zz-freedreno-emit-ring-null.patch` are the change since the runs
that crashed, and this run's teardown exercises the same path -- one clean run
is not a proof, but the crash has not come back since.

The same measurement inside Sileo (`rc.jobs.sileoframes`, network joined by the
scene, `glframes=1`, `touch_lat=1`, kept live logs; `logs/010605-run.log`): its
notification alert must be dismissed before anything below it reacts -- five
taps at (0.32, 0.601) six seconds apart moved nothing, and the first seven
scroll strokes after them drew **0 frames** as well; from the eighth stroke on
the page moved, and then its list committed at **about 20 frames a second**
(27 to 33 frames per stroke, 100% of steps over 16.7 ms and 74-100% over 33 ms),
with the window server drawing in 1 ms and **43 to 48 ms of "idle before"** --
the application is the limit, not the compositor. The unlock animation in the
same run ran at 52.7 frames a second with 31% over 16.7 ms and an average draw
of 20.8 ms, heavier than the earlier run's 57/23%/16.6 ms. So the felt drops are
application-side (Sileo's list), while the stock Settings list runs at 88 frames
a second; profiling the application's own frame work (its main-thread drawing,
image work and layout) is the next step there, not the window server.

**Sileo's install flow completed on the phone** (run
`logs/011937-run.log.live.*`, kernel `bac1b423`, scene `018844379f`, run with
XL_FOLLOW=5 XL_FOLLOW_KEEP=1 so the whole middle is kept): the scene's taps went
GET -> queue -> Confirm -> the analytics consent -> Complete, a dpkg run's
`term.log` entry is stamped 01:25:23, and the scene's own check answers
**`tree_2.0.1_install ok installed`**, with `/var/jb/usr/bin/tree` in place
(109,184 bytes) and `tree -L 1 /var/jb` running. This is the first end-to-end
install through Sileo's UI on the current kernel; the earlier attempts stopped
at the queue.

The owner was on the phone during the same run: they switched Sileo's language
to Chinese, confirmed the restart, and Sileo opened **black for a long while**
before drawing ("开屏时卡黑屏好久", then it went in). The log has the restart
(Sileo[629] `exit(0)` at 01:28:29, and spawns at 01:28:30, 01:29:29, 01:30:32 as
they opened it again) with **no 0xdead10cc** -- the container link holds -- and
no frame ended while the screen was black, so the opaque-black audit had nothing
to sample. The black is the application's own launch before its first frame;
why that first frame takes minutes after a language change (its reload, its
localization, its repository work) is the question to answer next.

The main thread was caught in the act (`appprof=1` samples it 50 times a
second; `logs/015743-run.log.live.*`): after every restart, and for both the
language change and the install's respring, Sileo's main thread sat in
**`__connect`** for whole two-minute windows (249 or 250 of 250 samples every
five seconds) -- a blocking connect at launch to a host that does not answer,
held for the system's initial-retransmit window (75 seconds by default). While
it is held nothing draws, which is the black the owner sees.

`init` now sets **`net.inet.tcp.keepinit = 5000` ms** (the boot log says
`net.inet.tcp.keepinit = 5000 ms`, `logs/024525-run.log`), and the same restart
-- triggered without the settings UI by the injected library's
`com.hakios.language-de` notification, which writes what the language screen
writes to UserDefaults and exits as its Apply Changes does -- shows the
difference: the first five-second summary still has connect at 211 of 250, the
next ones are the run loop (`mach_msg2` 239-248 of 250), and the screenshots go
from the old page through about two seconds of launch transition to the new
page. The black that was minutes is now a launch transition. The dyld
interposer of `connect` added before this did not apply (CFNetwork's own call
is not interposable) and is left in place, unused; the sysctl is what works,
and it bounds every connect of the system, not just Sileo's -- a host that has
not answered in five seconds would not have answered in seventy-five.

With the cap at five seconds the profile still showed several blocking
connects in a row at launch (whole five-second windows in `__connect`, 210 of
250 samples, between windows of the run loop), so the cap was lowered to **two
seconds** (`net.inet.tcp.keepinit = 2000 ms`, `logs/031957-run.log`): the first
windows now have `__connect` at about 85 of 250 (1.7 s) beside `mach_msg2` at
120, and the windows after them are the run loop alone (243-248 of 250). An
interposer for `connectx` (the call CFNetwork makes) was added with a log of the
destination it gives up on, but it is not being called either -- like `connect`,
the call does not go through a symbol the dyld interposers can reach -- so the
sysctl remains what bounds the launch. What the user sees on a boot with this
image is the next measurement: the same language restart, timed in screenshots.

The two-second cap, end to end on the phone (`logs/040630-run.log`, image
`b8531f6a`, kernel `bac1b423`, `appprof=1`, a screenshot every two seconds):
the language change (the injected library's notification, the same code the
language screen runs) exits Sileo at 04:08:06 and the next instance spawns at
04:08:13; the screenshots show the old page through 58, one transition at 60-62,
**one dark frame at 64**, and the drawn page from 66 on -- a black of about two
seconds, where the owner saw minutes. The boot log has

The scripted scroll inside Sileo was attempted twice more, with the alert
dismissed over a longer window (`logs/042018-run.log`): the dismissal taps
themselves draw (112 frames at 73 a second), but the scroll strokes after them
draw **0 frames**, and the main thread sits in `mach_msg2_trap` for 246-248 of
250 samples -- the swipes never reached a scrollable page (the content was still
loading when they ran, or a modal still stood over it). The one measurement that
did catch Sileo scrolling -- about 20 frames a second, the window server drawing
in 1 ms and waiting 43-48 ms on the application -- stands; measuring it again
wants a longer wait before the strokes, or the owner's own hand on a boot with
`appprof=1`.

The wallpaper experiments were re-tested against the current GL path, same
image and kernel (`logs/043448-run.log` with `wallpaper_freeze=1`,
`logs/044509-run.log` without; image `3c836b60`, kernel `bac1b423`): with the
argument the Settings scrolls ran at **58-60 frames a second** and the unlock at
43.9 (38% over 16.7 ms, slow frames drawn in 20.5 ms), while the same scene
without it ran **86-90** and 57.2 as before; and the freeze did not even engage
-- the log says `the wallpaper's window _SBWallpaperSecureWindow is not frozen`.
So the argument stays off: whatever its path does to the wallpaper's window
costs more than the composition it means to save (the old Metal-path note in
`inject_sb.c` came from the same idea; it does not carry over to this path).


`net.inet.tcp.keepinit = 2000 ms`. The loader's fastboot was also recovered
once with `fastboot -s xnuloader reboot` (the endpoint accepted it when it would
not boot), and the scene runners now always upload the system image: a loader
that has just been booted has none staged, and the state cannot tell it from a
prepared one (two runs had booted a kernel with no system image and returned in
seconds).













The owner was on the phone during that run and used it as a person would:
they joined the network, tapped Download in Sileo, and Sileo went away; it
went away with **0x8badf00d**: `[application<org.coolstar.SileoStore>:593]
failed to terminate gracefully after 5.0s` (`logs/155640-run.log`, 16:06:24),
and launchd spawned the next instance at once. Reopening it showed a black
screen. The audit has all-zero drawn surfaces in that run -- frame 6360, `AUDIT
6 of 16 drawn surfaces all zero`, among them 1125 x 1905 and two 1101 x 1536,
each "its memory" with seed 3 and no fills -- the shape a black app screen
leaves, though the moment is the last minute of a 600 s RAM session (the GPU is
given back at ~16:06:50 and backboardd takes its SIGSEGV at 16:06:51), so what
is the end of the session and what is the app exiting on a download are still
to be told apart: the next Sileo run wants to be longer than the download takes
(1200 s), and the termination itself (who waits on what for five seconds) is
the thing to look at.

A black screen was caught with the table at 1024 and not full: the 1200 s Sileo
scene's 360-second screenshot (`logs/sileo-360.png`, xl time; kernel `bac1b423`,
image `0d3c1094`) is black but for the battery icon and the home indicator, and
the session's table census (238 of 1024 live at its highest, no "there is room
for" line) rules the old failure out. It came about eight seconds after the
scene's second `uiopen sileo://package/tree` -- the application was already
running -- and the page was drawn again at 390 s. The focused scenes
(`devices/thyme/blackflash.sh`, `rc.jobs.blackflash`: Sileo opened then Settings
over it; then Sileo opened again by the same URL) did **not** reproduce it:
Settings and Sileo drew correctly in every screenshot, the audits found only
the one small all-zero surface, and neither `THE DISPLAY IS BLACK` nor a
table-full line came. What is left to find is whether that black window is the
application's own first frame after a relaunch (the owner's reopened Sileo
after the watchdog killed it) or a frame of the compositor's; the audit now
samples the display's own surface as well (`THE DISPLAY IS BLACK`, three audits
in a row), so a reproduction will say which.

The runs also showed how not to take logs with screenshots: `xl` follows the
log every 10 s by default, and a follow and a screenshot share the loader's one
staged buffer, so a follow can fetch the framebuffer into the log -- two runs
left 9.6 and 9.9 MB of pixels (`logs/163925-run.log`,
`logs/171417-run.log`). The screenshot runners now set `XL_FOLLOW=` and stop
their screenshots 40 s before the end (`devices/thyme/sileo.sh`,
`blackbox.sh`, `blackflash.sh`).

The launchd lines around that black frame tell a story of their own
(`logs/163925-run.log.live`): Sileo[342] **exited with exit(0)** at 16:43:48 --
the application itself quit, which is the owner's "tapped Download and it went
away" -- and the scene's next `uiopen sileo://package/tree` brought up
Sileo[4552] at 16:44:26. The 330-second screenshot (16:44:55) shows the package
page, the 360-second one is black, and the 390-second one shows the page again:
the picture at 16:44:55 is most plausibly the old launch snapshot, the black at
16:45:25 the snapshot taken away before the new instance's first frame, and the
page at 16:45:55 the first real one -- a cold launch whose first frame is some
thirty seconds in coming. That is what the owner saw as "reopened and it is a
black screen". Two things follow: why Sileo exits by itself after its apt
spawns (`dpkg --print-architecture` and the like), and why a cold launch's
first frame takes that long; the launch snapshot is what covers the gap, and
whether it is shown at all is the question to ask of SpringBoard.

A cold launch was timed with screenshots every two seconds
(`devices/thyme/launchtest.sh`, `rc.jobs.launchtest`, image `2f53abb9`, kernel
`bac1b423`): launchd spawned `com.apple.Preferences` at 17:31:55.85, backboardd
put its scene in the foreground at 17:31:56.14, and the 48-second screenshot --
about two seconds after the spawn -- shows the whole Settings page drawn. There
is **no black gap** in a Settings cold launch; the screen goes from the home
page to the drawn page. So the black window the owner saw on reopening Sileo is
not what a cold launch does in general: it is Sileo's own minute after the respawn
(the picture at 29 s was the old page, the black at 59 s, the drawn page again at
89 s), and the question is what fills it -- Sileo's launch image, the snapshot SplashBoard keeps, or the
application's own window before its first frame.

Sileo's source answers the other half (`Sileo/UI/DownloadsViewController/
DownloadsTableViewController.swift`, `completeButtonTapped`): the Complete
button fades the window out and then runs
`uicache -p <Sileo.app>` and `exit(0)` -- Sileo's own "dumb respring" after an
install. The `0x8badf00d` in the run is that exit, hanging for more than the
five seconds SpringBoard's watchdog gives it; and a window that is already at
alpha 0 when the snapshot is taken is a black snapshot. That is the chain to
look at next: what Sileo's snapshot and launch image hold, with `snap_test`
(`userland/src/snap_test.c`) on `Library/SplashBoard/Snapshots/
org.coolstar.SileoStore` inside the run.

Where launch pictures live, and what they hold (`rc.jobs.snaptest` with the
save hook in `inject_sb.c`; images `e684c024` and `f56675a5`, kernel
`bac1b423`): SpringBoard's manifest is asked for snapshots and **does write
them** -- `-[XBApplicationSnapshotManifestImpl
_imageAccessQueue_saveData:forSnapshot:]` receives real JPEG data (238,704 and
153,287 bytes for a scene and its downscaled variant) and, after the call, the
file is there:

```text
/var/mobile/Library/SplashBoard/Snapshots/com.apple.Preferences/sceneID:com.apple.Preferences-default/<uuid>@3x.jpeg
/var/mobile/Library/SplashBoard/Snapshots/org.coolstar.SileoStore/sceneID:org.coolstar.SileoStore-default/<uuid>@3x.jpeg
```

`snap_test` reads them back: Settings' snapshot is 1125 x 2436, average
243 242 245, opacity 255, **0 of 64 near black** (a dark-mode one averages
26 25 26). An earlier reading of this section said no launch picture exists;
that was wrong -- it looked under `/private/var/mobile/...`, which in this root
is not the same directory as `/var/mobile/...`, so the pictures were elsewhere
all along. The format hook now names the first twelve calls with each
snapshot's description, and the save hook says the container path (always
`/var/mobile/Library/SplashBoard/Snapshots/<bundle>`), the save's size and
whether the directory and the file are there after it. Sileo's directory was
not there fifteen seconds after its first visit in the last run even though its
saves do land (the hook saw them in the run before); the listing has to follow
the save, not the home press. What remains is what the owner's black reopen
means if the launch picture is not black: Sileo's own snapshot at that moment
(its respring fades the window to alpha 0 before `exit(0)`), or SpringBoard not
showing the picture for that launch. The scene reads the JPEGs of both
applications with `snap_test`.


The screenshot runners were also corrected: the kernel returns at `XL_SECS`
minus the boot time (60-90 s), so a capture loop that stopped only 10-40 s
before `XL_SECS` was still taking screenshots when the log was fetched, and two
logs came out as megabytes of framebuffer. All four now stop 90 s early
(`SILEO_SECS - 90`, and the same in `blackbox.sh`, `blackflash.sh`,
`launchtest.sh`), and diagnostics that need the whole log run with no
screenshots at all.






## The battery guard, and the charging bit that defeated it (2026-11-11)

**2026-11-16, the percentage that fell to 2% and came back.**  On the phone the
owner saw the battery go from the forties to 2% and, a while later, back --
not a discharge, a reading.  Two paths publish the battery in this kernel:
the loader's shared battery page (`battery_poll`) and the audio DSP's battery
manager (`hakios_battery_report`, with `adsp=1`), and both wrote the same
`battery_soc_percent` and the power source the status bar reads.  The DSP's
fuel gauge is the one Android shows; the loader page's estimate is what can
hand a 40-odd percent battery a 2% sample, and once it does, the status bar,
Settings and the run's `battery_stop` guard all follow the bad number -- which
is also the likely explanation for runs that the guard ended at "24-26%" while
the phone still had hours of use.  The page's samples are no longer published
once the DSP has reported (`battery_page_ignored` counts them); the DSP stays
the only source.  The fix is in the kernel but not yet proven on hardware: the
next runs watch the percentage and `kern.hakios.battery_from_dsp`.

A 30-minute daily-scene soak on the baseline kernel (`logs/125243-run.log`, kernel
`35d4e53f`, scene `8f5bc2aa`, SHA-256 `8268de80de00a089b3f24342a075bf314ccb1c074d4ecbf647de8ae19b9afc9c`)
stayed up for the whole 1800 s: the network joined and held for 1657 s (215 frames
sent, 112 for us, 5 handshake messages, the group's key changed once; `en0` 13,261
sent / 941 KB and 22,174 received / 29.9 MB, 2 dropped out, 0 in), 537 LE reports
arrived (173 named) with no H4 transport fault, thermal stayed nominal (54-60 C)
until the return's teardown (one moderate 70.1 C, one heavy 75.5 C), there was no
panic, no jetsam, and the audio DSP stopped in order; the UI answered at 7, 15 and
25 minutes (`logs/soak30-420.png`, `-900`, `-1500`). The battery told another story:
the loader read 70% at boot and +895..+1067 mA through the run, but the cell fell to
3583 mV and 53% (the next boot read 56%, so the 70% was the unsettled gauge), and
the phone was discharging through the PC port the whole time.

That exposed the guard's blind spot. The loader's `LOADER_BATTERY_CHARGING` bit meant
only "a cable is attached", and in a RAM run the cable is always attached, so the
kernel's `battery_stop` guard refused to act on a real reading: with `battery_stop=95`
it logged `charge 56% (battery present, charger attached, charging yes); stop at or
below 95% -- not a reading it acts on` while the cell fell 3905 -> 3514 mV in six
minutes at +810..+1025 mA (`logs/133314-run.log`, SHA-256
`db035ccb0e3aed9bb04458d4fd61a2c7b0d6f9d592700696047065a32b08c4eb`). The firmware's
signed current cannot be the test either: positive values appeared both while the
cell was rising at 98% (`logs/185653-run.log`, 2026-11-08) and while it was falling
here.

The bit now follows the cell itself: `charger_poll()` in `xnuloader/loader.c` keeps
the highest terminal voltage seen since the loader started and clears the bit when
the cell is more than 40 mV below it -- a cell taking charge holds or raises its
voltage, one being drained under the load falls. A run at 53% with `battery_stop=95`
then ended by itself: `charging=0` at 3429 mV and +1418 mA, three looks, and
`--- battguard: the charge is 53%, at or below battery_stop=95 three looks in a row:
back to the loader ---` at 175 s; the kernel returned at 185 s (XL_SECS was 420),
Wi-Fi restored its SMMU context, the audio DSP was asked to stop and TrustZone shut
it down, and there was no panic (`logs/134145-run.log`, SHA-256
`651533984e8f4bed4c5d8959c3d405b91fb1968da3950faca08c276eb8637114`, loader
`50c56443`, boot image `7c5a2cd8`). `devices/thyme/ram.args` now carries
`battery_stop=25 battery_stop_poll=30`, so a run nobody watches ends before the cell
is empty. The guard still returns to the loader, not to Android: the host's
`XL_REBOOT_AFTER=1` is what puts the phone where it charges.

Known limit, by design: a charging phone whose load rises faster than the charger can
follow is read as not charging, so the guard may end a run early. That is the safe
direction; the alternative was never stopping at all.

The canonical wireless regression on the committed loader (`82a7ea5`) and the new
arguments then passed the same gate on the new boot image: 109 `UMI Wi-Fi` lines,
DHCP `192.168.0.103` with the DNS servers, 121 LE reports (37 named), zero `H4
transport fault` lines, no panic or jetsam, and the guard stayed quiet above its
threshold (`logs/134621-run.log`, SHA-256
`2bd570248a9d059bfbf709bf978c6391b3dbe4f5cadd45e6de66713b3417f863`; loader
`50c56443`, image `7c5a2cd8`, args `e1aa63c2`). The version string says `-dirty`
because the documentation was being written while the run started; kernel
`35d4e53f`, loader and arguments are the hashes in the log.

The committed baseline was then soaked for 20 minutes with `ram.args` as
committed (`logs/140455-run.log`, kernel `35d4e53f`, loader `50c56443`, scene
`8f5bc2aa`, SHA-256
`5c5437a96f9ec713cd5e00445dafd0a07fddc90e2280e5f7a7e7deae8d18d55d`): 109 `UMI
Wi-Fi` lines, the network joined for 1053 s (201 frames sent, 108 for us, 4
handshake messages, the group's key unchanged), `en0` 6,395 sent / 582 KB and
21,308 received / 29.2 MB (2 dropped out, 0 in); 614 LE reports (53 named) with
no H4 transport fault; thermal nominal at 54-57 C with one slowdown; no panic
and no jetsam; the audio DSP stopped in order at 1326 s. The battery went 54 ->
42% (V 4.0 -> 3.48 V, 37-40 C) with the guard reading `charging no` and never
reaching its 25% threshold. The UI answered at 7 and 18 minutes, Settings >
Bluetooth showing the toggle on and the phone discoverable as "HakiOS Mi 10S"
(`logs/soak20-520.png`, `logs/soak20-1080.png`); the status bar's percentage
stayed at the boot value (53%) while the kernel read 47 -> 42% -- the stale
battery item the 30-minute soak showed at 69%, now with both sides in one run.

`loader_return_after` is on time; the ~127 s is the loader's own clock, not
lateness (2026-11-11). The kernel's log timestamps are `libkern/os/log.c`'s
`absolutetime_to_microtime(mach_absolute_time())`, and this machine's absolute
clock starts before the kernel: the fastboot boot, the UEFI, the system-image
upload and the payload staging all run while the counter runs. A 130 s budget
settled it: the log's last timestamp is 255 s but the kernel returned **135 s**
after it started (the log file's mtime 20:41:42 against the launchd boot-args
line at 20:39:27), and the 1200 s and 1800 s soaks' last timestamps (1326,
1928) are 1200 and 1800 s of uptime plus about 127 s the loader spent before
the handoff (the 2.4 GB upload). So `XL_SECS` is honest and the guard and
thermal stops, which call `loader_return` directly, do not depend on any of
this; the earlier reading of this paragraph (a late timer, the timebase as a
suspect) was wrong -- CNTFRQ is 192 MHz and no timer is late
(`logs/203916-run.log`, SHA-256
`cc1c1236b5e66a1f336dd1fd1dcba714a0fc6ad67bdcc22bb5ed733210d5e221`). The same
clock explains the demo job's "131 s": launchd starts the job about four
seconds into the kernel, 127 s after the counter's origin.

The loader's reading is a power source now (2026-11-11). Kernel `0df6409a`
(commit `c527a65`) makes the loader path publish the `IOPMPowerSource` as the
audio DSP's battery manager does, and the battery scene's `battery_test` sees
it: `the power source in the kernel is HakiosBattery`, `CurrentCapacity=84`,
`IsCharging=no`, `ExternalConnected=yes`, `MaxCapacity=100`, with powerd/IOPS
reporting 84% (`logs/195212-run.log`, SHA-256
`359c7fec03901be486b8766e30421443ac23d18593753d3c390aedfb8c586655`). The lock
screen's status bar went from 83% at 7:54 to 82% at 7:58
(`logs/batterysrc-150.png`, `-360.png`): the live charge rather than the launch
value. The other user-facing places follow too: the status bar read 68% at 7
minutes and 61% at 18 of the 20-minute soak (kernel 61%), and the lock screen
showed 62%, 57% and 54% with the green charge state
(`logs/soak2-420.png`, `logs/batui4-435.png`). The lock screen's cover sheet
was the one piece missing, and its blocker is now fixed: it stayed drawn after
the one-shot auto-unlock (touch gestures did nothing, and `com.hakios.unlock`
answered "not locked" while its clock ticked, `logs/batui6-435.png`), so
`inject_sb.c`'s `unlock()` now also dismisses it directly -- `SBLockScreenManager`'s
`coverSheetViewController` and `SBCoverSheetSlidingViewController`'s
`_dismissCoverSheetAnimated:withCompletion:` (both SpringBoard 16.5's, found
with `ipsw dyld search`). The minimal unlock scene proves the call:
`hakios: unlock: asked, answer 1` then `hakios: unlock: the cover sheet was
dismissed directly` (`logs/unlock-live.log`, SHA-256
`6b6f734e8186c515b2b78d1ef8dd1f7dcc77f84ad9e6f79e496daa59a7ced03d`;
`logs/235108-run.log`). Its captures were black because the notification
does not wake a sleeping display. The visual capture then followed with the
small `rc.jobs.batteryui` scene -- notify, a wake tap, home, then the Settings
> Battery URL: `logs/batteryui-200.png` shows **Settings > Battery** with the
Battery Percentage switch on, Low Power Mode, Battery Health & Charging, and
the status bar at 60% with the charging bolt (`logs/001330-run.log`, SHA-256
`0874fe424c86e20e0ddfd18991a910b6ae4f347aeef9052453399e30eb387abd`). A person's
finger reaches it too now: the backboardd injector recognizes the bottom-edge
swipe (down below 96% of the screen, up past the middle within 700 ms) and
posts `com.hakios.unlock`, so the very gesture the cover sheet ignored unlocks
through our own path -- `hakios: input: a swipe up from the bottom edge: the
unlock asked for`, then `hakios: unlock: the cover sheet was dismissed
directly`, and the screen is the home screen (`logs/touchunlock-200.png`,
57%; `logs/011537-run.log`, SHA-256
`bf6f0cdc4df90c8485ba096973802bdc11a8bc30023fed872b80b02e011d11f1`). The
regression with this userland passed too: 109 `UMI Wi-Fi` lines, `en0`
addressed, zero `H4 transport fault` lines, both screenshots written, no panic
or jetsam (`logs/012326-run.log`, SHA-256
`77179c1d3a15e1e29b1794f247dd63e09e4f58648b559208db9c7e2081155364`), and the
scenario is the baseline (`SCENARIO_SHA256` 5f57d77b).

The canonical wireless regression on `0df6409a` then passed the usual gate: 109
`UMI Wi-Fi` lines, the network joined with `en0` addressed, 149 LE reports (33
named), zero `H4 transport fault` lines, no panic and no jetsam, and the audio
DSP stopped in order (`logs/200338-run.log`, SHA-256
`8965d63fc34b8b747f4eefcf2c8f1d640cb8190a95113769fc43e2246a028c51`). The
Bluetooth screenshot of that run failed in the host's raw-to-PNG step (a
`ValueError` on the framebuffer slice, `logs/wirelessregress-bt.png` not
written); the LE counters above are the discovery evidence, and the next
regression carries the screenshot again. The kernel is the regression baseline
now.

The regression on the rebuilt scenario -- the injector fix included -- passed
the same gate: 109 `UMI Wi-Fi` lines, `en0` addressed, zero `H4 transport
fault` lines, no panic and no jetsam, and both screenshots written this time
(`logs/235830-run.log`, SHA-256
`851e4d01d29a7bb1fec8d4851724ec11d4c68f3ad47bdc4f618ecb64bb61249c`). The
scenario is the baseline (`SCENARIO_SHA256` 71ebd6ae).

The final 20-minute soak on the shipped scenario (2026-11-12,
`logs/003259-run.log`, SHA-256
`5a0002924650ae8fde8a3a216dd352c0bb3828803f5b7e0c7283eb76beec99cf`, scene
`71ebd6ae` with the cover-sheet fix) held the Wi-Fi join for 1053 s (197
frames sent, 125 for us, 687 to everybody, 4 handshake messages), `en0` 8,675
sent / 783 KB and 21,480 received / 29.2 MB (2 dropped out, 0 in), 2,175 LE
events with no H4 fault, thermal nominal at 52.9-54.9 C with **no slowdown at
all**, the battery 57 -> 52% with the guard quiet, no panic and no jetsam, and
the DSPs stopped in order. The wall clock again matched the budget: the log's
mtime 00:53:16 against a 00:33:1x boot is about 1200 s. The status bar read
59% at 7 minutes and 53% at 18 (the kernel 57 -> 52; `logs/soak3-420.png`,
`-1080.png`).

The same soak was repeated on the shipped scenario with the touch unlock
(`5f57d77b`; 2026-11-12, `logs/020252-run.log`, SHA-256
`67dee841aeca3092ceae9e1886400fb9a7680f2534840b165145d0afd46905ae`): 109
`UMI Wi-Fi` lines, the network joined for 763 s (199 frames sent, 111 for us,
405 to everybody, 4 handshake messages), `en0` 8,896 sent / 839 KB and 21,185
received / 29.2 MB (8 dropped out, 0 in); LE events with no H4 transport
fault; thermal nominal at 52.9 C with **no slowdowns**; the battery 49 -> 44%
with the guard quiet; no panic and no jetsam; the DSPs stopped in order; the
log mtime 02:23:09 against a 02:03:0x boot is about 1200 s. The status bar
read 52% at 7 minutes and 46% at 18 (the kernel 49 -> 44;
`logs/soak4-420.png`, `-1080.png`).

With the power source in place the baseline was soaked for 20 minutes
(`logs/205259-run.log`, SHA-256
`2ee5306bc35361ea77553e4ff4bc3b694d0ed234e2517079ecbd4ae63d600b9c`, scene
`8f5bc2aa`): 109 `UMI Wi-Fi` lines, the network joined for 1053 s (7,547
frames sent, 12,026 for us, 5 handshake messages), `en0` 25,104 sent / 2.3 MB
and 28,845 received / 37.8 MB (2 dropped out, 0 in); 1,051 LE events with no
H4 transport fault; thermal nominal at 53.7-56.4 C with six slowdowns; no
panic and no jetsam; the audio DSP stopped in order. The wall clock also
confirms the corrected timing: the log's mtime 21:13:16 against the 20:53:10
boot-args is 1206 s for the 1200 s budget. The battery went 69 -> 61%
(V 3.58 -> 3.60 V, 35-38 C) with the guard quiet, and the status bar read 68%
at 7 minutes and 61% at 18 -- the live charge, matching the kernel's 61% at
the end (`logs/soak2-420.png`, `logs/soak2-1080.png`).

## Wireless regression, and the config drift it caught (2026-11-11)

The daily wireless regression on the current tree (`logs/094056-run.log`, kernel
`7349bee2`, source `37c7b7f1`, image `8f5bc2aa`, SHA-256
`e69682dad0d71783b62dd50d27eb5546a22e2183b2105df308bdcd9bf85fab5d`) passes: 109
`UMI Wi-Fi` lines, the network joined (167 frames sent, 106 received, block ack
5/5), DHCP `192.168.0.103` with Safari's TCP connections to outside hosts
through `en0`; 127 LE reports (42 named) with **zero** `H4 transport fault`
lines; 113 `hakios-gl` frame lines; no panic. `logs/wirelessregress-wifi.png`
is the joined Wi-Fi page; `logs/wirelessregress-bt.png` (9 KB) missed the
Bluetooth page (the discovery itself is in the LE counters).

It first failed, and usefully so. The kernel that preceded it had been built
from a hand-edited `.config` in which `CONFIG_HAKIOS_THYME_WIFI` was lost: the
run had not one `UMI Wi-Fi` line and `net_test: wifi: no en0 after 120 s`
(`logs/092827-run.log`). The `.config` is generated; `make thyme_defconfig`
rebuilt it from `configs/thyme.config` (Wi-Fi and the PMIC both back), and the
PCIe2 modem driver's stale `requires MODEM` was removed (it uses none of the
reference driver) so the config check passes. The old run's kernel `16883c86`
and the passing one's `7349bee2` differ only by those options and that line.

## Camera path re-check on the rebuilt config (2026-11-11)

The camera scene was rebuilt from the current tree and run on kernel `35d4e53f`
(`logs/122328-run.log`, scene `ea83382f`, SHA-256
`c12229e24b949c6dc298a57da0a9fa26ab06fa60cc677bd7810b41d6e9f5d5b6`). Its first
command, `camera_avtest`, opened a session that started cleanly:

```text
UMI camera stream: armed 2592x1940 stride 3248, 4 lane(s) cfg 0x3210 phy 4 vc 0 dt 0x2b, 6301120 bytes
UMI camera: sensor streaming mode 1 (2592x1940)
UMI camera: iOS session start error=0
camera: frame 3 read: 8482560 bytes, the caches' lines forgotten in 226 us, copied in 1766 us
hakios: camera: focus: the lens to 439 (target 439) ...
```

So the device opens, the stream arms and frames reach the capture stack on the
rebuilt configuration. The scene's UI step did not run: `camera_avtest` does
not return, so the `rwtouch` line after it never starts (the two screenshots
from this run are the home screen, not a preview). The earlier Camera.app
preview and still-capture PASS evidence is unchanged; the scene itself needs
`camera_avtest` backgrounded (or given a timeout) before its tap step can
exercise the UI. Its old first line, `sound_test springboard`, stalled the
scene the same way and was removed (`logs/121542-run.log`).

The follow-up UI run did open the app (scene `fe6583df`, taps only, kernel
`35d4e53f`, `logs/123501-run.log`, SHA-256
`399443d646849d30007c8661d21326441d999038b95698dfc71b038d9603d2b1`): the taps
started Camera.app and the capture stack opened **the rear S5KHMX's own
session** -- `3008x2256 (mode 3 whole)` -- and delivered 34 frames (33 given)
before it closed. The front-switch tap did not register (the second session is
the wide again), and the two screenshots were mistimed (the loader's hello
screen and the home screen), so this run proves the app/preview path but not
the front switch; the earlier front/rear switch evidence stands.
Timing note for the next preview screenshot: the demo job starts late in this scene (the first session began at kernel time 131 s), so a capture has to be taken about 230-250 s after the run command (roughly 96 s of upload plus the kernel's own 130 s), not at 125 s -- the two mistimed captures were the loader's hello screen and the home screen.

## Upstream main 87bf485 merged (2026-11-05)

Upstream main was merged at `87bf485`. It brings the Metal-on-Vulkan/Turnip
path, the 120 Hz and wallpaper-freeze options, the screen-class and `fb_beam`
knobs, the Metal tools, and the documentation around them. The THYME device
layer, the QCA responder order, the 90 Hz re-announcement and the daily-driver
image stay in place. The new Metal path is opt-in (`TURNIP=1 M2V=1`,
`metal_m2v=1`) and is not enabled or tested on THYME yet.

Merge resolutions:

- `configs/boot-args.lst`, `configs/symbols.tsv`, `docs/boot-args.md` and
  `configs/common_defconfig` were regenerated from the merged sources, so
  both the THYME `umi_*` arguments and the new upstream arguments are present.
- `userland/build.sh` keeps the THYME Bluetooth audio HAL overlay. The move of
  files larger than 4 MB into the system image runs only for `TURNIP`/`M2V`
  builds, so the GL-only daily-driver image keeps `libOSMesa` and the pinned
  system image. HakiosMetal is built with the Mesa AIR archives, built here
  with ninja in `xnu-deps/build/mesa/build-softpipe-freedreno`.
- `MiniIOMobileFramebuffer.cpp` keeps `hakios_fb_publish_refresh()`. Upstream's
  `fb_beam` probe returns on the command-mode THYME panel before it reads the
  DSI interface, the access that panicked `measurePanel`.

The merged kernel `97fcecd0` and image `b7e0f382` pass the canonical cold RAM
run `logs/114117-run.log`: two Wi-Fi joins, DHCP `192.168.0.102`, Safari TLS,
85 LE reports (15 named), 735 `hakios-gl` frames, zero H4 faults and no panic.
The merged jiggle run `logs/115200-run.log` keeps the 90 Hz pacing: 452, 451,
451 and 451 frames in four consecutive five-second windows, an 11.1 ms median,
1022 `hakios-gl` frames, and `fb_beam stays off` at 173.9 s.

| File | SHA-256 |
|---|---|
| `logs/114117-run.log` | `2f95c123e6add2e6cc8babe96bb68f0ad17784a3eef6a0309f8bc6e78e3cb4dc` |
| `logs/115200-run.log` | `b3e9e2bd773e8084a192b0d8e4312ca9139607f07b878639538a9647d56dc573` |
| `logs/jiggle-gl.png` | `cb4fccec06dee608a189a2cc3d5b925f631ced498dadf5b53353098b73b4a4a3` |

## 90 Hz UI pacing and the panel-rate announcement (2026-11-05)

The panel timing sequence and the framebuffer both ran at 90 Hz, but
CoreAnimation kept drawing at 60. The framebuffer starts before the THYME
display driver switches the panel and publishes `IOMFBDisplayRefresh` once,
at the 60 Hz it sees then; it never announced the new rate.

Two jiggle runs on the same kernel differ only in the `fb_ca_hz` boot
argument. Without it, `logs/224744-run.log` presented `300 299 300 300 300`
frames in five consecutive five-second windows (60 fps; 17.1 ms median
between shown frames). With `fb_ca_hz=90`, `logs/105924-run.log` presented 451
frames in 5.000 s and again in 5.001 s (90 fps; 11.0 ms median). A Wi-Fi-list
scroll with the same override (`logs/232933-run.log`, image `5ff68b60`) held
11.1 ms medians over 400-frame windows. The rate told to CoreAnimation, not
the panel, was the limit.

The THYME display driver now calls `hakios_fb_publish_refresh()` after the
sequence is accepted. The framebuffer announces `refreshHz()` again, which is
`min(fb_hz, hakios_panel_refresh_hz)`; a rejected sequence leaves the panel
rate at 60 and the second announcement is 60, so 90-into-60 ghosting cannot
come back. `fb_ca_hz` is no longer needed.

The pre-merge run `logs/110745-run.log` (kernel `3746671b`, no `fb_ca_hz`)
presented 452 frames in 5.008 s (90 fps) and an 11.1 ms median; the merged
kernel repeats it in `logs/115200-run.log` (452, 451, 451 and 451 frames in
four consecutive five-second windows, 11.1 ms median), so the upstream merge
did not change the pacing. The panel stays at 90 Hz.

| File | SHA-256 |
|---|---|
| `logs/224744-run.log` | `9558170d155e2f5f207170d43d2e4754325bdd7fed0584b67822d59c204f7edb` |
| `logs/105924-run.log` | `648c92cbdcf1caa761f756ad529e157d0472ed4d3391b83b0cd85acbb71c0019` |
| `logs/110745-run.log` | `b28ce566d291a7aac9bb57e2ac31322235f5b7978b7178784cb3a26258a30164` |
| `logs/115200-run.log` | `b3e9e2bd773e8084a192b0d8e4312ca9139607f07b878639538a9647d56dc573` |
| `logs/232933-run.log` | `8f616d014b60a5a2cdee485c4835297427b79cbe6c0d9678793e0c6e3f51d492` |
| `logs/jiggle-gl.png` | `92b697d37101b6af39c96950c699f7c2e4725c4d4d37eba339210e8d0d6e4283` |

## Bluetooth launch order and the withdrawn restart (2026-11-05)

The 233519 boot started `bluetoothd` before the on-demand BlueTool responder;
the daemon read the local MAC but never opened the H4 transport. `mkjobs.py`
now starts `com.apple.BlueTool` (`/redwood/bin/umi_bluetool`) with `RunAtLoad`
and keeps it alive, so the responder is listening first. The clean cold run
`logs/104749-run.log` (source `8c39b6b`, image `1a8a9cf4`) spawned
`umi_bluetool` before `bluetoothd`; the H4 counters reached 70 LE reports
(12 named) with zero `H4 transport fault` lines, Wi-Fi joined twice, DHCP
issued 192.168.0.102, and the run submitted 730 `hakios-gl` frames.

A temporary `com.hakios.btstart` job killed `bluetoothd` 20 seconds after boot
to recover that race. It was withdrawn before promotion: the helper produced
no `restarting bluetoothd` line in `logs/091256-run.log`, and the image is
better without a deliberate daemon kill on every boot. `check-root.py` no
longer expects it, and the canonical image `1a8a9cf4` does not contain it.

The Bluetooth Settings page in the clean run shows the switch on and the
`HakiOS Mi 10S` discoverable footer; its DEVICES list stays on the spinner
because no known/connectable device was present (the paired OPPO headset was
off). Discovery and connection of a known headset remain covered by
`logs/214226-run.log`.

The canonical cold run `logs/111252-run.log` (kernel `3746671b`, image
`1a8a9cf4`) repeated the launch order and the full wireless gate: 83 LE
reports (12 named), zero `H4 transport fault` lines, two Wi-Fi joins with
DHCP `192.168.0.102`, Safari TLS traffic and 733 `hakios-gl` frames. Its
Bluetooth Settings screenshot again shows the switch on and the discoverable
footer, with no known device present to list.

| File | SHA-256 |
|---|---|
| `logs/104749-run.log` | `e0a45b4e46c3e12db60d20df8fa21e303ec658dad35b1c929d5d0f54364eb8f0` |
| `logs/111252-run.log` | `d1f9bb3df5d75247dd18dff136c03a3f175d80d7ac6a5ede3baa19c2bd9c58c2` |
| `logs/wirelessregress-bt.png` | `f51da0f1679d484ca3bf10cb049048a87a87de9c7f64c5a8caaf7cf0106a6387` |
| `logs/wirelessregress-wifi.png` | `036d08e5f72f026c88402cf9be4a8e46580d338c28588cb91332c1537a689381` |

## Bluetooth launch regression and fix (2026-11-04)

The canonical `202218` baseline image did not contain `/redwood/bin/umi_bluetool`
or `com.hakios.scname`. The official-main rebuild had regenerated stock
`com.apple.BlueTool`; it printed `Device not open yet, use 'device' to open it.`
and exited with status 1. `bluetoothd` started and read the local MAC, but it
never opened the H4 transport: the log had zero `H4 client connected` and zero
`iOS HCI opcode` lines, and Bluetooth Settings stayed on its spinner. The
earlier `210532` discovery evidence came from the legacy image, not from the
canonical bytes.

`userland/build.sh` and `userland/mkjobs.py` now build and install both
programs, point the BlueTool job at `umi_bluetool` when it is present, add the
Bluetooth launch set to `BLUETOOTH=1` builds, and carry the ADSP, haptic and
THYME camera-table rules the previous image relied on. The promoted scenario
`out/thyme-boot/root-wirelessfull.apfs` is
`bfb90a5811fc436f7d267b312445d9db2bdba8cceb94d19bd8ca39d22cca8e95`;
the previous image is kept as `root-wirelessfull-pre-btfix.apfs`
(`d33d40e5...`).

The new cold RAM run `logs/214226-run.log` reports:

```text
UMI BlueTool: command=RunBuiltinScript script=boot result=0
UMI BlueTool: command=RunBuiltinScript script=init result=0
UMI BT: H4 client connected uid=501 unit=1
UMI BT: transport status=0x41000 ... le-events=315 le-reports=315 ... le-named=66
```

There are zero `H4 transport fault` lines. The final Settings screenshot shows
Bluetooth on, the footer `This iPhone is discoverable as "HakiOS Mi 10S"`, and
`OPPO Enco Free4` connected in MY DEVICES; Wi-Fi remained joined to `1701`.

| File | SHA-256 |
|---|---|
| `logs/214226-run.log` | `257d572577e85fafd10a9e09ea54d6aad387b4dce0a72ec83e8980b7465c5952` |
| `logs/wirelessregress-wifi.png` | `25750480fb125d4c0ef27b0163d7ab7205b1c12cbd9c14b6a1eb19862666f200` |
| `logs/wirelessregress-bt.png` | `91a5fd8a34a8d59c9b698614a6b5ebefd6fcfae6a40703a7fab69f4258dacbfe` |

This restores discovery and controller operation to PASS on the canonical
image. HFP/SCO, AVRCP controls and microphone use remain separate checks.

### Bluetooth off/on recovery (2026-11-04)

A focused `rc.jobs.bttoggle` run (`devices/thyme/bt-toggle.sh`) pressed the
Settings switch, confirmed `Turn Off`, waited, and turned it back on. The
kernel log records:

```text
UMI BT: H4 client connected uid=501 unit=1      (boot)
UMI BT: H4 client disconnected unit=1           (off)
UMI BT: H4 client connected uid=501 unit=1      (on)
```

Scan commands resume after the second connection and there are zero
`H4 transport fault` lines. The final screenshot shows the switch on and the
Settings footer again. This is PASS for controller and H4 transport recovery.
Headset reconnection after the cycle is still a separate check: the headset
was not present in this run, while `logs/214226-run.log` shows an OPPO Enco
Free4 connected on a normal boot.

| File | SHA-256 |
|---|---|
| `logs/221636-run.log` | `71eac224f5897ec3fd9c94cfc7e51a379cc5a1cea5d1308a639346afab83250a` |
| `logs/bttoggle-on.png` | `51721d7c1989c1fd3e09d144850616722209709f0e48d1ac274313f419337569` |
| `logs/bttoggle-final.png` | `ee87eb291182a1dcfb4e654aae8f3ac9422a4a0a789bc6f578fd01addfbf4631` |

### GPU userspace omission and guard (2026-11-04)

The first Bluetooth-fix rebuild (`bfb90a58`) omitted `GL=1`. The image had no
`libhakios.gl.dylib`, `libOSMesa.8.dylib` or `gl_test`, the log had zero
`hakios-gl` frames, and the UI fell to a slow path: the 5-second windows in
the audio run held 50-168 frames with p90 intervals near 100 ms. The kernel GPU
was not hung; it reported `THE GPU STAYS ON`, FD650 and 587 MHz.

Rebuilding with `GL=1` (candidate `46954054`) restored 940 `hakios-gl` lines.
The jiggle animation presented `300 299 300 300 300` frames in five
consecutive five-second windows (60 fps, the same pacing as the 60 Hz
baseline); the only `softpipe` context is after the run deliberately hands the
GPU back. The panel remains at 90 Hz, but the measured SpringBoard animation
is 60 fps; a 90 fps UI would be a separate CoreAnimation frame-cap check.

`devices/thyme/build-root.sh` now fixes `GL=1 ADSP=1 BLUETOOTH=1 REST=1`, and
`devices/thyme/check-root.py` refuses an image missing the GL userspace, QCA
responder, `scname`, ADSP/haptic/camera firmware or Bluetooth jobs. `bfb90a58`
is not a daily-driver baseline; the canonical image is re-promoted after the
full wireless GL run.

| File | SHA-256 |
|---|---|
| `logs/224744-run.log` (jiggle) | `9558170d155e2f5f207170d43d2e4754325bdd7fed0584b67822d59c204f7edb` |
| `logs/jiggle-gl.png` | `8272bd02a3a4170f0aee79c9880604d4591fe784e73adabfa283b0d55785500a` |

## Current-build regression (2026-11-02)

The official-loader RAM run `logs/143146-run-cpoem.log` reached SpringBoard
but reproduced a device-wide regression:

```text
UMI FTS: board mismatch
display: brightness board mismatch
UMI keys: board mismatch; driver not started
```

The loader omitted `/chosen/hakios-board`, so every THYME driver that rejects
another board (touch, keys, brightness/thermal and the QCA6390 Wi-Fi path)
remained disabled. The run also had no charging bolt and no usable Wi-Fi or
Bluetooth pairing. Bluetooth firmware and HCI reset were still visible in the
kernel log, so that observation does not invalidate the earlier low-level
Bluetooth evidence.

The fix in commit `66500d7` publishes the selected platform as
`/chosen/hakios-board`. A SpringBoard battery-state-refresh change is in the
same commit. Neither fix is a hardware `PASS` until a new RAM run proves the
drivers start and the user-visible charging state updates.

Tested artifacts:

| Artifact | SHA-256 |
|---|---|
| `devices/thyme/prebuilt/xnu-usb.img` | `a559fa38ab4e684459dd9f4d49066486da5ce734038ae723b17c9f632d2d93be` |
| `hakios-kernel` | `10f6b1c18a5fe2e45b5239326b1b681ca41ac9406f17e45193bcc736ad689331` |
| `devices/thyme/prebuilt/system.apfs` | `fae315dd2ec82a5eb6100c6cc23c11ca5a39a453188932545bdefa1913f55ad4` |

Post-fix RAM test image:

| Artifact | SHA-256 |
|---|---|
| `out/thyme-boot/root-battery-ui-2.apfs` | `11a23b1b9db35d58a3d732e941b7b5f3a69162624511a23ccb30623cf061df58` |

## Canonical regression after UFS enablement (2026-11-03)

The tested source revision `0c7b589e` on `main` was cold-booted from RAM with the pinned
UEFI-capable image and the complete wireless scenario. The worktree was clean,
and the kernel (`66a3008f02ab`), boot image (`2d662c991a5a`), loader
(`7bc139e4b1e4`), system image (`fae315dd2ec8`) and scenario
(`373a75aeac33`) all matched `ram-baseline.conf`.

One boot completed:

- 5 GHz WPA2 authentication, association, four-way handshake and key install;
- DHCP address `192.168.0.103`;
- Wi-Fi power off, interface removal, power on and radio restart;
- a second scan and second successful association;
- Safari opening `https://example.com`;
- Bluetooth Settings displayed the nearby `LAPTOP-69NFHNVP` device after
  discovery;
- the kernel counted 54 scan responses and 52 named advertisements at the
  screenshot point, with zero queue or enqueue errors.

Evidence: `logs/162111-run.log` (SHA-256
`24a8241c0bbe1792557bb48cff4a1d2f811aa7ad8c5fc374c3ebff7a7e470e6a`),
`logs/baseline-final-wifi.png` (SHA-256
`21808cfe32feeb414033598264cd6b464712089c329e9b2edfdb6a1067fea73c`) and
`logs/baseline-final-bt.png` (SHA-256
`55d9cf7dda64accab7d862896ea74d4a407d29dccf95c28f3f9ac300b6a2707c`).
The Bluetooth screenshot was taken 20 seconds after Settings opened; it shows
the discovered laptop, not the transient page-entry spinner.
The later radio teardown in the log is the intentional
`loader_return_after` shutdown, not a Wi-Fi failure.

A separate full-system power-key run pressed the lock key at 90 and 100
seconds: `backboardd` reported `displayState: off` and `Set Blanked: YES`,
then the second press produced `Main will unblank` and `Set Blanked: NO`
with `changeSource: LockButton`. Evidence: `logs/075530-run.log`, SHA-256
`0d91ebdba4e4615eb7314180d0b288f22515392592eaf73ba9a93d951c61b85d`.

The regression command now uses `out/thyme-boot/root-wirelessfull.apfs` by
default and refuses any kernel, boot image, UEFI, loader, system image,
scenario or boot-argument hash that differs from
`devices/thyme/ram-baseline.conf`. This prevents a supposedly fixed feature
from being tested with stale local artifacts or a Bluetooth-only scenario.

### Bluetooth identity and headset pairing (2026-11-03)

The Bluetooth Settings footer was not derived from the HCI local name or
`MGCopyAnswer("UserAssignedDeviceName")`. Disassembly of the iOS 16.5
`BluetoothSettings` bundle shows `-[BTSDevicesController
tableView:viewForFooterInSection:]` calling
`SCDynamicStoreCopyComputerName(NULL, ...)` and inserting the returned string
into `DISCOVERABLE_IPHONE`. That API returned `NULL`, which is why the UI showed
`(null)` even while HCI readback reported `HakiOS Mi 10S`.

`com.hakios.scname` now runs at boot and uses the private but present
SystemConfiguration preferences API to set `ComputerName`, then commits and
applies it. The job is idempotent and retries if it exits unsuccessfully. In the
cold run it reported:

```text
scname: preferences lock=1 set=1 commit=1 apply=1
scname: write "HakiOS Mi 10S" preferences=1 dynamic=1
```

The stock Settings footer then rendered `This iPhone is discoverable as
"HakiOS Mi 10S" while Bluetooth Settings is open.` The user also confirmed on
the current RAM boot that a Bluetooth headset connects and Wi-Fi remains usable.
This makes discovery, pairing and connection PASS. Headset audio routing,
microphone use and reconnection after a deliberate Bluetooth power cycle still
need instrumented validation.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/210532-run.log` | `d15603d5db14f911754beb7d12c631b442cac09119cca4c92ea0acfab9d3f9f0` |
| `logs/clean-main-wifi-210532.png` | `e7c0669c68f4b122b705f5504c96971bb0fed1851351f80f15de6d2b47f0c1a5` |
| `logs/clean-main-bt-210532.png` | `d0b3078c35bc0dd7e01874973a69e70916246f0a190f87e9f7ab75e073f1a70f` |

### Bluetooth audio route (2026-11-03)

The reduced system image did not include the stock Bluetooth audio HAL. A local,
Git-ignored copy of `BTAudioHALPlugin.driver` extracted from the matching 16.5
IPSW is now overlaid through `/redwood/hal` for both RAM-root and flash-root
boots. The missing `com.apple.BTServer.le`, `.avrcp`, `.map` and `.pbap` jobs,
plus `bluetoothuserd`, are now part of the Bluetooth job set.

On the physical phone, a focused run waited for the paired headset and observed
the system route change to `BluetoothA2DPOutput`; AudioQueue then accepted a
3-second test tone with `144000/144000` frames handed over while the A2DP route
remained selected. This is PASS for HAL loading and A2DP routing.

At 115200 8N1, direct listening found continuous hoarse, crackling audio.
The QCA6390 path now sends the Linux-compatible 3 Mbaud vendor command,
reprograms the QUP root to 48 MHz and updates the NVM HCI tag before firmware
download. The user confirmed both a ringtone and ordinary Bluetooth audio were
clean with this transport. A2DP audible quality is PASS at 3 Mbaud, which is
now the default. `bt_baud=115200` remains as an explicit rollback.

One later default-speed boot did not reconnect the headset within 150 seconds;
automatic reconnect is still an independent issue and is not counted as an
audio-quality failure.

AVRCP controls, HFP microphone/SCO, Bluetooth-off/on recovery and reconnection
after a deliberate power cycle still require separate checks.

`AVAudioSession` still returns `OSStatus -50` when an application explicitly
sets `AllowBluetooth` or `AllowBluetoothA2DP`; selecting Playback without those
options succeeds, and the system-selected A2DP route remains active. This API
compatibility gap remains open.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/024949-run.log` | `291723a6aa6a6ee40202e22460e4e3b4ba2dc46abb6ff4a3db85d5f60c802307` |
| `logs/wirelessregress-wifi.png` | `dc1d814b6edbd5af963e7bd0c7004954c1266b0512f64da04cf24ab88c36a60b` |
| `logs/wirelessregress-bt.png` | `cfb8ed1fd6fdab808958a36570204012a082a513d3f1e501e66ff051b3999470` |
| `logs/231436-run.log` | `73ba4941c833d03233c9dca59460a72f0f8e9c10781a175b1a084654ef11474d` |
| `logs/232341-run.log` | `42f29de2b2b3f1a51b692bbb7ecfd083d2274c540886ffc8abc76a1e36240f39` |

### Chinese UI (2026-11-04)

The system image previously retained only English `.lproj` resources, so the
language-change notification fired but the UI could not switch. The image
selection now keeps Simplified/Traditional Chinese resources and PingFang
Medium faces. The automated language gate selected 简体中文 and Settings
rendered the whole page in Chinese, including region and date format labels.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/022305-run.log` | `a6490eb47b0efc3102c8285d60f2c1ec719b8b9c941a61766b23c96ed577d282` |
| `logs/thyme-zh-hans-final.png` | `0f62b092635f95837527778d59c4bb29d0b81fde889599a35b8f5407af5a5d8a` |

### UI animation frame pacing (2026-11-04)

The `rc.jobs.jiggle` home-screen edit-mode animation was run with the pinned
GPU baseline. For consecutive five-second windows during the animation, the
framebuffer presented `302`, `301`, `301` and `301` frames, i.e. approximately
60 fps. The GPU path reported FD650, about 1.1-2.4 ms in the framebuffer driver
and 1.8-2.5 ms waiting for the GPU per interval. No softpipe context appeared
during active use.

The softpipe context later in the log appears only after the run deliberately
returns the GPU before shutdown and backboardd respawns. It is not a runtime
fallback during the animation.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/041711-run.log` | `f0a8d27dfa216afde5f5de4da7c81f7013156cf64de294f16bad2d9bd122c5ba` |
| `logs/jiggle-60.png` | `18221b7ce78edef5fd1b287f1ea3dd8e1933df5c65b30721d6cf74407fb0a0d3` |
| `logs/jiggle-90.png` | `fb105681ee1d2d4dea55fca296189119579eb63fc903458eb0d6bf3a9206ffb8` |

### 90 Hz panel investigation (2026-11-04)

The firmware leaves the panel in its 60 Hz mode. The ABL panel file
`work/thyme-firmware/extracted/RawFiles/Panel_J2S_MP_42_02_0a_amoled_dsc_cmd.xml`
sets `DSIRefreshRate` to `0x3C0000` (60<<16), and the live DTB's
`qcom,mdss_dsi_j2s_mp_42_02_0a_dsc_cmd` node has timing@0 at `0x3c` (the `f0`
vendor page, a `b4` timing table, then `2f 01` and `26 01`) and timing@1 at
`0x5a` (a different `b4` table, then `2f 04` and `26 02`). A bare `fb_hz=90`
therefore only makes the framebuffer produce more frames into a 60 Hz scan.

Two diagnostics are unsafe on THYME and must not be used until fixed:

- `fb_measure=1` panicked in `measurePanel` (SError) while reading the INTF1
  frame counter; symbolization of `logs/100621-run.log` points at
  `measurePanel+0x40`. The command-mode block needs the same clock/power check
  the GPU `display_probe()` does, and the frame counter is not the TE counter.
- `display_probe=1` also stopped before user space (`logs/102923-run.log` ends
  at the loader banner). The same INTF access is the likely cause.

A gated implementation is in `drivers/display/hakios.thyme_display.c`. The full
DTB sequence is accepted at the DSI level when the 2f/26 commands are sent as
short packets: `logs/192355-run.log` shows `F0 page`, `B4 table`, `2f 0x4`,
`26 0x2`, `panel timing 90 Hz seq=0 (ok)` and `brightness ready (ok)`. The
earlier hang was caused by sending 2f/26 as long packets, which wedged the
command DMA and made every later brightness command `skipped`
(`logs/190626-run.log`).

The full sequence is now the RAM baseline: `fb_hz=90` is in
`ram.args`, `CONFIG_FB_HZ=90` is in `thyme.config`, and the display driver
retries the sequence. The framebuffer follows `hakios_panel_refresh_hz`: the
display driver raises it to 90 only after all four packets are accepted, and
the framebuffer clamps back to 60 when they are not, so a failed switch no
longer produces the 90-into-60 ghosting that the seq=1 runs showed.

`logs/200350-run.log` is the promoted full run: `panel timing 90 Hz seq=0
(ok), framebuffer rate 90 Hz`, `brightness ready (ok)` and `bluetoothd: Got
local-mac-address`; `logs/reliable-bt.png` shows the Bluetooth Settings page

**The controller behind the panel refreshes it at about 60 a second, whatever
`fb_hz` says** (2026-11-06). The DSI status bits 0 and 2 move with the
command DMA; counted over a measured second (`panel_te_rate=1`, kernel clock,
no sleeps) they change 118 times, i.e. 59 events — and the same 118 with
`fb_hz=60` or `90`, with the panel timing switched to 60 or 90. The count is
therefore the controller's own auto-refresh cadence, not the framebuffer's
writes: the firmware leaves the panel refreshed from its memory at ~60 Hz,
so pacing the framebuffer at 90 puts 90 frames into a memory pushed 60 times
a second — 3:2 judder and torn frames, the "stutter and ghosting while
scrolling" the user reported. TE is not routed here (pin 66 reads 0) and the
INTF block that would say more is unsafe to read on THYME, so the panel's own
scan cannot be measured directly. True 90 Hz needs the controller's refresh
reprogrammed (not just the DCS 2f/26 switch); until then the daily boot uses
a self-consistent **60** (`fb_hz=60` in `ram.args`); set `fb_hz=90` back for
the old behaviour.

The push itself is the MDP ping-pong's auto-refresh (`MDP5_PP_AUTOREFRESH_CONFIG`:
bit 31 enables it, bits 0-15 count the TE events between frames), and the TE
it counts is the panel's own — so ~59 pushes a second says the panel still
scans at 60 even though the DSI accepted the `2f 04`/`26 02` switch (it goes
out in LP mode through the same command-DMA path as the working brightness
writes, with the exact DT sequence). Next suspects: a latch condition the
vendor meets with `b5 84` (it follows `2f`/`26` in every nolp sequence) and a
TE that may not be routed at all (pin 66 reads 0). Reading the PP block is the
way to see the count and what TE it waits for. Sending the vendor's `b5 84`
after `2f`/`26` changes nothing either (119 a second, `logs/025552-run.log`):
the switch is accepted but does not move the push.

### The LP-mode switch and the 90 Hz daily default (2026-11-09)

The device tree marks the timing-switch command `dsi_lp_mode`, and the command-DMA
control that carries it is `DSI_CMD_DMA_CTRL` at `0x38`, not the `0x3c` the driver
had been writing; bit 26 is the LP bit. The switch now sends the tree's whole
sequence — F0 page 0, B4, C1, F0 page 3, BB, then `2f`/`26`/`b5` — with that bit
set (`465dfe6`), and the daily scene requests 90 Hz again (`8befdf8`).

The cold gate `logs/005313-run.log` (kernel `9b48a381`) shows every step accepted,
`panel timing 90 Hz seq=0 (ok), framebuffer rate 90 Hz`, two Wi-Fi joins with DHCP
`192.168.0.114`, Safari, the Bluetooth page and `hakios-gl` frames; the frame
intervals cluster at the 11 ms beat.

The sequence was then corrected to the panel's own node (`60c369b`). The live
device tree carries several 42 02 0a panels; the one this phone runs (J2,
panel-id `0x4a32`) has timing@0 60 Hz and timing@1 90 Hz, and its
timing-switch-command is F0 page 0, the mode's B4 table, `2f`, `26` — the C1,
F0 page 3 and BB writes belong to the J1S nodes and are gone, as is the `b5`
latch. A run with it (`logs/014057-run.log`, kernel `c329d30a`) logs every step
accepted, `panel timing 90 Hz seq=0 (ok), framebuffer rate 90 Hz`, and returns
cleanly; the full gate on the same kernel and source (`logs/014626-run.log`,
source `60c369b`) repeats the two Wi-Fi joins with DHCP, Safari, the Bluetooth
page and the accepted switch, with no panic. A 21-minute soak on the 90 Hz
daily scene (`logs/021747-run.log`, 1250 s, same kernel) passed: no panic, the
hottest core 65.4 °C with the rows unchanged, the idle scene's 20 frames a
second with 0 swaps waited for the one before, 0 given up and 0 too late for
the next scan, and an ordered teardown (the audio DSP stopped and TrustZone
shut it down); `backboardd`'s SIGSEGV is the return's known one.

The panel's TE pad is not a readout: with the loader's own TE-on (`35 00` is in
its init) and a whole-second edge count (`panel_te_probe=1`), pin 66 reads 0
edges, so the pad does not carry the pulse on this board. The vendor's disp-rate
GPIO (pin 50, which its driver holds high) hung the kernel when driven from the
probe and stays opt-in. The panel's own scan rate therefore remains unmeasured
by us; the user's check of scrolling feel is the deciding evidence.

A 32-minute soak on the then-current 60 Hz default (`logs/035725-run.log`, 2026-11-06)
passed: no panic or watchdog reset; a steady 20 frames a second idle with
every swap on a beat and none dropped; the GPU at 631-635 us a submission,
98 MB of buffers kept and none made; the hottest core 70.5 C with the rows
unchanged (no throttle); an ordered teardown (ADSP stopped, TrustZone shut
it down). The `backboardd Corpse allowed`/`EXC_CORPSE_NOTIFY` lines at
+1919 s are the known teardown artifact after the GPU is given back, as in
the previous official soak, not a runtime restart.
(the pane still initialising). The user confirmed on the physical phone that
the full-sequence UI is smooth with no dropped frames. The panel's TE rate is
inferred from this user-visible result; the INTF TE block is still unsafe
(`display_probe=1` stops before user space, `logs/193357-run.log`), and
`fb_measure=1`/`display_probe=1` must not be used on THYME.

The Bluetooth observation in the short-packet test was an image issue: the old
`root-jiggle-gpu.apfs` has no `fw/qca6390-bt`, so `bt_qca_firmware()` returns
`ENOENT` (`probe result=2`). `root-wirelessfull.apfs` carries the firmware and
starts `bluetoothd` with the local MAC (`logs/200350-run.log`).

Evidence for the promoted run:

| File | SHA-256 |
|---|---|
| `logs/200350-run.log` | `127a44a6333235700090af1e005c97741a70cae57ee4ffe0866c4b4fa23c7a72` |
| `logs/reliable-wifi.png` | `e1df5d9a7536a3db990868d191e90242b810b5a8abd52a1becef914ed1410bf6` |
| `logs/reliable-bt.png` | `cf95bbb8dcd00211010b7eacfcb3200c08c0ceddad2f22803f675c1966bc0fa3` |
| `logs/202218-run.log` (canonical baseline repeat) | `88562406b9a14ab28e483a905786d70d1b409dc462f67457d16ebe7b87730b80` |
| `logs/wirelessregress-bt.png` (Bluetooth pane initialising) | `71cb558ca126619993c36bf25deb0f2039d4869734be44a59afde135ee42c2c9` |

### CPU-monitor log flood and Bluetooth build repair (2026-11-04)

The XNU CPU-monitor denial path logged every refused request. During a boot,
lsd alone produced tens of thousands of identical lines. The current kernel caps
this at eight detailed lines plus one suppression notice; `logs/063408-run.log`
shows exactly that pattern.

The Bluetooth Settings regression on freshly rebuilt images was a build and
service-publication problem, not a failure of QCA firmware or HCI:

- the published `compatible` value was the wrong placeholder;
- the service was attached at the platform root instead of under the DTM
  `qca6390` node;
- the default userland omitted `libhakios.bluetooth.dylib` and the
  `com.apple.bluetoothd` injection line;
- `mkjobs.py` dropped the Bluetooth jobs and still described 115200 baud;
- the build did not package `fw/qca6390-bt` patch/NVM files.

Those are corrected. With explicit `ALLOW="bluetooth BTServer"` the jobs keep
their `RunAtLoad`/`KeepAlive` behavior, the H4 transport advertises 3 Mbaud,
and the firmware directory is included automatically. `logs/084844-run.log` proves
the full cold path: QCA patch/NVM accepted, H4 endpoint ready, QCA compatibility
injection enabled, `bluetoothd: Got local-mac-address` and no
`No bluetooth on this device`. The same boot completed Wi-Fi association,
CCMP keys and DHCP at 192.168.0.102.

The RAM baseline now promotes kernel `407df1a44556` and scenario
`d33d40e54b77`; the system image remains the known-good `4f0f3f8b1ba0`.
A separate warm-reinitialization issue remains: after an earlier successful
3 Mbaud session, a warm reset can leave QCA UART/firmware state such that the
next run reports error 2 or 92 before daemon startup. Cold boot remains the
reliable path; a reset/recovery protocol is the next Bluetooth stability task.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/084844-run.log` | `f7de08e66f302af1d5b43fb639a6f97e5b136b57076bc4b5c6d5a8180c7edb19` |
| `logs/063408-run.log` | `9c06818d7297109f9ecd714213a33b4042743168813970c424eef37e0a53dc1f` |
### Historical unnamed-accessory diagnosis

The HCI trace now records the address in the LE white-list add/remove commands.
This closes the ambiguity in the earlier pairing failures:

```text
LE whitelist add address-type=0
LE whitelist add 0 address=<anonymous advertiser>
LE enhanced connection complete status=0 handle=1
LE enhanced connection peer 0 address=<same anonymous advertiser>
disconnection complete status=0 handle=1 reason=0x3e
```

A Windows Intel Bluetooth controller was then used as a known connectable BLE
peripheral at approximately `-40 dBm`. HakiOS still selected the anonymous
advertiser at approximately `-98 dBm`; the Windows peripheral was not placed in
the white list. The failure is therefore in iOS's choice of the single
unnamed `Accessory` row, not in HCI event delivery, queueing or Wi-Fi/Bluetooth
coexistence.

The Windows GATT provider does not publish a local name through this path, so
iOS collapses nearby unnamed connectable advertisers into one `Accessory`
entry. A named, known BLE peripheral remains required for a valid end-to-end
pairing result.

A temporary diagnostic that replaced the white-list address with the strongest
connectable advertiser did reach the Windows radio, but `bluetoothd` exited
immediately because the connection-complete address no longer matched the peer
iOS selected. That experiment was removed and is not part of the baseline.

## Wireless startup and coexistence (2026-11-02)

The board-gating fix in `66500d7` is now verified on the physical phone:
touch and charging indicators work. Wi-Fi was also validated with the full
SpringBoard workload after the QCA6390 mission-mode barrier in `513489d`:
`net_test` returned three access points, and the stock iOS Wi-Fi Settings page
displayed real nearby networks (`logs/182354-run.log`,
`logs/180902-ui-65.png`). This proves the radio, scan path and user-space scan
display can all work together; it does not prove stable discovery on every boot.

Bluetooth firmware, HCI, the iOS H4 endpoint and LE reception are verified
independently. The controller reported seven to eight LE advertisers both with
Wi-Fi off and with Wi-Fi in mission mode. The full regression run
`logs/204958-run.log` then displayed an `Accessory` in stock Bluetooth Settings
while Wi-Fi remained usable. The pair attempt in `logs/212033-run.log` was
accepted by the controller as LE Create Connection (`0x200d`, command status
0), but no LE Connection Complete event followed before iOS timed out. The
failure is therefore in LE connection establishment or the peer, not H4 event
delivery, Wi-Fi coexistence, or missing discovered devices.

A clean-main rerun on 2026-11-02 (`0f28e9c`, clean worktree) again brought Wi-Fi
up, returned three APs through `net_test`, and displayed real networks in stock
Settings. The exact build and artifact set is pinned in `BASELINE.md`. A later
user-visible failure is treated as a boot/artifact mismatch until the recorded
source, worktree, image hashes and boot arguments are compared.

| Wireless test artifact | SHA-256 |
|---|---|
| `logs/201710-run.log` (clean main) | `e180cfa3ea5ff13a842780c7f1740f5791caf5b98829bdaa04e134781027b8b8` |
| `logs/clean-main-wifi.png` | `220bf17333495c8a3c6c3347aefb01bf486d0303410d84294ae2bd103a574fe3` |
| `logs/183439-run.log` (Bluetooth only) | `5d9eebe1c34fe8c948764634c19d4b6ce537e4b830ff910b1aee27a4f0206dc9` |
| `logs/194622-run.log` (Settings spinner) | `c8a572b4e6d8a198212c0b94fb83115c71dd19ce35788237d8459508ba5b238d` |
| clean-main `hakios-kernel` | `86a4f3423e1858ef27e51790ef6da54611cb6714db3ad7ffd2e19f7e74c99cff` |
| `logs/203537-run.log` (Wi-Fi power cycle) | `075d5f2d3622e27a10d335a61aae3282076cbe9ddbe0c6516c853822ef6a413a` |
| `logs/wifi-after-toggle.png` | `16b0cfee4034ce277d65c8412d69f240dccd8ad660f9fd06599f525ef82f6091` |

## User-priority hardware gate

These are the minimum user-facing hardware checks for the current THYME port.
No item moves to `PASS` from source inspection alone.

| Item | Status | Evidence / next action |
|---|---|---|
| Charging while iOS runs | PASS | Full 30-minute run stayed connected; the charging indicator was also confirmed by the user on the current RAM boot |
| Battery percentage display | PASS | The current status bar shows a stable percentage and charging state; kernel telemetry remains valid |
| Speaker acoustic output | PASS | Known 1 kHz tone is present in microphone captures only while `UMI SPEAKER TONE NOW` runs; median 1 kHz level rises by 25.78 dB (`docs/official-speaker-mic-loopback.md`) |
| Dual-speaker stereo balance | PASS | Independent left/right channel separation and acoustic balance verified on Mi 10S (`logs/101157-run.log`, `logs/stereo-lr2.wav`, `sound=3 sound_channel=3`): both CS35L41 amps load per-unit calibrations (`0x40` bottom/right `R=8942`, `0x42` top/left `R=8534`); 1 kHz Goertzel at bottom mic `ADC1` rises from `-60.30 dBFS` baseline to `-47.47 dBFS` (`+12.83 dB`) during 3 s Left-only, drops to `-62.26 dBFS` during the 2 s silence gap, and rises to `-42.04 dBFS` (`+18.27 dB`, `+5.44 dB` louder at bottom mic than top speaker) during 3 s Right-only (`159/159` periods each, `0` underflows) |
| Vibration | PASS (hardware BEMF + iOS CoreHaptics verified; tactile feel subjective) | AW8697 continuous-mode BEMF (`+47/-200`, `+48/-200`) matches Android stock (`+45/-202`), and iOS CoreHaptics (`sound_test haptic 0.3` and SpringBoard UI interactions) drives `/dev/hakios-haptic` -> AW8697 end-to-end (`logs/192300-run.log`: 17 taps + 4 vibrations; re-verified `logs/095159-run.log`: 6 taps + 277 ms continuous vibration, AW8697 state 7 -> standby). Subjective physical feel remains for the user |
| Bluetooth | PASS for discovery and connection on the canonical image; A2DP verified earlier; HFP/reconnect open | The 2026-11-04 fix restored `umi_bluetool`, `scname` and the H4 transport; the canonical run reports 315 LE reports with zero H4 faults, shows the `HakiOS Mi 10S` footer and reconnects the OPPO Enco Free4 (`logs/214226-run.log`). The QCA responder now starts at boot before `bluetoothd` (`mkjobs.py` RunAtLoad; `logs/104749-run.log`). The earlier 3 Mbaud A2DP result remains valid for the legacy image. HFP/SCO, AVRCP, microphone and automatic reconnect remain unverified |
| Wi-Fi discovery/startup | PASS | The current run opened Wi-Fi and returned real 2.4 GHz and 5 GHz networks; the user also confirmed Wi-Fi remains usable on the current RAM boot (`logs/210532-run.log`) |
| Wi-Fi power off/on recovery | PASS | The current run switched Wi-Fi off (`set 19`, interface removed), then on again (`set 19`, radio restarted), scanned, associated and retained real network traffic (`logs/210532-run.log`) |
| Wi-Fi association/DHCP | PASS | Both current-run associations completed the four-way handshake and installed pairwise/group keys; DHCP assigned `192.168.0.103`, and en0 traffic remained usable after the power cycle (`logs/210532-run.log`) |
| Wi-Fi TCP/TLS and Safari | PASS | Safari opened through en0; DNS, TCP and TLS connections were established after each association (`logs/210532-run.log`) |
| Combined wireless regression | PASS | The current GPU-enabled cold boot stayed at 3 Mbaud, completed two Wi-Fi associations with CCMP and DHCP, carried Safari traffic, submitted hardware GL frames and reached Bluetooth Settings discovery without H4 faults (`logs/024949-run.log`). The 2026-11-05 canonical run repeated this: two joins, DHCP `192.168.0.102`, Safari TLS, 83 LE reports and 733 GL frames with zero H4 faults (`logs/111252-run.log`). The merged kernel repeated it with 85 LE reports and 735 GL frames (`logs/114117-run.log`). The kernel with the sensor framework's sleepstate entry repeated it on 2026-11-12: association accepted with the keys given, DHCP `192.168.0.102`, Safari/DNS on en0, LE advertising reports with names and the footer `HakiOS Mi 10S`, zero H4 faults, 806 `hakios-gl` frames, thermal nominal at 45–55 C and no panic (`logs/170916-run.log`, SHA-256 `dbaf5287d57d561d3d424c94796405fb600a8e2c395a35bd6778028f7929621a`); the probe kernel with the sensor arguments repeated it the same evening with 804 `hakios-gl` frames, zero H4 faults and no panic (`logs/173630-run.log`, SHA-256 `cf3892e30d7fc27c765239ab45eb8844fe88e19284fd41e979d8601aaacd73cf`); the listener-timeout kernel repeated it with 729 `hakios-gl` frames, 165 LE reports, zero H4 faults and no panic (`logs/191216-run.log`, SHA-256 `63f38e2f2623e2b8e7807be940715ea4386e40b71970984f6a9c726f6f032bf6`); the registry-channel kernel repeated it with 807 `hakios-gl` frames, zero H4 faults and no panic (`logs/235150-run.log`, SHA-256 `9ec0053bf88426e31c02f73f13906f66e2f47961bf08ffe52ab665ce8099098c`) |
| CPU | PASS | 7 CPUs start and all three EPSS domains raise to 1804/2419/3187 MHz |
| GPU acceleration | PASS | With `gpu=1` and the `GL=1` image, the THYME Adreno 650/GMU path reports `THE GPU STAYS ON`, Mesa/FD650 initializes and the current run submits more than 760 hardware `hakios-gl` frames with zero `softpipe` fallback (`logs/024949-run.log`). The 2026-11-04 first rebuild omitted `GL=1` and submitted zero `hakios-gl` frames; `devices/thyme/build-root.sh` and `check-root.py` now refuse that image (the GL jiggle run presents 300 frames per 5 s, `logs/224744-run.log`) |
| UI animation frame pacing | PASS at 90 Hz | The re-announced panel rate drives the GPU-backed jiggle at about 90 fps: 452, 451, 451 and 451 frames in four consecutive five-second windows and an 11.1 ms median after the upstream merge (`logs/115200-run.log`); the same animation was 60 fps before the fix (300 frames per 5 s, `logs/224744-run.log`) |
| 90 Hz panel refresh | PASS (global 90 Hz verified across apps and sleep/wake) | LP-mode timing sequence accepted (`seq=0 (ok), framebuffer rate 90 Hz`); `hakios_fb_publish_refresh()` updates both `publishModes()` and `publishRefresh()`, so `CADisplayMode.refreshRate = 90`, `UIScreen.maximumFramesPerSecond = 90`, and `tl: a` ticks steady 11.11 ms before and after Display Sleep/Wake (`logs/032733-run.log`, `logs/034010-run.log`, `logs/100533-run.log`, `logs/101254-run.log`). `init.c` exports `CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0`, SpringBoard defaults `anim_max=1`, and kernel `@apps` exec-injection of `libhakios.hfr.dylib` is confirmed live in `MobileSafari`, `Preferences`, `MobileCal`, and `identityservicesd` (`logs/095159-run.log`, `logs/100533-run.log`) |
| Metal backend | PARTIAL: the window server and app UI draw through it; content behind VectorKit's pipeline library is still blank | The CoreAnimation plug-in (`METAL_PLUGIN=ca`, `metal_all=1`, `ca_metallib=`) serves the window server's Metal renderer: the home screen, its widgets and application UI draw (`logs/093154-run.log` and its 2.4 MB frames; Maps' search sheet, favorites and location dot in `logs/ptm-3.png`), the GPU taking the command streams whole with no `no answer`. Maps' map canvas is empty: VectorKit's `iphoneos.pipelinelib` shipped only states precompiled for Apple GPUs (`pipelines.g10p` ... `g15p`), its `functions` archive is 0 bytes, and its by-name lookups answer nil. The plug-in now opens it with an empty LPPM function cache and builds the missing pipeline from the two functions each description names (`hm_vk_new_pipeline`), not yet observed on the device. The old no-SpringBoard bisect and the Turnip/M2V path stay as recorded (`docs/official-metal-probe.md`); `gpu_recover=1`'s reset is still unverified. **A plain process gets no Metal device**: `MTLCreateSystemDefaultDevice()` answers nil as root and as uid 501, the only `IOAcceleratorES` service is `sgx` with no `MetalPluginName`/`MetalPluginClassName`, and the port's GPU is the separate `HakiosGPU` service -- the plug-in path is what draws, not a system-wide Metal device (`logs/202425-run.log`, `rc.jobs.metaldev`, 2026-11-18). No app can bring up ANGLE or Metal until a device is published to app processes. The mechanism such a device would use is spelled out in `src/metal_test.m`'s header: an `IOAcceleratorES` service carrying `MetalPluginName` and `MetalPluginClassName`, whose class subclasses Metal's private `_MTLDevice` and answers `-initWithAcceleratorPort:`; Metal loads `/System/Library/Extensions/<MetalPluginName>.bundle` and keeps the device under a dispatch_once. The port already has the switch for it: boot-arg `metal=1` makes the kernel name `HakiosMetal`/`HakiosMetalDevice` on that service (`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`), and the bundle is linked at `/System/Library/Extensions/HakiosMetal.bundle` -- it is **off by default on purpose**, because a device that cannot draw makes the window server pick Metal and fall over (the non-CA plug-in in the store images historically crashed it; the CA plug-in's `tools/metal-run.sh` is the path the plan records). A `metal=1` probe on **QEMU** (2026-11-18) shows what the switch does: the accelerator names `HakiosMetal`, the bundle loads, `HakiosMetalDevice` is registered as a `_MTLDevice` and `MTLCreateSystemDefaultDevice()` answers **"HakiOS Adreno 642L (Metal: a device that exists, nothing more)"** in a plain process; the queue, command buffer, encoders, states, textures, buffers, library (274 functions) all appear. `backboardd` is deliberately **not** handed the device at `metal=1` -- the mode numbering is metal=1 for metal_test only, 2 for all but backboardd, 3 for all (the plug-in says so) -- so the window server is not put at risk by the probe. With no GPU under QEMU the queue has no GPU addresses (`command buffer nil`, "no blits"), which is the QEMU limit, not the phone's. A second QEMU run with the **non-CA** plug-in and `metal=2` (the mode meant for apps) showed why the switch is off in the daily image: CoreAnimation logs `[WindowServer] CoreAnimation: Failed to initialize Metal or OpenGLES contexts!` and `Software renderer! Simulating crash!`, and backboardd crashes in a loop (`/tmp/vk/qemu-alpimaps.log`, 24 crash lines; the same line appears with metal=1 in `/tmp/vk/qemu-metaldev.log`) -- naming the accelerator is enough for CA to commit to Metal, and a device the window server cannot use takes the screen down. The supported route is the **CA build** (`METAL_PLUGIN=ca`, `ca_metallib=`, the path `tools/metal-run.sh` and `docs/metal-plan.md` record): such an image was built with the Alpimaps wrapper scene (`root-almapswrap` `24f654f6`, `/redwood/share/ca-share/QuartzCore-default.metallib`) together with `/tmp/vk/runnow2ca.sh`, and the phone runs wait for the charge. A QEMU run of that CA image states its requirements plainly: `HakiosMetal[backboardd]: no device: no HakiosGPU service: the kernel has not started the GPU` -- the CA device *is* the GPU path, so QEMU (no GPU) cannot carry this test; the phone must. The same log gives the last parameter: **`metal_all=1` is what hands the device to every process** -- without it the plug-in says "the window server alone is [shown the device] (metal_all=1: every process)". The phone experiment is therefore CA build + `metal=1 metal_all=1 ca_metallib=...`: the window server through the CA device on the real GPU, and HakiosMetalDevice in the app's process, where Alpimaps' `eglInitialize()` can be answered |
| Touch | PASS | User confirmed touchscreen input on the current RAM boot after `/chosen/hakios-board` was restored; FTS521 polling also reports `errors=0` |
| Brightness adjustment | PASS (manual slider + lock backlight off/wake verified; ALS auto-brightness remains PARTIAL) | Control Center brightness slider live drag sweeps `485 -> 265 -> 971 -> 857` permille with `45/45` consecutive `DCS 0x51` `(ok)` completions (`level 1097 .. 3977`, `logs/101918-run.log`, `logs/cc-slider-high.png`, `logs/cc-slider-70.png`). Lock button and idle blank forward `DisplayBrightnessFactorWithFade = 0` via `kern.hakios.backlight_factor` to true AMOLED backlight off `DCS 0x51 level 0 (ok)`, and wake restores the exact user slider level (`level 3512 (ok)`). Auto-brightness remains `PARTIAL` pending SLPI ALS |

## Official-main migration gate

- [x] Register THYME in `devices/manifest.conf`.
- [x] `tools/hakios-device.py check thyme` passes.
- [x] `make thyme_defconfig` and `make config` pass.
- [x] Official-main kernel builds.
- [x] Official-main kernel reaches launchd and SpringBoard on the phone.
- [x] Official-main kernel returns to Android safely.
- [x] Official loader and `xnu-usb.img` boot on the phone.
- Upstream check, 2026-11-18: `upstream/main`'s tip is still `63fc2b46`
  (2026-11-06, PR #53, soc-args register-cells), it is already the merge base
  of this branch, and `git rev-list --left-right --count main...upstream/main`
  reads `803 0` -- 803 local commits on top, no upstream commit outside.
  Nothing to merge until upstream moves.

## Boot and interaction

| Check | Official main | Legacy evidence |
|---|---|---|
| XNU reaches userspace | PASS | PASS |
| 7 CPUs and cpufreq | PASS; three EPSS domains at 1804/2419/3187 MHz | PASS |
| 1080 x 2340 display | PASS: native 1080 x 2340 panel at 90 Hz (`seq=0 (ok)`) renders SpringBoard and apps cleanly (`logs/101918-run.log`, `logs/cc-slider-70.png`) | PASS |
| Brightness adjustment | PASS (manual Control Center slider `265..971` permille + lock `DCS 0x51 = 0` / wake restore, `logs/101918-run.log`; ALS auto-brightness PARTIAL) | PASS in the legacy display probe |
| Adreno 650 GPU rendering | PASS, more than 2,100 `hakios-gl` frames in four minutes | PASS |
| 30-minute UI stability | PASS; continuous SpringBoard rendering, stable memory and orderly return (`docs/official-ui-soak.md`) | PASS |
| FTS521 touch polling | PASS, 415 events in the GPU run / `errors=0` | PASS |
| Tap, drag, gestures | PASS, backboardd touching/none and gesture handling | PASS |
| Power key | PASS; short press blanks the display and the next press wakes it | PASS |
| Volume keys | PASS, pressed/released captured | PASS |
| Display off and wake | PASS; `Set Blanked: YES` sends `DCS 0x51 level 0 (ok)` (OLED backlight off), `Set Blanked: NO` restores exact user brightness and retains 90 Hz (`logs/100533-run.log`, `logs/101918-run.log`) | PASS |

## Power and charging

| Check | Official main | Legacy evidence |
|---|---|---|
| Battery percentage/voltage/temperature | PASS for telemetry; with the loader path publishing the power source (kernel `0df6409a`) powerd/IOPS report the kernel's charge and the status bar follows it live (83 -> 82% while the kernel read 84 -> 81%, `logs/batterysrc-150.png`, `-360.png`, `logs/195212-run.log`) | PASS |
| Signed current | PASS, `-97 mA` during the probe | PASS |
| Positive current in HakiOS | PASS; observed 1239 mA and 106-140 mA with `charging=1`, but a positive reading is not proof of charging (2026-11-11: +1025 mA while the cell fell 3905 -> 3514 mV, `logs/133314-run.log`) | PASS |
| One-shot `TakeAction` | TODO | PASS, exactly two calls |
| 30-minute charging endurance | PASS; endpoint online throughout the UI run, 0 failed reads | PASS with endpoint online |
| Net charge under USB load | PARTIAL: SOC stayed 97% over the 30-minute UI run at +129..151 mA (and the cell rose 4082 -> 4138 mV at 98%, `logs/185653-run.log`), while the daily scene drains through a PC port: 4054 -> 3583 mV over the 30-minute soak (`logs/125243-run.log`). Whether a wall charger keeps up is untested -- it takes the USB port the host uses | PARTIAL: load can exceed a 500 mA host port |
| `battery_stop` safety guard | PASS on hardware: with the charging bit following the cell's voltage, a run at 53% and `battery_stop=95` ended after three looks and handed back to the loader at 185 s, Wi-Fi restored and the DSP stopped in order, no panic (`logs/134145-run.log`); the old cable-only bit never fired (`logs/133314-run.log`). Reproduced on the `0df6409a` baseline by `devices/thyme/battery-guard.sh` at threshold 99: the stop fired with `charging=0` and the kernel returned at 186 s instead of 420 (`logs/201116-run.log`, SHA-256 `558a75a4af82cf921c4eee4f14f858c38706ff42b208a4016eaebbda08f14ed1`). `ram.args` now defaults to `battery_stop=25 battery_stop_poll=30`; the guard returns to the loader, so `XL_REBOOT_AFTER=1` is still what sends the phone to Android | PASS |
| Thermal frequency caps | PASS; big-core domain 17 -> 16 at 75.9 C, then recovered | PASS |
| `thermal_stop` hand-back | PASS with forced `thermal_stop=30`; GPU stopped in order and loader returned without panic | TODO |
| Automatic Android return after `thermal_stop` | PASS: a forced `thermal_stop=30` fired at 37.0 °C after 2000 ms, the teardown stopped the DSPs in order, the loader returned 18 s after the kernel started, and `xl`'s `XL_REBOOT_AFTER=1` put the phone back in Android without a manual command (`logs/212426-run.log`, SHA-256 `c98eb759f36d832a283aeb0f68ef88e9679ba2498242214bd818fb04b1d5980d`; `adb` saw it back) | TODO |

## Audio, wireless, and sensors

| Check | Status | Next action |
|---|---|---|
| ADSP firmware | PASS on official main; APR services up | port and validate official ADSP path |
| Speaker playback | PASS: dual CS35L41 amps default-enabled (`adsp_umi=1`), per-unit calibrations loaded (`0x40` `R=8942`, `0x42` `R=8534`, `-12.000 dB` HALO-protected ceiling), verified on 1 kHz loopback (+25.78 dB), live VoLTE `10099` (`1,055,552` frames, `0` underflows, `logs/035640-run.log`), and independent L/R stereo separation (`+12.83 dB` L / `+18.27 dB` R, `+5.44 dB` R-L at bottom mic, `logs/101157-run.log`) | keep a regression check in future soak runs |
| Microphone capture | PASS: DEC1/ADC1 + DEC0/ADC4 captures dual-channel PCM (`rms 4854-7930`); Darwin AVAudioRecorder through `/dev/hakios-mic` ran 4.9 s with `0 frames skipped` and `0` short reads | keep a tone/intelligibility regression check in future soak runs |
| Haptics | PASS: iOS's own haptics reach the actuator; the physical feel is the owner's | The haptic scene on the current baseline (`logs/192300-run.log`, kernel `26257831`, root `9a84fa86`) ends with **17 taps and 4 vibrations played** (one cut at the kernel's 1 s) from 406,272 frames iOS wrote; the audio plug-in's actuator path played 256-frame buffers with peaks up to **32,643 of 32,768**, and the AW8697 sat in state 7 through each play. The system sounds' feedback (peek 1519, pop 1520, nope 1521), CoreHaptics and the home screen's long-press menus all arrive — the earlier 0-vibration reading was the ringer's system sound alone, which carries no haptic pattern. Repeated BEMF peaks match stock (continuous drive); BSTERRI is the vendor-ignored boost latch; F0 is advisory. What remains is a person's feel of it |
| QCA6390 PCIe/MHI | PASS on official main; firmware reaches mission mode | port and retest |
| QMI / WLFW service | PASS; QCA6390 firmware `0x1016046e` ready | diagnose any long-run reconnect failures |
| Wi-Fi station / join / DHCP | PASS on the current baseline: WPA2 5 GHz association, four-way handshake, DHCP, DNS, TLS and Safari page rendering (`logs/022344-run.log`) | test 2.4/5 GHz switching and reconnect after link loss. Display sleep/wake passes: `rc.jobs.wifisleep` joined (192.168.0.107, DHCP/DNS, `Link = { Active = 1; }`), fetched a 200 in 0.13 s, pressed the side button (`Set Blanked: YES` 10:20:48), woke it (`NO` 10:21:13) and fetched a 200 in 3.13 s with the same address (`logs/101937-run.log` and its `.live.N` snapshots) |
| Bluetooth UART firmware | PASS on official main | `htbtfw20.tlv` and `htnv20.bin` accepted |
| Bluetooth HCI reset | PASS on official main | HCI 0x0c03 completed; iOS command flow followed |
| Scan / pair / connect | PASS for scan, pairing and connection; PARTIAL for audio/reconnect | the user confirmed a known Bluetooth headset connects on the current RAM boot; next verify A2DP/HFP routing and reconnect after disabling/enabling Bluetooth |
| ALS / proximity / accelerometer | PARTIAL | the SLPI boots and its SNS client answers QMI (see below); no sensor data or HID service yet |

## User space and daily use

| Check | Status | Evidence / next action |
|---|---|---|
| Darwin launchd and core daemons | PASS in historical RAM runs | launchd starts process 1 and brings up cfprefsd, logd, runningboardd, backboardd, powerd, wifid and other selected system daemons |
| SpringBoard home screen | PASS | SpringBoard reaches the home screen cleanly on the current 90 Hz Adreno 650 GL build (`logs/100533-run.log`, `logs/101918-run.log`, `logs/clean-cc-low.png`) |
| SpringBoard settings and status bar | PASS | Status bar percentage and charging bolt (`98%`/`97%`/`94%` charging), Control Center toggles/sliders, and Settings panes verified on the current RAM build (`logs/095159-run.log`, `logs/100533-run.log`, `logs/101918-run.log`, `logs/clean-cc-low.png`, `logs/cc-slider-70.png`) |
| Settings and application launch | PARTIAL | URL openings, Settings panes and application launch navigation work in scripted runs; broad application compatibility is unverified |
| Wi-Fi Settings UI | PASS | 2.4/5 GHz WPA2, DHCP/TLS, `COUNTRY_CODE` (`51`, `CN` settled on first try, zero locale retries), `DEAUTH` (`29`), accurate link-down reason enum (`1=Deauth`, `2=BeaconLoss`, `0=None`, fixing false `SystemSleep` reports), spontaneous AP deauth kernel auto-rejoin (`2.68 s`), 65 s real locked/blanked display sleep + hardware power-button wake (`press_power=80 press_power_gap=65` -> post-wake IPv4 + DNS + HTTPS `200 OK` in `0.08 s`), radio power off/on kernel standing rejoin (`0.10 s` HTTPS), and `leave` keepalive rejoin (`0.10 s` HTTPS) all verified live on Mi 10S with zero SMMU faults or panics (`logs/171727-run.log`, `logs/173008-run.log`) |
| Bluetooth Settings/pairing UI | PASS for discovery, pairing, connection and audible A2DP | The footer renders `HakiOS Mi 10S`, devices are discovered, a headset connects and 3 Mbaud playback is clean. HFP, AVRCP controls and deliberate reconnect remain open |
| Chinese UI and fonts | PASS (default from boot) | Seeded `userland/var/mobile/Library/Preferences/.GlobalPreferences.plist` (`AppleLanguages=zh-Hans-CN`, `AppleLocale=zh_CN`, Pinyin/QWERTY/Emoji keyboards) into `var.skel`; cold boot lands directly in Simplified Chinese across SpringBoard, Settings, and CoreLocation permission alerts without tofu glyphs or reboot (`logs/104116-run.log`, `logs/105145-run.log`, `loczh-home.png`, `loczh2-loc-off.png`, `loczh2-loc-on.png`, `loczh2-locprobe.png`) |
| Rootless package manager (Sileo) | PARTIAL; offline and online repository installs PASS | The Procursus prefix grafted, dpkg installed/queried/removed tree 2.0.1 offline, and Sileo rendered. On 2026-11-08 apt fetched the Procursus index (`InRelease` 8,310 B, `Packages` 334 kB) and installed tree 2.0.1 from https://apt.procurs.us over the phone's own Wi-Fi, queried, listed and removed it, with Sileo launching (`logs/012326-run.log`). A longer interactive Sileo session and dependency repair remain | A re-run on 2026-11-11 (kernel `35d4e53f`, scene `root-sileo` `44281532`) stopped after the scene's first command, `sound_test springboard`: no `prep.sh`, `apt-get`, Sileo or `dpkg-query` line followed (`logs/111316-run.log`, SHA-256 `9fb720bf6d4e41dbd71e0ac5f28a595c39fcc13069453a7ee079689b9092d040`), so the scene image/script needs its first step fixed before the online path can be re-verified on the current kernel; the 2026-11-08 apt evidence above is unchanged. On 2026-11-11 the scene was re-run correctly on kernel `35d4e53f` with the JB system image `b7e22d95` (`system-jb-meta.apfs`) and the rebuilt scene `3d3b5b11` (`logs/115231-run.log`, SHA-256 `43f9bd40c2270a0b217ae98bff829213babac83d0b7149a7f40d9c29b7fff00b`): init grafted `/var/jb from /jb` (439 directories, 6432 files and links), `apt-get update` fetched `InRelease [8,310 B]` and `Packages [334 kB]` from https://apt.procurs.us, and Sileo launched (`org.coolstar.SileoStore`); the UI taps still installed nothing (`dpkg-query: no packages found matching tree`), the known queue-sheet problem. The scene needs the JB system image: with the daily `system.apfs` (`4f0f3f8b`) `/var/jb` is absent and `prep.sh` exits 127 (`logs/113529-run.log`). On 2026-11-12 Sileo's own source (github.com/Sileo/Sileo) explained the queue: the QUEUE button only adds the package to DownloadManager; the queue is the LNPopupController popup bar the TabBarController shows when `operationCount() > 0`, and `presentPopupController()` does nothing unless the popup is already presented; the popup bar sits above the tab bar, behind the package page's modal page sheet, so the sheet must be dismissed first. A drag must start on the sheet's grabber row (measured at y 0.060 of the screen in the timeline shots; earlier drags at 0.033 began above the card and did nothing). The scene now dismisses the notification alert from two background tap lines across the whole page-load window (its appearance tracks the page load, up to ~30 s after it and minutes after boot, which defeated every fixed tap), then queues, closes the sheet and taps the popup bar. The install from the UI is still unproven as of `logs/220916-run.log`, where the alert was still up at the GET moment. The 2026-11-13 00:25 and 00:38 runs (600 s, the guard lowered to 10 % for the run) show the alert dismissed by the background taps (`logs/sileoJ-43.png`), the GET tap repeated six times (an already-queued button only presents the popup and reloads, it does not unqueue), and the page still reading GET with its Repository row **"Untitled Repo"** afterwards (the 20:41 run that had the page show `Procursus` queued fine) -- Sileo's repository object loses its name in these boots, the same state the earlier sessions blamed for the queue path; the grabber drags still do not close the sheet either (`logs/sileoK-44.png`..`-48.png`). The repository name itself is now fixed and verified: apt writes `<repo>_InRelease` while Sileo reads `<repo>_Release`, so the scene seeds the second from the first (`userland/sileo-seed.sh`, run after `apt-get update`); the page's Repository row then reads **Procursus** in the 01:02 run (`logs/sileoL-36.png`). The queue still does not take: the same run's six GET taps (`logs/sileoK`/`sileoL` frames) leave the button at GET -- Sileo's `reloadData(recheckPackages: true)` runs `recheckTotalOps()` and, on any apt error, calls `removeAllItems()` and shows the error, so the queue is emptied the moment it is filled. The root helper's own log in the 01:19 run shows the taps reaching that path: Sileo starts its session tasks (including `https://apt.procurs.us/dists/iphoneos-arm64-rootless/1800/Release`, havoc.app and repo.chariz.com from its own source list) and then spawns `/bin/rm -rf /var/jb/var/lib/apt/sileolists`, `mkdir`, `chown` and `chmod` -- the DependencyResolver rebuilds its index, and the queue clear follows it (`logs/012034-run.log`); the scene's own apt simulation needs a 600 s run to land in the kept log. What apt inside Sileo objects to is the next thing to read. Sileo's own source (github.com/Sileo/Sileo, `Sileo/Backend/Download Manager/DownloadManager.swift` and `APT Wrapper/APTWrapper.swift`) narrows it: the queue is emptied in `reloadData(recheckPackages:)`'s catch -- `recheckTotalOps()` throws, then `removeAllItems()` and `TabBarController.displayError` -- and the apt query it makes is `apt-get -sqf --allow-remove-essential --allow-change-held-packages --allow-downgrades ... -oDir::State::lists=sileolists/ install --reinstall <pkg>=<ver>`. Two facts matter on a rootless port: `sileolists/` is a **relative** `Dir::State::lists` (Sileo prepares `/var/jb/var/lib/apt/sileolists` first, so apt's working directory must be `/var/jb/var/lib/apt`), and the root spawn (`spawnAsRoot` -> `spawn(command:args:root:true)`) sets only `PATH` -- no `chdir` exists anywhere in the app. The error alert itself has not been captured: the scenes' background taps (meant for the notification alert) likely dismiss it before a screenshot. The next Sileo run should dismiss the notification alert, tap GET, and then **stop tapping**, so the alert stays for the frame. One hypothesis the alert will settle: if nothing in the launch path sets Sileo's working directory, apt's cwd is `/`, the relative `sileolists/` resolves to `/sileolists`, and that directory is not the one Sileo prepared -- an empty lists directory makes `install pkg=ver` answer "Unable to locate package", which is exactly a thrown error. The one-line test beside the alert: as root, `ln -sfn /var/jb/var/lib/apt/sileolists /sileolists` before the GET (the boot volume is RAM-backed, so `/` is writable in a RAM boot); if the queue then takes, the cwd is the bug and a daily image should carry that link. **2026-11-19: this line was tested and disproved** -- apt resolves the relative `sileolists/` against its own `Dir::State`, not the process cwd, and `/` is read-only in this boot so the link cannot be made at all; the UI path now reaches the package page and its QUEUE tap is accepted, and the open question is closing the package sheet to reach the queue bar (see "The Sileo page is drawn; the taps are what miss" and the sections after it). **2026-11-19 03:13: an install from Sileo's own UI now works end to end** -- the queue sheet's Confirm starts the download and `dpkg-query` answers `tree_2.0.1_install ok installed`, `/var/jb/usr/bin/tree` present (`logs/031316-run.log`; see "Sileo installs a package through its own UI").
| Third-party apps and a store (TrollStore, iSH, IPA path) | PASS for the store UI, for an App Store app, for an IPA install through TrollStore's helper, for the store's own GET -> Confirm install and update (1.0 -> 1.1), for a real third-party app (open-source Zhihu client, live feed), and for a curated daily source whose own row installs and launches that client | Two walls fixed on 2026-11-17: a scene line's `>> FILE 2>&1` silently loses the append and its last words (init's `run_command` reads `>> FILE` only as the last two words), and `/var/jb` is a graft whose files are symlinks into the read-only `/jb`, so `chmod`/`cp` through it fail EROFS and the binary stays 644 (xpcproxy `access(X_OK)` -> EACCES). `userland/bin/hk-appinstall.sh` now replaces the links with real copies and chmods them; TrollStore 2.1.1 launches (pid 89) and draws its Apps/Settings window (`logs/172052-run.log`, `logs/app5-2.png`..`-4.png`), and iSH, installed as a copy with its own bundle id (`hk-appjb.sh`, `app.ish.iSH.jb`), spawns (pid 76), takes the keyboard and shows in the switcher (`logs/173118-run.log`, `logs/app6-1.png`, `app6-3.png`). The helper path installs a signed IPA into an MCM container and launches it (`trollstorehelper returning 0`, `Successfully spawned LocProbe[83]`, `logs/225723-run.log`); the store's own picker followed on 2026-11-18: TrollApps' GET -> drag -> Confirm downloaded and installed LocProbe through its root-persona spawnRoot of the helper (`logs/101423-run.log`, `logs/sb-08.png`/`sb-09.png`, container `3CCE02B0-.../LocProbe.app`, `Successfully spawned LocProbe[280]`). The Baidu/Amap IPAs the user wanted have no lawful public source (see below). |
| HakiOS Maps / Weather (self-built screens) | REMOVED on the owner's instruction (2026-11-18) | The port does not ship replacement apps written here: existing apps are downloaded and installed as IPAs through the store chain instead. The history below stays as a record of the probe: the native tile view drew and requested correct OSM URLs, but every fetch failed while the link was still joining and a failed tile was cached for good (`logs/171103-run.log`). The sources, icons, Leaflet assets and the `rc.jobs.hweather`/`mapapp`/`mapqemu` scenes were removed from the tree; `build.sh` no longer compiles or stages them. |
| Existing third-party weather app (贵州天气) | PASS: installed from the curated store row and launched with live data | The developer's own GitHub release (`wangvizi/WeatherGZ` v3.2, 731 KB, SHA-256 `e3777b82…1c25`, cryptid 0, arm64, minOS 16.0) installs through `rc.jobs.storeweather` (安装 -> queue sheet -> Confirm) and opens as `cn.guizhou.weathergz` (pid 273, container `9C771D6E-…`, `logs/174409-run.log`, image `6528bd97`).  The console shows no helper line of its own for this install -- the `returning 0` at 17:45:39 is TrollApps' own install; the container and the spawned pid are this install's evidence.  The screen at 17:49 showed the app's live view (安顺·西秀区, 阴, 8°, 78 %, 东北风 2 级, 大风蓝色预警 cards, with the system location consent over it) and no crash or amfid abort is in the run; that frame's file, `logs/sb-29.png`, was overwritten by later runs because `shotsb.sh` writes one `logs/sb-NN.png` per run, so the surviving evidence for the launch is the run log.  A re-run will recapture it under a run-named file. |
| Existing third-party map app (Alpimaps) | INSTALL PASS; launch FAILS on EGL (`eglInitialize()`), cause named | `akylas.alpi.maps` 3.0.4 (build 242), MIT, 62,303,169 bytes, SHA-256 `566c5f52…4f94`, cryptid 0, arm64, minOS 12; the developer's GitHub iOS alpha release, the curated source's second row. `logs/180739-run.log` (image `7d4f20be`): the store downloads it, the helper installs it (container `39587D6C-…`), `uiopen` spawns it -- then an immediate `exited due to SIGABRT`. The binary itself is sound: as uid 501 outside SpringBoard it initialises NativeScript (V8 8.9.6), Sentry and its event loop (`logs/185005-run.log`, via src/runas.c). The SpringBoard launch's own words were captured by **execwrap** -- a signed stand-in installed as the bundle's executable with the real binary moved to `alpimaps.real`, everything the app says appended to `/private/var/tmp/execwrap.err` (src/execwrap.c, `rc.jobs.almapswrap`, image `a8b93481`, `logs/190404-run.log`): **`Uncaught Error: Failed To call eglInitialize()`**, thrown from NativeScript's `_setupUI` -> `_addViewCore` inside `UIApplicationMain`, then `libc++abi: terminating due to uncaught exception of type NSException`. The app statically carries ANGLE (2128 `ANGLE` strings in its binary) and builds its map view through EGL; this port serves GL to the window server, not EGL/ANGLE to third-party apps, so the view's init fails. This is the port's graphics-stack boundary (the same family as Apple Maps' VectorKit and PPSSPP), not a packaging or FairPlay problem. The root of it is now measured: in a plain process, as root or as uid 501, **`MTLCreateSystemDefaultDevice()` returns nil** -- `IOAcceleratorES` is only `sgx` with no `MetalPluginName`, and the port's GPU is the separate `HakiosGPU` service -- so ANGLE has no Metal device to initialize (`logs/202425-run.log`, `rc.jobs.metaldev`, 2026-11-18). |
| Existing third-party torrent client (iTorrent) | DOWNLOADED and verified; phone install run pending | `com.xitrix.iTorrent2` 2.2.0, MIT, 24,288,935 bytes, SHA-256 `50457a89…93d7`, cryptid 0, arm64, minOS 16.0: the developer's GitHub release, staged as the only row of a test source (`repo-itorrent.json`, `org.resonance.store.itorrent.plist`, scene `rc.jobs.storeitorrent`, image `c37dcb78`). It bundles Firebase Analytics; installed only for compatibility testing. The run waits for the phone. |
| Notifications, audio routing and background apps | PARTIAL | Safari reaches and renders HTTPS content, Weather/Calendar widgets render, Apple services resolve and establish TLS. Notification delivery and sustained background behavior still need an interactive soak |
| Lock/unlock and passcode | PARTIAL | display sleep/wake through the lock button is verified; the sleep also takes the Wi-Fi link down (see the Wi-Fi row), and passcode entry, secure storage and reboot persistence are not validated |
| Location Services | PASS (Settings UI + Wi-Fi positioning + SDX55M QMI LOC GNSS engine); outdoor 3D fix pending | Settings → `隐私与安全性` displays native blue Location arrow icon, localized `定位服务` title, and live `打开`/`关闭` status (`inject_prefs.c`), entering stock `PUILocationServicesListController` with working master switch & per-app/system-services list (`logs/105145-run.log`, `loczh2-loc-off.png`, `loczh2-loc-on.png`). Wi-Fi CoreLocation delivers live fixes (`+/- 52..69 m`, `logs/105145-run.log`, `loczh2-locprobe.png`). SDX55M hardware GNSS (`umi_gnss.inc` + `umi_gnss_qmi.h` + `umi_gnssd` LaunchDaemon) brought up by adding Qualcomm `QMI_LOC_REG_EVENTS` AFW client identification TLVs (`0x10="MHAL"`, `0x11=1/AFW`, clearing `QMI_LOC_START` error 46) and `QMI_LOC_SET_OPMODE(STANDALONE=4)`: `0x21`/`0x4a`/`0x22` all return `0`, `FIX_SESSION_STATE=1 (STARTED)`, `ENGINE_STATE=1 (ON)`, continuous 1 Hz `UGL_SV` streaming with `malformed=0` and zero lease cycling (`logs/113308-run.log`, `logs/114028-run.log`). Indoor desk tests report `0` satellites in view (matching Android on the same desk); outdoor clear-sky 3D fix (`CLSimulationManager` injection) awaits outdoor test |
| User data persistence | IMPLEMENTED, NOT EXERCISED | The tree carries the machinery (var_store's two-slot archive of /private/var through /dev/rufs_darwin, a writable HFS+ data volume from darwin block 2042368, and tools/darwin-write.py as the only writer, with GPT, by-name and read-back checks); the daily arguments enable none of it, on thyme `by-name/darwin` is the 128 MiB `rescue` partition, not a darwin partition, so no round trip is possible yet, and any write to darwin needs the owner's explicit authorization |

### Rootless package manager (2026-11-04)

A RAM candidate was built from the current main with the locally fetched
Procursus rootless bootstrap. The system image hash was
`b7e22d95764ec68a8609160227f1abdc49ab07df79ff831b387a46690728ff99`;
the focused scenario hash was
`8323a216e68025402ae78a4d53c4440af8bf648f2292789a72ce44dc794b8eff`.
The default Wi-Fi/Bluetooth baseline was not replaced.

On the phone:

- init reported `/var/jb from /jb` with 439 directories, 6416 files/links
  and resolvable users;
- `dpkg --version` reported Procursus `1.22.6 (iphoneos-arm64)`;
- `dpkg -i` selected, unpacked and configured `tree 2.0.1`;
- `dpkg-query -W tree` returned `tree 2.0.1` and
  `/var/jb/usr/bin/tree -L 1 /var/jb` listed the prefix;
- `dpkg -r tree` removed it and the next query reported no package;
- Sileo launched, rendered its Featured page and remained up through the
  capture window.

The host prefix now carries `.list`, `md5sums`, `conffiles`,
maintainer scripts and triggers for packages merged by `tools/mk-jb.sh` and
`tools/jb-preinstall.py`. All 70 packages in the generated dpkg database
have a file list, and the device run emitted no missing-file-list warnings.
The builder now runs `tools/jb-check-prefix.py` and fails if a package has
no file list.

The offline package was fetched and SHA-256 checked by
`devices/thyme/tools/prepare-jbverify.py`. The offline scenario is
`userland/rc.jobs.jbverify`; `userland/rc.jobs.jbupdate` is the online
diagnostic and contains no credentials. Neither the package nor the generated
prefix is tracked.

Still open:

- online repository installation through Sileo: **PASS** on 2026-11-08 — the
  current root (`df20bc6e`) with the 2026-11-04 JB system image joined
  `192.168.0.109` and fetched `InRelease [8,310 B]` and `Packages [334 kB]`
  from https://apt.procurs.us; `apt-get install -y tree` fetched
  `tree 2.0.1 [43.0 kB]`, unpacked and set it up, `dpkg-query -W tree`
  answered `tree 2.0.1`, `tree -L 1 /var/jb` ran, `dpkg -r tree` removed it
  and the next query found nothing, and Sileo launched (`logs/012326-run.log`).
  The image's dpkg warned that ellekit, the openssh packages, preferenceloader
  and sileo have no file lists — the 2026-11-04 prefix predates
  `jb-preinstall.py` — and the install is unaffected. The earlier attempt with
  the 2026-11-04 *root* never completed the Wi-Fi handshake
  (`logs/011346-run.log`), so the current root is what made the difference;
- Sileo's own install flow: the first four attempts were made with a scene
  that never joined the Wi-Fi (`net_test waitaddr` alone: "NO address after
  60 s", apt "Could not resolve"), and Sileo's URL tasks answered
  `NSURLErrorNotConnectedToInternet` — correctly (`logs/013503-run.log` …
  `logs/025856-run.log`). With the join added the tasks return **status 200**
  (`logs/031850-run.log`, `logs/032907-run.log`: `getaddrinfo apt.procurs.us`
  0, gateway 192.168.0.1:80 connected, 21 tasks `status 200, error none`), the
  tree page loads from the Procursus repository and GET changes the button to
  QUEUE (`logs/sileoA-380.png`). Later runs timed the first-launch
  notification prompt (it appears only once the page has loaded,
  `logs/sileoD-260.png`), dismissed it and tapped GET again: the button reads
  QUEUE (`logs/sileoE-340.png`, `logs/041449-run.log`) but a tap at the same
  point does not open the queue sheet in this scripted flow, so the Confirm
  step is still the open piece and `dpkg-query` stays empty
  (`logs/042541-run.log`). The page's Repository row still reads "Untitled
  Repo" after the index fetched (`logs/sileoF-450.png`), so Sileo's
  repository object never got its name — the likely reason its queue sheet
  does not open, and the next thing to read in its logs; the apt path
  through the same repositories is PASS;
- Sileo on-line repository refresh and a longer interactive session;
- dependency repair after installing a package that needs additional packages.

**The Confirm step is closed (2026-11-13, `logs/094507-run.log`).** The queue
sheet is reached and used. The package page -- Sileo 2.5's
`NativePackageViewController`, whose modal Done button is commented out --
is dismissed by the injected library on the Darwin notification
`com.hakios.close-sheet` (`hakios: Sileo: close-sheet: the page on top was
dismissed`), the queue's popup bar opens the downloads sheet, and the Confirm
heights press it. Sileo then did what it does on an iPhone: moved the
downloaded `tree_2.0.1_iphoneos-arm64.deb` into `/var/jb/var/cache/apt/archives/`,
ran `apt-get install --reinstall --allow-unauthenticated --allow-downgrades
--no-download --allow-remove-essential --allow-change-held-packages
-oDir::State::lists=sileolists/ -y -f -oAPT::Status-Fd=5 ... tree=2.0.1`,
and apt's `term.log` records `Log started 09:51:46, Log ended 09:51:47`.
`dpkg-query` answers `tree_2.0.1_install ok installed`, the binary is
`-rwxr-xr-x root wheel 109184 /var/jb/usr/bin/tree`, and `tree -L 1 /var/jb`
prints the prefix. The run has no window-server fault. What remains of this
row is the long interactive session and a dependency-bearing package.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/060438-run.log` | `bdca7a132549bec6dbd899adb081c4701a8d300296bba0d95503be4e6184619c` |
| `logs/jbmeta-sileo.png` | `d9cbdd35279824497bb778fd4c885b4429fba7ffdc38e14207d5943eb0bf76f9` |

## Display, storage, and installation

| Check | Status | Evidence / next action |
|---|---|---|
| Native 1080 x 2340 display | PASS | Loader validates THYME GOP size and SpringBoard renders at native 1080 x 2340; J2 panel (`panel-id 0x4a32`) switches cleanly to 90 Hz (`seq=0 (ok)`, `F0`/`B4`/`2f 0x4`/`26 0x2` in LP mode) and retains 90 Hz across Display Sleep/Wake (`logs/100533-run.log`, `logs/101254-run.log`) |
| Brightness control | PASS (manual slider + blank/wake backlight off/restore); auto-brightness PARTIAL | Control Center slider sweep (`485 -> 265 -> 971 -> 857` permille, 45/45 `DCS 0x51` `(ok)`) and lock/idle backlight off (`DCS 0x51 level 0 (ok)`) / wake restore (`level 3512 (ok)`) verified in `logs/101918-run.log`; SLPI ALS auto-brightness remains `PARTIAL` |
| Display sleep and wake | PASS (re-verified with true OLED backlight off/restore & 90 Hz retention) | Power button lock and idle blank now forward CoreBrightness `DisplayBrightnessFactorWithFade` (`0` / `1`) via `kern.hakios.backlight_factor` so `Set Blanked: YES` sends `DCS 0x51 level 0 (ok)` (OLED emission off) and `Set Blanked: NO` restores the exact user slider level (`level 2056` / `3512`) while preserving 90 Hz (`CADisplayMode.refreshRate = 90`, `UIScreen.maximumFramesPerSecond = 90`, `logs/100533-run.log`, `logs/101254-run.log`, `logs/101918-run.log`). Cover-sheet bottom-edge swipe unlock remains working |
| APFS root/system images | PASS in RAM | the small `/redwood` image and full 2.5 GB system image mount in RAM |
| UFS read-only survey | PASS | The rebuilt THYME UEFI exposed 6 UFS LUNs with valid GPT CRCs and enumerated `super`, `userdata`, both boot slots and firmware partitions. The final packaged `prebuilt/xnu-usb.img` repeated this successfully (`logs/043951-run.log`); no write was attempted |
| Storage layout survey | PASS, read-only | Six LUNs and both liblp metadata slots validated. `super` uses 6.25/8.50 GB in slot A with 2.24 GB uncovered; `userdata` is about 111.4 GB and ends near LUN0's end. No free GPT partition exists (`docs/storage-layout.md`) |
| Persistent system/user storage | TODO | no accepted design; preserve Android and do not write partitions yet |
| Safe return to Android | PASS | RAM runs return through the loader; Android remains installed and bootable |
| Standalone installer/recovery | TODO | requires carve, guarded-write, RAM-boot-from-flash, slot-switch and recovery gates from `docs/install-safe.md` |

## Camera, cellular, sensors, and security hardware

| Check | Status | Evidence / next action |
|---|---|---|
| Rear camera capture | PASS (hardware stream) | `cam=7` streams the S5KHMX itself: 1920×1080 RAW10 through CSIPHY 3, CSID480 RDI 0 and IFE-lite 0 into memory — clean CSI (1591–2329 packets, no ECC/CRC errors), exactly 2,592,000 bytes written, no SMMU fault, one frame retained (`logs/100723-run.log`, `logs/104032-run.log`). `cam_shutter`, `cam_gain` and `cam_frame_lines` set the SMIA exposure (0x0202), gain (0x0204) and frame length (0x0340) and read back; a 16384-line frame with a 16000-line exposure and 128× gain captured a real light gradient (`logs/111407-run.log`, `logs/camera-umi-long-0.png`). The iOS camera graph receives these frames through `cam=5 cam_real=1` |
| Rear camera stream path | PASS | stage 7 of `hakios.thyme_umi_camera.c`: camcc PLL0 (Lucid, 1200 MHz) feeds ife_lite 400 / ife_lite_csid 400 / cphy_rx 400 / csi3phytimer 300 MHz; CSIPHY 3 is C-PHY, 3 trios, lane enable `0x2a`, settle 19, rate row 0 (1.707 Gbit/s a trio in the 1080p mode, 4.11 Gbit/s in the full-resolution one); CSID receives from PHY 3, vc 0, RAW10 `0x2b`; the SMMU mapper was extended past one 2 MB L2 entry for the 2.47 MB frame. The DT's `cam-sensor@1` names `csiphy-sd-index = 3` — the first attempt listened on CSIPHY 1 and saw 0 packets. `cam_mode=N` selects the module file's `resN` (default 3) |
| Rear camera full-resolution still (sensor path) | PASS (hardware stream) | `cam=7 cam_mode=0` runs the S5KHMX's whole res0 mode: 12032×9024 RAW10 (108.6 MP, 15040-byte lines, 129.4 MiB a frame), three C-PHY trios at 4.11 Gbit/s each through CSIPHY 3, CSID480 RDI 0 and IFE-lite 0 into the two-bank buffer (`kmem_alloc_contig` took 271,450,112 bytes at boot). The run logged 9723 CSI packets with no ECC/CRC errors, exactly 135,720,960 bytes written, no SMMU fault, the buffer guards intact and one whole frame (`logs/081219-run.log`); the exposed run (8000 lines, gain 0x200 — res0 itself sets neither 0x0202 nor 0x0204) read back `0x0202=0x1f40`, and `tools/camfull.py` made `logs/camera-full.png` from the fetched 271,450,112-byte recording (the room's desk, box and cable). `hakios.thyme_camera_dma.c` now refuses frames over 192 MiB (the old 32 MiB cap was below the full-resolution mode); the capture stack's 108 MP still is the next step |
| Front camera capture | PASS | `cam=7 cam_sensor=front` receives 734 packets of the whole 1280×720 mode-2 frame through CSIPHY 4 (`logs/205858-run.log`); the iOS path's mode 1 (2592×1940) reaches Camera.app, and a 150 s lit-session run applied the exposure before stream-on (`logs/083341-run.log`) |
| Rear camera sensor/power probe | PASS | `cam=2` reached the S5KHMX sensor: ID `0x1ad0`, revision `0xa000`, module selector `0x1301`; all three supplies released with error 0 (`logs/101242-run.log`) |
| Camera sensor mode programming | PASS | `cam=3` applied 259 init and 179 mode writes; dimensions `3008x2256`, line/frame lengths `0x3ea0/0x0945`, stream register `0x0100=0`, then powered down cleanly (`logs/114440-run.log`) |
| Camera ISP clock/reset probe | PASS | `cam=4` read IFE `0x10000001`, bus `0x20000000` and CSID `0x20000001`; completed CSID480 and IFE-lite48x resets with DMA disabled and all supplies released (`logs/122824-run.log`, SHA-256 `f5aa54ee6186f2420cb26fcaa7eea930add008daf994abf2a86de419d1784397`) |
| Camera CSID/IFE test pattern | PASS | `cam=6` wrote a whole 320x240 RAW/Bayer frame through the THYME DMA path; `xl camera` exported `logs/cam-tpg-0.png` (`logs/104221-run.log`) |
| Camera userspace / AVFoundation | PASS with the real sensors | `cam=5 cam_real=1` served the S5KHMX's own frames to `/dev/hakios-camera`: 3174 frames of 1280×720 (a centre crop of mode 5) at 15 a second, 3173 delivered to mediaserverd and Camera.app (`logs/131304-run.log`); an earlier session at 5 a second delivered 1166 of 1167 (`logs/122512-run.log`). The front camera is the device's second stream: the app's switch sends `HAKIOS_CAMERA_SENSOR`, the kernel selects the front descriptor (its `camera-s5k3t2.tab` modes) and the UMI driver's next session, and the front's 2592×1940 frames reach the same capture graph (`logs/213334-run.log`). The program's exposure/gain requests reach the sensor through its group hold. The preview was frozen on a session's first frame: the IFE-lite takes one buffer address per frame, so with a single address it wrote once and every read returned the same frame — often the black first one after stream-on. The DMA buffer now holds two frames, each read hands the WM the other bank and copies the one its frame has finished in (`logs/001158-run.log`, `logs/preview-135.png`). The AE's group hold is bit 0 of 0x0104 on the S5KHMX (the module's 0x0100 registers the values but the sensor keeps the old ones; 60 -> 1000 lines now moves the frame 29 -> 41 in the dark, `logs/000647-run.log`); the S5K3T2's front takes the exposure only before 0x0100: mode 1 with a pre-stream 2000-line/64x exposure is a real picture (`logs/front-mode1-0.png`), while the app's session stayed black at the maximum and the AE's changes are now applied between a stop and a start (30 ms standby, the register read back). The AE's commands are logged and reach the driver (`rear exposure 1180 lines, gain 0x200 -> 0x800`, `logs/033113-run.log`); a command that arrives between sessions is kept and starts the next one. The front's stop/start exposure works: a short session with the app on the front (`logs/050628-run.log`) shows `front exposure 2024 lines (reads 0x7e8), gain 0x800` at the session start and the AE's ramp `0x200 -> 0x372 -> 0x800`, the register reading back 2024 lines every time; in the dim room the app now shows a real, detailed (noisy) scene at the 64x gain ceiling (`logs/front-lit-210.png`), not a black frame. Later the front took SMIA's own group hold after all (0x0104 bit 0, mid-stream, no stop — see the front camera's row), so the stop/start path is now only the fallback behind `cam_front_hold=0`. Long sessions wrap the kernel log ring, so their early session lines are not evidence. `cam=5 cam_fake=1` still provides the synthetic chart for QEMU (`logs/110735-run.log`) |
| Camera exposure and switching | PASS | The frame header used to name the exposure just asked for while the bank held the frame before it: the capture stack's AE waited for the header, measured the wrong light, and rang between dark and bright (gains `0x2a`–`0x200`). The header now names the exposure of the pixels the read copies, and the AE's step is 0.4 in log space (0.7 against the one-frame feedback delay rang for fifteen frames); a session settles from 8x to its room's gain in five steps (`logs/161455-run.log`: `ae: frame 2 ... 8.00x -> 0xc0` … `frame 10 ... -> 0x81`). The reads follow the sensor's frame time now: mode 3's HTS x VTS / PCLK is 92 ms at the mode's 2373-line frame (its longest exposure, 2333 lines, nearly fills it), and a 40 ms wait copied the DMA bank while the WM was still writing it — the preview tore across, a line of the next frame's picture, whenever the phone moved, while reads faster than the sensor duplicated frames (the earlier fifteen and twenty-five counts were reads, not frames). The capture stack's AE now also asks for the frame length: the sensor takes a shorter frame (0x0340, 1000 lines = 39 ms at the shortest) when the light fits in it, and the grab waits whatever the sensor is running (10.0 frames a second at the mode's frame, `logs/172555-run.log`; 17.4 in the room, 1548 frames in 88.9 s, `logs/175138-run.log`; 25.6 at the shortest frame). The line time is the sensor's HTS / PCLK, so the header's exposure and lux are right too (the ctab now says 10.85 frames a second, not 30; the front 11.06). The AE's measured mean is smoothed (0.12 a frame), a change beyond 15 % between asks holds the exposure until the scene settles (a violent shake is not a light to follow), asks are at least 10 frames apart (12 on the front, whose exposure now applies mid-stream through SMIA's hold, so an ask keeps the frames it used to lose), and one step is capped at a fifth either way; a 5.8-minute front preview in `logs/202824-run.log` ran 6939 frames at 20.0 a second with no panic. A 30-minute daily session with that build stayed up (three Wi-Fi joins, no mid-run fault; `backboardd`'s SIGSEGV is the known teardown artefact, at the return's second — `logs/205333-run.log`). A vibrator-driven shake scene ran the rear and front previews at 17.5 and 12.9 frames a second; neither CoreHaptics nor the ringer's system sound reached the kernel's vibration counter (0 vibrations played — the system sound went through the audio path as a tap), so whether the gate holds under a hand's shake is still the phone owner's test. What the front's log does show is the spacing: five asks over its 62-second session, 41 frames (about 2.4 s) apart, converging 8.5x → 5.6x instead of asking every frame (`logs/213228-run.log`, `logs/213857-run.log`). A switch asks for the stream's sensor before its first frame, so the kernel starts no session on the sensor it was last on; the timing is in the front camera's row. `cam_frame_min` and `cam_front_frame_min` set the shortest frame's lines (defaults 1000 and 1600): a higher floor is more light a frame — less gain's noise — and fewer frames a second, a lower one the reverse; either is clamped to the mode's own frame. The grab's wait now takes the longer of the frame length the read was asked under and the one the previous read ran at: a bank whose frame was exposed at the old, longer length is not copied on the new, shorter one's clock. When the AE shortened the frame while the phone was moved through changing light, that wait copied the bank mid-write — the tear the phone showed as a flicker (`logs/232943-run.log` is the run that found it; the fix is in `umi_camera_stream_grab()`). A 30-minute preview soak on the camera-focus build (`logs/003837-run.log`, kernel `9488e228`, root `df8e41f6`) ran the rear session for **20,927 frames** (11.6 a second on the dark room's 1960-line frames), every frame given to the capture stack, with the UI answering the screenshots at 5, 13, 22 and 29 minutes (`logs/camsoak30-300.png`, `-800`, `-1340`, `-1740`); no camera error, no memory pressure or jetsam, thermal nominal/moderate (59–71 °C) through the run and one heavy reading (76.3 °C) only in the return's teardown, the battery on its charger at 91–92 % and 35–36 °C. The only fault is the known `backboardd` SIGSEGV at the return. The UI path works too: with the scene waiting 300 s for SpringBoard (the demo job starts at launchd, minutes before its window server), the Camera icon tap opened Camera.app, which ran the rear S5KHMX session (481 done / 479 given, mode 3 3008x2256) and the flip tap switched to the front S5K3T2 (1,331 done / 1,328 given, mode 1 2592x1940) -- `logs/202610-run.log`, SHA-256 `c2973f8b71fb784873a1ecf929feb207b6d728ff93fc84620b6dbae48493a01a`, scene `864738bc`. The timed screenshots missed the preview window (the app opened at kernel t≈435, the captures ended at ≈400); the kernel's session lines above are the evidence. The preview itself is now in a screenshot too: `logs/camui2-520.png` shows Camera.app's PHOTO interface with the live rear image, the mode bar and the shutter/flip controls, from the rebuilt scene (`3a9d9d0d`, the touch-unlock userland) whose bottom swipe also exercised the new unlock path -- the log's "a swipe up from the bottom edge: the unlock asked for", then "not locked", then the direct dismissal, left the camera flow untouched (rear 344 done / 342 given, front 4,309 / 4,305; `logs/013325-run.log`, SHA-256 `8bf73a2a2d0052666657cb06cdac7c5933891affb6729a0e8409680c7ea32098`). **2026-11-26 switching & AE audit fix**: removed redundant `stream_begin()` from `device_request_streams()` (stopping Back->Front->Back triple power-ups), added atomic `HAKIOS_CAMERA_SENSOR` slot invalidation & read gating during switches, raised `IDLE_MS` `1000 -> 4000` ms (stopping mid-shutter sensor reboots), and changed initial exposure (`frame_length/2, 3x`) + fast startup AE (`ae_frames < 15`, `ae_hold=4`), eliminating the 4 s Front Camera white-out flash (`logs/092759-run.log`, `camfix3-t134.png`, `camfix3-t170.png`). |
| Camera in the daily boot | PASS | `devices/thyme/ram.args` carries `cam=5 cam_real=1 cam_mode=1 cam_keep_mb=48 torch=3` (rear defaults to 27 MP `res1` `6016x4512` Tetracell 2x2), creating `/arm-io/isp`, advertising Back and Front cameras via MobileGestalt, and launching Camera.app smoothly (`logs/092759-run.log`) |
| Camera colour tuning | PASS | `camtuning.py` now reads the phone's Parameter Parser V2.0.0 tuning file (its 0x48-byte symbols, 8-byte fields, cc12 matrices and padded lens-shading grids); the scene carries `/redwood/fw/camera-s5khm2sd.isp` made from `com.qti.tuned.thyme_semco_s5khmx_wide.bin` (25,428 bytes). The media library loads it: 3 colour matrices' lights and 3 lens shadings (`logs/131304-run.log`). That file's AWB reference lights are zeroed, so the auto white balance runs grey world without the reference-light projection (gains around R 1.2 B 1.7 under warm indoor light; `logs/211126-run.log`). The front S5K3T2's own tuning (`/vendor/lib/camera/com.qti.tuned.thyme_sunny_s5k3t2_front.bin`, 5.3 MB, pulled with `su`) is in the image as `/redwood/fw/camera-s5k3t2.isp`: 1 colour matrix's light and 3 lens shadings (corners about 3.3x), loaded for the front stream (`logs/215111-run.log`). The white balance now also takes a
white patch: a 16 x 16 histogram of each cell's colour (R/G across, B/G up) weighted by
its green, the brightest bin near grey with its 3 x 3 neighbourhood and a mild warm-side
prior (this sensor's white is red over blue under every light), blended 70/30 with the
grey/neutral estimate. In a room whose cyan wall filled the rear's view the blue drift
falls from B/G 1.174 to 1.095 (`logs/lit-rear.png` against `logs/cl-rear.png`,
`logs/132746-run.log`), while the white-walled scenes are unchanged (front still R/G
1.008, B/G 0.994, `logs/133327-run.log`). The whites in that view still read warm — they
clip at the 28x gain the dim room needs, which the white balance cannot undo; the
exposure's highlight headroom is the next piece there. That pink was in the end the still
path's, not the white balance's (the still capture row has it): the frame sums were not
cleared between shots, so the second photograph went out at twice the light and clipped.
Android's own HAL, on the same scene,
reports its white balance: `CamX ... Applying Stats Update. AWB Gain(R:1.867, G:1.000,
B:1.971) CCT(4761). AEC Gain(11.0) ExpTime(20 ms)` (`adb logcat`, `logs/android-rear-1420.jpg`).
Ours in that room were R 1.86, B 1.88, CCT about 5300 — the same white balance, so the
pink is not the AWB's; on the same fridge patch Android renders R/G 1.043, B/G 0.930 and
ours 1.123, 1.048, which puts the difference in the rendering after the white balance
(the colour matrix, the tone curve, the scaling), the next place to compare |
| Camera still capture | PASS | Camera.app's shutter on the real stream: the photograph is made of the wide's 3008×2256 mode whole (the session hands up the whole sensor frame, not a centre crop), the mean of eight raw frames summed in the sensor's 10-bit values (eight since the preview's frame is shorter and carries less light; ~0.3–0.5 s of light in the room), rendered to the graph's 4032×3024 StillImage buffer. The four frames are exposed alike now that each frame's header names the exposure its pixels were made with, and the lens is at the scene's focus (the fridge at ~2 m settles near 1120). The same buffer goes whole to the host through `HAKIOS_CAMERA_KEEP` (`camera: a picture from iOS for the host: 4032x3024, format 2`) and `xl camera` writes it out (`logs/1655iosrear.png`). The label crop beside the Android reference is legible — the QR code, the 中国能效标识 title and the print below it (`logs/162831-run.log`, `logs/label-compare-af3.jpg`); the same crop was a noisy blur before the autofocus and AE fixes (`logs/cmp-ios-label-4f.png`, `logs/cmp-android-label.png`). The frame sums behind a photograph were **not cleared between shots**: the second shot added onto the first one's total and went out at twice the light, its whites clipping channel by channel into pink — the preview, which never averages, measured R/G 0.934 and a still rendered from a live frame 0.935, while the averaged second shot measured 1.136 (`logs/preview-lit2.png`, `logs/live-rear.png`, `logs/pv-rear.png`). With the clear the second shot measures R/G 0.938, B/G 0.955, matching the preview and Android's own 0.939/0.966 (`logs/fix-rear.png`) **2026-11-26 sharpness & anti-ghosting fix**: upgraded rear default to `cam_mode=1` (`res1` `6016x4512` 27 MP native Tetracell 2x2, ~26 fps live viewfinder), replaced unconditional 8-frame still averaging (which caused severe multi-exposure ghosting) with instant 1-frame capture in normal light (`gain < 12x`) + motion-checked 3-frame low-light burst (`gain >= 12x`), upgraded still demosaic to integer 5x5 Malvar-He-Cutler gradient-corrected linear + 2D 3x3 unsharp mask with noise coring; verified live in `logs/092759-run.log` (`camfix3-still-label100.png`: 3 mm text & QR code razor-sharp at 2 m with zero ghosting). |
| Front camera (S5K3T2) | PASS (hardware stream and Camera.app) | The stream's silence was the CSID's PHY select: `RX_CFG0`'s phy number is a 3-bit field at bits 22:20, the front's CSIPHY is 4, and the code masked it to two bits while sweeping only 0–3. With `phy & 7` and phy 4, `cam=7 cam_sensor=front` receives **734 packets, the whole 1,152,000-byte 1280×720 mode-2 frame**, rx IRQ `0x10000ff`, no ECC/CRC, IFE done (`logs/205858-run.log`). The iOS path (`cam=5 cam_real=1 cam_sensor=front`) runs the module's mode 1 (2592×1940, stride 3248, 6.3 MB) through CSIPHY 4 (D-PHY, lanes `0xd5`, settle 19), CSID-lite 1 and IFE-lite 1, and Camera.app displays it (`logs/213334-run.log`, `logs/preview-165.png`); `HAKIOS_CAMERA_SENSOR` now selects the front descriptor and switches the UMI sensor between sessions, and the app's rear↔front switch was exercised both ways in that run. Power/ID/tables: `cam=2`/`cam=3` power the module (PM8009 LDOF7/LDOF4, the 2.85 V rail, the PM8150L gates) on the CCI controller at `0xac50000`, master 0 = **GPIO105/106**, read **ID `0x3142` at slave `0x20`** and apply the init (281) and res2 (66) tables (`logs/153959-run.log`, `logs/160909-run.log`). The front power-up took all three PM8150L gates but the release skipped PPID `0x4c2`, leaving the 2.85 V rail on for the next session (EBUSY 16); the release restores it now, and an already-on gate is taken over. Tables come from `umi_sunny_s5k3t2.bin` via the generalized `umi-camsensor-v2.py`. The front's lit-session check: a 150 s run with the app forced to the front applied 2024 lines at gain `0x200`, the AE ramped `0x200 -> 0x372 -> 0x800`, and the screenshots at 190–250 s show a real scene — the ceiling/wall corner and a wall panel — heavy chroma noise at the 64x gain ceiling but no black frame (`logs/083341-run.log`, `logs/front-lit-210.png`). The app's rear→front switch failed after the first session: `uprobe()` re-read `cam_sensor` from the boot argument and overwrote the switch (EINVAL before the front was powered), and the first session shrank the boot-time 2592×1940 DMA reserve to its own frame (ENOMEM for the front). Both are fixed: after the switch the front arms (`armed 2592x1940 stride 3248, 4 lane(s) ... phy 4`), reads `S5K3T2 ID 0x3142`, and every session start answers 0 (`logs/105556-run.log`, `logs/camcmp-220.png`). The handover itself: a stream's worker now asks the kernel for its own sensor before its first frame, so the kernel no longer starts a session on the sensor it was last on (the other camera's power-up and two frames) before the switch; the handover is one kernel power-up. Measured from the old session's stop to the new camera's first frame: 1.7–1.9 s to the front and 2.4 s to the rear, of which about 1.3 s is the capture graph's own reconfiguration before the kernel is asked and about 0.6 s the kernel's power-up (`logs/161455-run.log`). The session follows the mode's frame time now (90 ms for the front's mode 1): ten frames a second, one distinct frame per read — the fifteen before were duplicate reads of a slower frame (see the exposure row). The front's white point against the Android front camera's on the same walls measured 1.5 % short in red (R/G 0.987 in the brightest tenth against 1.002, `logs/1840iosfront.png`); the stream's white balance now carries a 1.5 % red calibration after the scene's estimate (`isp.wb_bias`, the camera table), and the same measurement reads 0.995 with the neutral mean at 1.002 (`logs/1900iosfront.png`). The module's colour matrix is the identity: both of its tuning file's colour-correction modules (`mod_cc12`, `mod_cc13`) carry the 3x3 identity in its symbol table (dumped entry by entry, `tools/camtuning.py`), so there is no real matrix to extract — the front's colours are the white balance's and the gamma's alone, less saturated than Android's; a chart-based calibration would be the only way to get a real one, so the stream applies a 1.18 saturation after the white balance (its pictures looked grey beside the Android camera's). Its frame floor is 1100 lines (48 ms, twenty frames a second) against the wide's 1000; the earlier 1600 (70 ms, fourteen) kept its high-gain noise down, but a preview that follows a moving hand read as a stream that had stopped, so the floor gives up about a stop of light a frame for the motion. The module has no lens motor and the frame header now says so (`HAKIOS_CAMERA_NO_FOCUS` from `umi_camera_ios_focus()`): before, the front's frames carried the rear motor's last position and the capture stack ran its contrast search on every front frame, sending the rear's AK7377 positions while the rear's supply was down — `lens position … not taken`, one per step (`logs/233807-run.log`) — and the search cost a pass over each frame. In a lit room on 2026-11-08 the floor's 1060-line exposure ran the front session's 390 frames (about fourteen a second, the hold reading back `0x424`) and its still measured R/G 1.020, B/G 0.990 in the centre against the Android front camera's 1.025/1.004 on the same room — there is no cool cast left to correct (`logs/110219-run.log`, `logs/camreturn-front.png`). |
| Camera autofocus | PASS | The lens motor is an **AKM AK7377**, not the DW9800 the earlier notes assumed: the module file names it `umi_ak7377_s5khmx`, and the AK73xx family takes the position in register `0x00` as a 16-bit value with the position shifted left four bits (`0x02` is control). The chip answers at CCI address `0x18` (the 8-bit form of the module file's 7-bit `0x0c`; `0x1c` is the OIS), and the DW9800's `0x03/0x04` writes never touched it. With the AK7377 write the lens moves: `cam_focus=0` leaves the fridge label a blur, `cam_focus=1023` renders its QR code and print (`logs/141344-run.log`, `logs/focus0-0.png`, `logs/focus1023-0.png`, `logs/cmp-ak1023.png`). The capture stack's requests are no longer masked to ten bits, and the app's search walks AF_NEAR 16 to AF_FAR 3072 in steps of 96 (fine 24). The search runs end to end: the frame header carried NO_FOCUS in the UMI path, so the app's search returned at every frame until `umi_camera_ios_focus()` reported the motor's position. It sweeps and settles at 1168 for the fridge scene, and the photograph's label crop rises from a 48 to a 63 gradient score with its QR code and print legible (`logs/144509-run.log`, `logs/ios-rearpic.png`, `logs/cmp-ios-label-af2.png`). The metric was not the problem: over the fridge scene the block-gradient measure runs 0.0043 out of focus to 0.0355 at the peak, and the `0.0` in the earlier log was its one-decimal format (now `%.4f`). At stream start the lens climbs about the module's position (343) in steps of 96; when neither way is sharper — the fridge's focus is near 1120 — it falls back to the whole travel and then the fine search, so the photograph is not left at the module's position (`logs/162831-run.log`, `logs/1655iosrear.png`, `logs/label-compare-af3.jpg`). The front camera's lens is fixed. A travel with no peak in it is no focus and no longer ends at a noise-picked position: the second session after a switch (rear→front→rear) swept all 32 steps with every value between 0.0068 and 0.0070, took 'the best' at 880 and left the rear soft until the next search (`logs/233807-run.log`); the sweep now tracks its least value and, when the best is within a fifth of it, goes back to the position the last search settled on (`af_home`, kept across the streams). A scene that turns much sharper than the one the lens settled on — the phone pointed at something with detail after a blank wall — now asks for the search again after three frames; the watch only reacted to a drop before, so a search parked on a blank surface never came back. Near-black frames are not measured at all: the measure divides the centre's gradient by its mean's square, and on an all-black frame (the phone face down, `logs/235943-run.log`) the noise read 0.36 and the search 'settled' on it. `camreturn` (`userland/rc.jobs.camreturn`) is the scene that exercises this rear→front→rear path; its lit verification ran on 2026-11-08 in a lit room (`logs/110219-run.log`): the first rear session settled at **1399 (sharpness 0.018)**, the front session ran 390 frames at the 1100-line floor, and the second rear session settled at **1471 (0.017)** — a real peak eleven moves away, where the old code swept all 32 steps on noise and parked on it. The dark half is verified on the phone: an all-black rear→front→rear run (`logs/002926-run.log`, the phone face down, every frame green 0) made no focus measurement at all — zero `focus: at` lines and no lens moves, where the run before the guard swept 66 positions and settled on 0.36 of pure noise — and the front's session logged no rear-motor writes (its `NO_FOCUS` frames keep the capture stack from searching a motor that is not there). The three streams (289 rear, 278 front, 474 rear frames) ran to their stops without a fault. **2026-11-26 AK7377 & AF audit fix**: prior boots hit `AK7377 writes REFUSED` on 154/158 runs because `uprobe()` wrote `VCM_REG_POS` immediately after `VCM_REG_CONTROL=0` without the module's wake-up delay, and `VCM_OPTIMAL=343` was a 10-bit code shifted `<<4` instead of `<<6`; added `ums(5)` wake delay + 3-attempt retry + `VCM_OPTIMAL=1152` (`0x4800`), fixed `isp_sharpness()` (`1x1` cell in normal light) and `AF_SWEEP` peak-before-early-stop guard (`AF_NEAR=256..AF_FAR=3600`), plus shutter AF-wait; verified `AK7377 at 0x18: writes taken, control 0 position 0x4800 (read back)` and clean parabolic AF lock at `1152` (`logs/091824-run.log`, `logs/092759-run.log`). |
| Torch/flash | PASS | Hardware pulse: PM8150L channel 97 ramped up and cleaned up (`logs/081934-run.log`). End-to-end userspace and stock Control Center: AVFlashlight/mediaserverd requested 1000, 250 and 850 through `/dev/hakios-torch`; every request reached the kernel and the safety timer returned it to dark (`logs/090116-run.log`, `logs/091200-run.log`). The daily args now carry `torch=3`, so the device is there on every boot |
| Cellular modem / SIM | PASS | SDX55M (`17cb:0306`) boots over PCIe2/MHI (`hakios.thyme_pcie2.c` + `hakios.thyme_baseband.cpp`), switches physical SIM slot 2 to logical slot 1 (`UIM 0x0046`/`0x0038`), registers on China Broadnet (`46015` / `中国广电`) with 4 LTE signal bars + `4G`, and populates native Settings → Cellular (`52` items, zero blank page, `logs/020956-run.log`, `logs/cc4-cell1.png`) |
| Mobile data / calls / SMS | PASS (4G data + two-way VoLTE voice); SMS deferred | 4G LTE dual-stack (`pdp_ip0`) verified with HTTPS `200 OK` (`253,027` B in `0.87 s`, `logs/020956-run.log`). VoLTE IMS (`pdp_ip1` + `ipsec0/1`) + two-way AMR-NB RTP voice (`volte_rtp` + `/dev/hakios-imsrtp` + `/dev/hakios-imsrtp-data`) verified live on China Broadnet `10099` for `21.8 s`: `977/977` RX packets decoded to CS35L41 speakers (`19.54 s` at `48001` fps, peak `35%`, `0` underflows) and `972` TX mic packets (`mic_peak=15971`, `0` errors, `logs/035640-run.log`). SMS testing deferred per user direction |
| ALS / auto-brightness | PARTIAL | the SLPI ALS path is part of the sensor bring-up; brightness remains manual until the HID sensor service works |
| Accelerometer / orientation | PARTIAL | the SLPI boots and the accelerometer SUID lookup is accepted; no samples reach backboardd yet. The sensor framework no longer dies at start-up: the AP's `sleepstate` SMP2P entry, which the phone's dt has and this kernel's dsps block lacked, is now written (`master-kernel` + `sleepstate`), the DSP adds its `sleepstate_see` at +146 ms instead of 2.2 s, the `EX:kernel:...:PC=0xb01c1f2c` exception is gone from item 424 and the SNS reads answer 0 (`logs/162243-run.log`, `logs/162659-run.log`; `docs/sensors-status.md`). The client manager's first message is still answered 5 at +1.2 s; the driver repeats it and the registry upload and stops at the first 0. Neither the 189-group set, a delayed probe after the PD's own restart (`sns_start_delay=150`), nor the AP's sleep-state value (0x2 with the SMP2P doorbell) changes it: the framework hangs and its user watchdog ends it 77 s after the start (`docs/sensors-status.md`). **The hang's cause is now found and the framework stays alive**: with the phone daemon's listener order (`fastrpc_phone=2 fastrpc_pd=2 fastrpc_smmu=2 fastrpc_call=9 fastrpc_early=1 fastrpc_grow_kb=4096 fastrpc_grow_after=1`) the DSP opens `adsp_default_listener` (retval 0, the bare order's open is refused `0x80000434`), the sensor process has zero protection-domain failures and zero watchdog lines through a 110 s run, acknowledge the stop in 20.5 ms, and its first file reads arrive and are served (`apps_std` open, `fopen_with_env`, two `fread`s and `fclose` for `/fw/sensors/soc0/hw_platform`, `logs/182550-run.log`). A later `frpc_dsp` exception (`PC=0xb20c2dac`, `slpi.b20`) turned out to be the listener's re-issued `next2` at the teardown, not the SNS path: it reproduces with `sns=0` at 150 s (`logs/185755-run.log`) and is gone when the listener stops at its first timeout (`logs/190531-run.log`). The framework then lives the whole 300 s run with no watchdog even after the listener stops, and on it the SUID lookups produce events (40 `has SUID`, result 0), whose SUID is the request's placeholder because no sensor instance is registered yet (`logs/193745-run.log`, `logs/191949-run.log`). The registry's own instance is still not started: its group write answers 5 at +45..53 s (`logs/194241-run.log`) |
| Proximity | TODO | the SLPI bring-up covers the path; no proximity data or pocket behavior yet |
| NFC | TODO | not ported |
| Fingerprint | TODO | not ported and no secure unlock design exists |

### Sensor core finding (2026-11-05)

The decompiled THYME device tree has no accelerometer, light or proximity
node on the AP's I2C buses. The sensors are behind the SLPI core
(`qcom,ssc@5c00000`, `qcom,msm-ssc-sensors`, firmware-name `slpi`), reached
over QRTR/GLINK. The stock Android partition exposes `slpi.b00` to `slpi.b20`
at `/vendor/firmware_mnt/image` (readable as root).

### SLPI bring-up (2026-11-05)

The loader is `devices/thyme/drivers/sensor/hakios.thyme_slpi.c`, a copy of the
audio DSP bring-up with the SLPI's numbers: processor 3 in shared memory,
SMP2P items 481 (its) and 430 (ours), last words 424, IPCC client 4, PAS 12,
the 0x8e800000 carveout (47 MB), GLINK items 478-480 and the IPCRTR channel.
The firmware is `slpi.mdt` and `slpi.b00..b20` pulled from the phone into
`fw-slpi/` (Git-ignored) and packed when `SLPI=1`.

`logs/131207-run.log` with `slpi_step=10` proves the boot: the 20-segment
8.18 MB image is accepted by TrustZone; the DSP says it is up 104 ms after
the start; GLINK IPCRTR opens; QRTR reports node 9 and 8 services, including
the SNS client (400). The AP's `fb_beam` guard is a separate fix.

The SNS client QMI path is up. The SSC carries the protobuf payload in an
array TLV (outer length = inner + 2); with that encoding the service returns
QMI result 0 and a client id, where the plain TLV was rejected with error 19
(`logs/140238-run.log`). The `accel` and `registry` SUID lookups are accepted,
but no indication arrives yet, and the sensor process gives up after about
164 s with the user-PD watchdog
`dog_virtual_user.c`/`stalled initialization`. The next layer is the sensor
registry: the phone's `/mnt/vendor/persist/sensors/registry/registry/` JSON
and `/vendor/etc/sensors/config/*.json` have to be delivered to the SLPI (the
Android HAL path), or served by an AP-side SNS_REG service; FastRPC/HexagonFS
may also be required. No sensor data reaches iOS and no HID sensor service is
exposed yet.

The AP-side legacy Sensor Registry service (0x10f, the mainline `sns-reg`
protocol) was advertised to the DSP; the sensor manager requested no group,
so this firmware does not use that bootstrap (`logs/141201-run.log`). The
modern path is FastRPC (`direct_channel::init_fastRPC_symbols`,
`libsns_fastRPC_util.so` in the HAL); the SNS QMI lookups alone never load
the registry. The fast diagnostic loop is `devices/thyme/slpi-run.sh`.

The FastRPC reverse tunnel is now up as far as the DSP. The argument packet
lives in shared memory (item 621): an ordinary kernel page is not reachable
by the DSP and wedges the phone at step 10. `INIT_ATTACH_SNS` (client 0),
`remotectl_open("adsp_default_listener")`, the listener's register and
`adsp_listener_init2` all answer 0; `adsp_listener_next2` (handle 3,
`0x040202`) is polled with one call outstanding because the DSP holds the
GLINK intent while it waits. `remotectl_open` is rejected in PD 0
(`0x80000434`), so the chain stays in PD 2 (`fastrpc_pd` selects it).
`logs/185355-run.log`, `logs/190234-run.log`, `logs/191700-run.log`.

The register goes on the handle `remotectl_open` returns (method 0; on the
fixed handle 3 the DSP answers `0x8000040e`). The handle `init2`/`next2`
must use is neither that nor the fixed 3: Qualcomm's own `adsp_listener`
stub opens it by name (`remote_handle_open("adsp_listener")`, i.e.
`remotectl_open` of that name), and its module load can take longer than the
other opens. An outstanding `next2` holds the DSP's FastRPC thread: an
`INIT_ATTACH_SNS` sent afterwards is not answered, even with another client
id and thread id, so the listener cannot simply wait while the sensors PD is
attached. The guest-OS attach (PD 0) is what makes `init2` in PD 0 answer 0;
without any attach it answers `0x80000434`.

The AP side of the tunnel serves `apps_std` methods 1, 2, 3, 4, 9, 19, 26,
27, 28 and 31 from `fw/sensors/`: the phone's `sns_reg_config`, its 49
`config/*.json`, and the six `/sys/devices/soc0` values (`sensor-files.sh`
pulls them; `SLPI=1 devices/thyme/build-root.sh` packs them). What is still
missing: the DSP's sensor process has not made a nested call. Without a
listener it watchdogged at +37 s (`dog_virtual_user`); with one it waits
silently, so the tunnel's delivery, not the process, is the remaining
question. The working notes are in
[`docs/slpi-fastrpc.md`](docs/slpi-fastrpc.md).

The phone's own daemon (`sscrpcd`, pulled read-only and disassembled) settles
the sequence. It runs twice: `sscrpcd sensorspd` (init service
`vendor.sensors`) and plain `sscrpcd` (the root PD). It dlopens
`libssc_default_listener.so` and calls `adsp_default_listener_start` with a
URI: `'":;./\createstaticpd:sensorspd` only selects the **attach mode**
(`FASTRPC_INIT_ATTACH_SENSORS`, i.e. `INIT_ATTACH_SNS`), and `attachguestos`
selects the root PD; `geteventfd` returns the listener thread's eventfd.
`INIT_CREATE_STATIC` exists only for the audio (3 MB heap) and OIS static PDs,
so the `adsp_listener` create attempt was the wrong mechanism. The order is:
open `adsp_default_listener` (which attaches the sensors PD, calls `init2` on
the fixed handle 3 and starts the `next2` thread on thread ids of its own),
then register, then take the eventfd; the control handle is never closed.
`fastrpc_phone=1` reproduces that order, `fastrpc_close=1` restores the
mainline close. The phone's kernel is a custom 4.19 with the downstream
`/dev/sdsprpc-smd` UAPI and the fastrpc tracepoints, but tracefs is not
writable from the shell domain.

The last probes of 2026-11-06 settle what the DSP does and does not do. With
the listener registered before the attach (`fastrpc_phone=2`: attach, init2,
open, register, then next2) everything answers 0 and next2 waits — and no
request ever comes (`logs/004550-run.log`). Running a listener in the root PD
and the sensors PD at once (`fastrpc_phone=4`) also gets nothing on either
(`logs/010653-run.log`). The sensor process itself starts at the attach and
meets its user-PD watchdog about 31 s later: item 424 then reads
`err_qdi.c:1038:EF:sensor_process:0x1:TMR_CLNT_1:0x88:dog_virtual_user.c:240:USER`
— a QDI timer-client failure, not a file error (`logs/005342-run.log`). After
that watchdog the DSP's
FastRPC channel stops serving: a re-attach gets no answer and next2 reports
"the channel did not take the call". So the reverse tunnel works up to the
DSP and the missing piece is on the DSP's side of the sensor process's own
start-up, not in the AP file service.

The remote heap was the next suspect and it is not it either: the vendor's
`remotectl_grow_heap` (method 2, one physical address and a size) is refused
before any attach with `0x80000474`, but accepted after one (retval 0) — a
124 KB heap from shared memory, page-aligned, is the most the partition holds
(`logs/071411-run.log`). Offering it in the root PD's session before the
sensors PD is attached and bringing the sensors PD up as a second client
answers 0 at every step and next2 still times out; the sensor process never
asks for a file (`logs/072345-run.log`).

Disassembling the firmware segment `slpi.b15` (loaded at `0x8A000000`; wrapped
as a one-segment ELF32 for `llvm-objdump --arch-name=hexagon`) shows what the
watchdog string means: `timer_client_user_init.c` opens the QDI device
`/dev/timers` under the name `TMR_CLNT_%u`, and the failure logged is
`TMR_CLNT_1:0x88` — that `qdi_open` failing. The sensor process dies before it
ever reaches a file: its QDI timer client cannot open in the PD the AP starts,
which is a PD/QDI capability question inside the DSP, not a FastRPC one.
`INIT_CREATE_STATIC` is not the answer for it either: with the shared-memory
heap it answers `0x80000416` for every plausible module name (`sensor_process`,
`sensorspd`, `sensor`, `sensors`, `sns_process`, `adsp_sensors`, `ssc`,
`sensor_pd`, `sensors_pd`, `adsp_listener`), so this firmware does not create
static PDs that way at all (`logs/090126-run.log`).

Android's own kernel registers a **service-location client** with the DSP's
`tms/servreg` (QRTR service 66) before it attaches, which is the step our
FastRPC chain never did; the QDI failures (timer, island manager, power
manager, watchdog) fit a PD spawned without the sensors configuration. The
driver now sends the mainline PDR register-listener request (`servreg=1`, path
`msm/slpi/sensor_pd`) in step 10 before the FastRPC chain
([`docs/slpi-fastrpc.md`](docs/slpi-fastrpc.md)). The first attempt to run it
met the loader's payload limit instead: with the SLPI firmware in the scene
the run payload was 67.4 MB against the loader's 64 MiB area, the stage failed
and left the loader's USB endpoint unresponsive (a physical restart is
needed). A scene without the audio firmware (`root-slpitest.apfs`, 27.2 MB)
fits and is built; the servreg answer and the DSP's last words after it are
the next measurement.

That measurement is in: the registration answers result 0 / state `0x1fffffff`
(UP), and with it the whole FastRPC chain runs afterwards — SMMU bypass,
`INIT_ATTACH_SNS`, `remotectl_open`, register, `init2` all retval 0, `next2`
waits (`logs/094147-run.log`, `logs/094812-run.log`). The sensor process still
dies at its timer client. A control boot with neither the attach nor the
registration writes the same crash item by step 10 (0x88 this time) while
item 424 is absent at step 1 (`logs/095517-run.log`, `logs/100002-run.log`):
the sensor process starts on its own at DSP boot, before the AP is involved.
The 0x88/0x89 value tracks the QDI handle the client receives (0x89 only with
the TMS proxy handler attached), so the failure is a later QDI call, not
`qdi_open` itself; item 473 holds binary state, not the DSP console. The
remaining work is the DSP-side timer client code, not the AP tunnel
([`docs/slpi-fastrpc.md`](docs/slpi-fastrpc.md)).

The AP side's last gap was the **APSS service locator** (QRTR service 64,
Android's pd-mapper): the DSP's servreg asks it where a service is served
before its protection domain finishes starting. The driver now serves it
(`servreg_loc_advertise`/`servreg_loc_input`, the open-source pd-mapper's
`servreg_loc.qmi`: service 64, get_domain_list 0x21 with the service name in
TLV 1, the domains in TLV 0x12) with the phone's own maps from the firmware
JSONs (`slpir/slpius/adspr/adspua/cdspr.jsn`: `msm/slpi/root_pd` and
`msm/slpi/sensor_pd` instance 90, `msm/adsp/root_pd`/`audio_pd` instance 74,
`msm/cdsp/root_pd` instance 76). In the boot the DSP's node 9 port 13 asked it
for `tms/pdr_enabled` and `tms/pddump_disabled` (answered, 0 domains — the
JSONs map only `tms/servreg`) and the SNS service 400 answered the SUID
request's QMI with result 0; the sensor process's crash item still reads
`TMR_CLNT_1:0x89 ... stalled initialization`, and no SUID event followed
(`logs/200444-run.log`). The locator is not the missing piece either: the stall
is the PD's own timer client, and the SUID lookup is accepted but silent — the
registry it may need is the next question.

The ack was the timer client's missing half. The AP now answers the notifier's
state-updated indication (0x22) with mainline's `SERVREG_SET_ACK_REQ` (0x23:
TLV 0x01 the path, TLV 0x02 the indication's transaction id), and the DSP
accepts every one (result 0, error 0). The timeout is gone: in
`logs/200251-run.log` the notifier reports the domain down and up (txn 2-5) as
the sensor PD restarts, and the DSP's last words changed from
`servreg_monitor.c:537:Servreg ack timedout:...` to
`EX:kernel:0x0:sns_sig_remote_:0x2061:PC=0xb01c1f2c` — a `memcmp` whose first
pointer is NULL, in the SNS remote-processor-state signal path. The
protection-domain-failure request (0x24) is answered too, so the reason is
read: the sensor process dies at `irq115:0x1072 ...
sns_err_handler.c:180:Crash`. The phone's persist registry (189 files) now
travels in the scene, but the process never opens it; the restart ordering
(`fastrpc_restart=1`) makes the fresh process crash immediately in
`frpck_1_2:0x5b` (`logs/201811-run.log`). What the SNS signal path compares
when the remote-processor-state interrupt arrives is the next question.

A control boot with no SNS request at all (`sns=0`) has **no
protection-domain failure** for the whole run — no `TMR_CLNT`, no `irq115`,
no item 424, the DSP up to the end (`logs/204756-run.log`). The SNS client is
therefore what destabilizes the sensor process.

The phone's own bytes were then read off the wire: a root strace of
`android.hardware.sensors@1.0-service` (strace exists at /system/bin) shows
the request as QMI 0x20 to service 400, the SUID placeholder 0xab, a
`sns_suid_req` of `{name, 1, 0|1}` (field 3 = 0 for registry, 1 for every
sensor), and TLV 0x10 as **length 0x0001, value 0x01**. The driver had put
its client byte in TLV 0x10's length high byte (length 0x0101 — refused with
result 1/error 1). The request now matches the phone byte for byte
(`logs/212720-run.log`: result 0/error 0), but no SUID event follows, and the
lookup itself still takes the process down (a config addressed to a known SUID
does not — below). What the client manager starts on that first request — and
the registry the process reads once at PD start, before the listener exists —
is the next question.

The same capture also holds the DSP's answers: the real SUIDs — the
accelerometer's is `0x1fbb6afc01727ea6`/`0x9a418d19f8d7ba44`.
Sending `SNS_STD_SENSOR_CONFIG` (513) to that SUID directly, with no lookup,
is **accepted** (result 0/error 0) and the sensor process **does not crash**
for the whole run (`logs/221040-run.log`: no `TMR_CLNT`, no `irq115`, no item
424). It is the SUID lookup (512) that starts the service the missing
registry cannot answer and takes the DSP down. No `SNS_STD_SENSOR_EVENT`
(1025) follows the config yet and the reverse tunnel still receives no
request, so the stream — the client manager's instance and its registry
group — is the next piece.

## Later product work

- [x] Integrate the phone's extracted UFSDxe/DiskIoDxe/PartitionDxe into the
  THYME UEFI, expose BlockIo/PartitionInfo, and pass the read-only survey.
- [ ] After that survey, make a separate accepted design for persistent storage.
- [ ] The rear camera 108 MP still through the capture stack. Capture, preview and the app
  switch are PASS (the camera rows in the table): the sensor streams at 12032x9024 into
  memory and the capture stack receives mode-3 frames; what remains is that still through
  the stack rather than the recording.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [ ] Refresh prebuilt/ from the verified tree: the tracked prebuilt/hakios-kernel is from 25-27 October, while the kernel verified on the phone today (dd3f6368: boot, Wi-Fi standing re-join, keepalive) and the userland that installed wget and its six dependencies are not in it. tools/update-prebuilt.sh regenerates the set; it is a large binary change, so it wants a deliberate pass rather than a loop turn.

- [x] Rootless Procursus prefix, dpkg metadata and offline install/remove verification.
- [x] Online repository installation through Sileo, from the page to the
  installed binary: PASS on 2026-11-13 (`logs/094507-run.log`; the page
  dismissed on `com.hakios.close-sheet`, the queue's bar, Confirm, apt's own
  `tree_2.0.1_install ok installed`, and `tree -L 1 /var/jb` printing the
  prefix -- see "Rootless package manager").
- [x] Sileo long-run stability and a package with dependencies. A package with six dependencies is PASS on 2026-11-15 (wget 1.24.5 and all six installed from the repository through Sileo, logs/070701-run.log); a longer Sileo session remains.
- [x] Performance and thermal soak under daily use: a 20-minute soak on the daily scene with tonight's camera kernel changes stayed live (settings pages at 150-900 s, battery flat, no panics, no memory pressure; `logs/040632-run.log`); only backboardd meets SIGSEGV during the kernel's abrupt return. A longer interactive soak and a thermal log remain. A 20-minute daily-scene soak on the current baseline (`logs/185653-run.log`, kernel `26257831`, root `8f5bc2aa`) ran two Wi-Fi joins with the scene's Safari/Settings traffic and GL frames, reached thermal **heavy** (75–78 °C, cores at their rows) under the load and settled again, and had zero panics, zero memory-pressure or jetsam lines, and a clean audio-DSP shutdown at 1374 s; the display slept once the scene's jobs ended, so its screenshots are black — it is an idle-stability soak, while the interactive evidence remains the 30-minute camera soak (`logs/003837-run.log`). A 30-minute camera-preview soak on the camera-focus build ran 20,927 frames (11.6 a second) with the UI answering at 5, 13, 22 and 29 minutes, no memory pressure or panic, thermal nominal/moderate until the return's teardown (one heavy, 76.3 °C) and the battery charging at 91–92 % (`logs/003837-run.log`, `logs/camsoak30-*.png`). A 20-minute daily soak on the servreg-ack baseline (`logs/230103-run.log`, kernel `bcc297c7`, root `8f5bc2aa`) joined the Wi-Fi network, kept every thermal read nominal (44–59 °C, hottest cpu-1-5), showed no panic or jetsam, and shut the audio DSP down cleanly at 1374 s; the Bluetooth daemon's short exits (117 over the run) are the same count as the earlier soak's and are its usual idle behaviour. A 22-minute daily-scene soak on the current build (`logs/102532-run.log`, kernel `09cb2178`, root `8f5bc2aa`, SHA-256 `e222104aa335a6ced2bc12c5170bd3fdc75e1c864f941cd0cde90da53114c1c9`) held the Wi-Fi join for 763 s (DHCP 192.168.0.103, live TCP at the end), counted 171 LE reports (50 named) with zero H4 transport faults, stayed thermal-nominal at 54-56 C (the usual five slowdowns), kept the battery on its charger at 80-82 % and 38-39 C, and showed no panic and no jetsam -- the only `memorystatus` lines are the teardown's at 1314 s; the UI answered at 6, 14 and 18 minutes (`logs/soak-360.png`, `soak-840.png`, `soak-1080.png`; the 10-minute capture failed to save). The full **30-minute** daily soak on the current build (`logs/174520-run.log`, kernel `401195712077`, scene `5f57d77b`, SHA-256 `b6f54637e3716fcc000709c6cced137479f5240d0f2c5cca49ae783a4f9518c9`) ran the whole 1800 s: en0 carried 24,854 frames out (1.98 MB) and 30,487 in (42.8 MB) with 2 dropped out and 0 in, held live TLS to Apple at 192.168.0.102 (`17.253.87.200:443`, RTT 58-63 ms at the end), counted 219 LE reports (65 named) with zero H4 transport faults, submitted 1,166 `hakios-gl` frames, stayed thermal-nominal (45-59.6 C) until the return's teardown (one moderate 69.3 C), showed no panic and no jetsam (the only `memorystatus` lines are the teardown's at 1918-1929 s), and the screenshots at 5, 10, 15, 20 and 25 minutes show the scene's Settings/Bluetooth page with the phone discoverable as `HakiOS Mi 10S` (`logs/soak-300.png`, `-600`, `-900`, `-1200`, `-1500`). The battery guard read 53% at boot and 53-56% through the run ("charging no": the PC port did not keep up under load), which is the known charge-under-load limit, not a fault.
- [ ] Battery-cycle integrity. The percentage jumps are explained: across every
  run log, a change of 2 points or more within five minutes happens only on a
  sample the driver itself marks `(voltage estimate)` -- its fallback when the
  gauge's counter is not ready at that read -- and the next read returns to the
  true value (77 % -> 53 % -> 76 % at 299/299/419 s in `logs/031850-run.log`;
  9 % -> 66 % in `logs/040411-run.log`). A full charge-to-empty cycle on the
  phone is still the open part. The loader printed a discharging phone's
  current as `I=18446744073709551406 mA` (-210 mA through an unsigned `%d`);
  its `print` now handles `%d` as signed (`xnuloader/loader.c`).

## The sensor DSP's DIAG channel is not a road to its log (2026-11-15)

A 400-second probe on the full daily set plus `slpi=1 sns=1 diag_mask=1`
(`logs/050305-run.log`, kernel `bac1b4236d3a`, scene `1f0519454f1a`) shows the
DIAG channel's open acknowledged but its DSP side never opened
(`DIAG asked 1 ack 1 theirs 0`, `0 asks, 0 replies`): the log-mask ask never
went out, because the driver's send needs one of the DSP's receive intents and
the router never offered one. The same boot's sensor process crashed and
restarted seventeen times from `TMR_CLNT_1 ... dog_virtual_user.c:240` and
`irq115 ... sns_err_handler.c:180:Crash`, with `0 module opens`,
`0 file requests` and `registry 0 of 0 written` -- it died in its own start,
before the file-service stage the 12:20 boot stalled at. The sensor framework's
own start remains the open piece, and no AP-side log channel reaches it (see
devices/thyme/docs/sensors-status.md).

Later the same day, a second probe with `fastrpc_early=1 sns_quiet=1` and no SNS message at all repeated the crash (34 reports from 244.7 s, `0 file requests`), while every boot carrying the FastRPC chain (`fastrpc_smmu=2 fastrpc_call=9`) has none: the crash tracks the absent listener session, not the messages (docs/sensors-status.md).

## The sleep that takes the network down, and the keepalive that brings it back (2026-11-15)

The dependency-install scene (`rc.jobs.sileodep`: wget and its six dependencies, image
`root-sileodep` `34a15896`, kernel `bac1b423`) reached Sileo's Install sheet with wget in
it and failed at the download: five
`hakios: Sileo: task after 10 s: state 3, status -1, error The Internet connection appears to be offline.`
lines, then `dpkg-query: no packages found matching wget`, no
`/var/jb/var/log/apt/term.log`, and no binary. The same log names the cause: at 05:35:22,
180 s after the scene joined the network, wifid ended the Wi-Fi session
`due to SystemSleep` (`SessionDuration=180`), and the link never came back --
`net: en0's link (the phone's radio (ath11k)) is down` at 1155 s, its frame counters
frozen at their join-time values. The taps at 05:39 woke the screen (the sheet is in the
720 s and 750 s screenshots), but nothing re-joined the network: wifid has no network of
ours among its preferences, so the scene's one join is the only one there is. That is the
"forgets the Wi-Fi" the owner reported, seen from the inside. The scene itself was sound:
the page opened, GET queued wget, `com.hakios.close-sheet` dismissed the sheet, the queue
bar opened the Install sheet, and only the download failed on the network (the same
choreography installed tree on 13 November).

The keepalive is now in every image: `net_test keepalive` (src/net_test.m) joins
`/redwood/fw/network` again when the link is not reachable, and
`com.hakios.wifi-keepalive.plist` runs it from boot. The first version asked the radio
for its state (Apple80211's STATE) and joined when it did not read 4 (RUN): its join
worked -- `logs/060142-run.log` has the ASSOCIATE at 155.4 s and the link back at
155.76 -- but that reading is wrong for the purpose. GET STATE reads 0 whenever the
station layer is not standing, which it also is not while the driver is simply carrying a
joined link; the join on that reading bounced a link that was up, and a join it asked for
at 83 s was refused `Device not configured` (ENXIO: no station). It now asks
SCNetworkReachability instead (configd's own answer, which follows the driver's link
events), waits 90 s before its first look so it does not race the scene's own join, and
retries a join that does not take after a minute.

That version's end-to-end test did not run: the phone was at 22 %, and the battery guard
ended the run at 203 s (`battguard: the charge is 22%, at or below battery_stop=25 three
looks in a row`), before the scene's 300 s idle and its tap. The re-join after a sleep is
therefore implemented and compiled but **not yet verified on the phone**; `rc.jobs.wifikeep`
with `battery_stop=10` (the phone charged first) is what will verify it.

Two more runs tried to drive that verification from a scene, and both showed why it
cannot be: the phone's system sleeps about a minute after boot when no application holds
the idle timer, and a sleeping system suspends every scene process. In `logs/064236-run.log`
the scene's `net_test wait 30 joinfile` began at boot and its join only happened at 226 s
(after the demo job had already ended); in `logs/065221-run.log`, with background taps every
five seconds from +5 s, the join still never ran and the demo job was still asleep at the
run's end. The keepalive daemon itself was suspended with them and resumed only at the
teardown, when it did exactly what it was built to do: the kernel's link-off event arrived,
it printed `the link is gone; joining /redwood/fw/network again`, and the join was refused
`Device not configured` -- the station layer was already gone with the driver's shutdown.
So the event path works end to end; what the phone does after a *wake by touch* (whether the
station layer is back by then) has to be seen with a hand on the screen, not from a scene.
`rc.jobs.wifidrop` (join, `net_test leave`, then the address and a name again) is the scene
that will answer it from a boot an application keeps awake.

**The drop/rejoin answer, 2026-11-18** (`logs/152900-run.log`, image `root-wifidrop.apfs`
`2387708c`): `sleep_hold 300` kept the system awake, the scene joined (`waitaddr` gave
192.168.0.105 after 2.5 s), `net_test leave` then dropped the link -- wifid logs `Link went
down due to None` at 15:30:37 -- and the keepalive printed `the link is gone; joining
/redwood/fw/network again` twice (its own join bounced the link once and it rejoined again).
After the sequence en0 still had its address and `resolve apt.procurs.us` answered
**172.67.138.140 104.21.46.121 in 0.02 s**, so the rejoin path works on hardware.  The first
resolve in the scene failed (`nodename nor servname provided`) because mDNSResponder was not
up yet; the final one is the link proof.  Wake-from-sleep is still the one path a scene
cannot drive: a sleeping system suspends the scene and the daemon with it, so that case needs
a hand on the screen.

### The dependency install passes (2026-11-15, later)

The same scene on the current image (`root-sileodep` `5fe04c8f`, with the Wi-Fi keepalive
in it, kernel `bac1b423`) installed the package and every dependency: Sileo's Install sheet
took wget, apt's `term.log` is stamped `2026-11-15 07:14:59`, `dpkg-query` reports
`wget_1.24.5_install ok installed` and `libidn2-0_2.3.7`, `libintl8_0.22`,
`libiosexec1_1.3.1`, `libpcre2-8-0_10.43`, `libssl3_3.2.1` and `libunistring5_1.2` all
`install ok installed`, `/var/jb/usr/bin/wget` is there (522,912 bytes), `wget --version`
answers `GNU Wget 1.24.5 built on darwin.` (`+https +ssl/openssl`), and the scene's last
line fetched `https://apt.procurs.us/dists/iphoneos-arm64-rootless/1800/Release` through it:
`Origin: Procursus`, `Suite: stable`, `Codename: 1800`, `Architectures: iphoneos-arm64`
(`logs/070701-run.log`). The Wi-Fi link stayed up for the whole run this time (no
`SystemSleep`); the keepalive's event reader was up (`reading the link's events`) and its one
join attempt fell at the teardown, when the driver was already going down (`Device not
configured`). So the previous run's download failure is the sleep behaviour, not the
dependency path: with the link up, Sileo's queue, apt's resolver and dpkg carry seven
packages through.

### Holding the idle sleep from a scene (2026-11-15, later)

A scene cannot wake a sleeping phone, and the system sleeps about a minute after boot when
nothing holds the display: three Wi-Fi attempts were suspended whole (`logs/064236-run.log`,
`logs/065221-run.log`). `userland/src/sleep_hold.c` asks IOPMrootDomain's user client for the
same setting powerd changes -- method 0 (kPMSetAggressiveness), type kPMMinutesToSleep (3),
value 0 ("never") -- holds it for a given number of seconds and puts the previous value back
on exit; `rc.jobs.wifidrop` now starts with `sleep_hold 400 &`. It is built into the image and
compiled (`root-wifidrop` `697e8820`), but **not yet run on the phone**: the phone went off at
about 10 % after the dependency run and was not reachable afterwards. The first run will show
whether an unprivileged job's call is accepted (powerd, also in the user session, makes the
same call), and whether the drop test finally runs its join and its `leave`.

The scene's gestures, read by `tools/frames.py` from the same log, mostly ran at 80-90
frames a second on the panel (the GET and Confirm taps at 86-90, over-16.7 ms shares of
0-2%); the one slow stretch is the queue at 596.9 s: 69 frames at 36.5 a second, 48% over
16.7 ms and 41% over 33 ms, with the window server waiting 42.5 ms on the application and
the copy 1.8 ms -- the application's own commits, the same shape as Sileo's scroll.

### A userland join is the standing network now (2026-11-15, later)

The station layer already re-joins the "standing" network whenever the radio is powered on
(`hakios.thyme_sta.c`, the power-on branch), but only a boot-arg join used to set it: a join
from userland (net_test, wifid) was consumed by the request and left `standing` empty, so a
Wi-Fi power cycle had nothing to re-join. `hakios.thyme_apple80211.c`'s set_join now asks
for the standing form (`hakios_sta_ask_join_standing`), which keeps the network and still
falls back to the previous one if the join does not come through. Kernel `dd3f6368` carries
it (built; the driver file compiled with no errors). `net_test` gained a `power 0|1` step and
`rc.jobs.wifipower` uses it -- join, an address and a name, the radio's power off and on,
then an address and a name again with no joinfile between them -- which answers whether the
station layer's own re-join covers what the system's sleep does. Both are **built but not yet
run**: the phone has been off USB since the dependency run.

 runs both tests in sequence from an Android state
(battery_stop=10, 300 s each, with the loader waits between them) and prints from each log
the lines that answer the two questions: the power step and the address/resolve after it, the
keepalive's link event and its join.

### The persistence machinery that is in the tree (2026-11-15)

The tree already carries a data-volume design for the port's own partition, and none of it
has been exercised on thyme:

- `drivers/storage/hakios.ufs.c` reads every GPT partition through the loader's UFS service and
  refuses every write but the port's own: with `ufs_write=1`, `/dev/rufs_darwin` takes whole
  blocks from `UFS_WRITE_FIRST` (darwin block 2042368) on, and `/dev/ufs_darwin_data` is darwin
  from that block to its end as a writable HFS+ volume; without the argument nothing writes.
- `userland/src/var_store.c` packs `/private/var` into a two-slot archive (2 GiB a slot,
  written in turn, header last, checksummed) into `/dev/rufs_darwin` at block 2097152 and
  unpacks the newest good one at boot; `com.hakios.varstore.plist` runs it with `var_store=1`.
- `tools/apfs/mkstore.py` makes the store volume once, and `tools/darwin-write.py` is the only
  writer of darwin from Android: it checks the GPT, the by-name link, the bounds and the
  read-back, and writes only with `DARWIN_WRITE=yes`, only into darwin.

The daily arguments carry neither `ufs_write` nor `var_store`, so a daily boot neither writes
nor loads any of it; the persistence row stays open until a round trip is shown. The read-only
steps are `tools/darwin-write.py check` and `selftest` from an Android boot with root; any real
write to darwin is an irreversible operation and needs the owner's explicit authorization
first (the project's rule).

On this phone (2026-11-15, read-only `tools/darwin-write.py check` with the phone on
Android at 5 %), `by-name/darwin` is not the partition the port was set up with: it
resolves to a partition named `rescue`, start block 22020096, 262144 blocks (128 MiB),
where the tool expects sda34, block 202997760, 41943000 blocks; the guarded writer refused,
which is its gate working. There is no darwin partition on thyme yet, and creating one is a
repartitioning operation that needs the owner's explicit authorization.

### The power-cycle run and what actually happened (2026-11-15, later)

The scene `rc.jobs.wifipower` ran (`logs/090945-run.log`, kernel `dd3f6368`, image
`cf9f1e3d`) but its own steps never got a chance: `sleep_hold` declined to hold (the root
domain answered the read of `kPMMinutesToSleep` with a failure, and the tool refuses rather
than leave the system never sleeping afterwards), and the system's idle sleep then suspended
the demo job before its `wait 30` was over -- its `power 0`/`power 1` and the address/resolve
lines are not in the log. What the log does show is a real loss and a recovery: at 242.65 s
the access point deauthenticated us (reason 3), and at 247.67 s an ASSOCIATE -- five seconds
later -- went through to association accepted at 248.20 and the link up at 248.35, with
traffic after it (a TCP connection at 264 s). The per-request log line naming the caller was
beyond the driver's 200-line budget, so which process asked is not in the log; the
keepalive's own only join is at 419.4 s, at the teardown, where it got `Device not
configured` -- the daemon had been suspended with the rest of userland and processed the
queued link events when it resumed. So this run shows wifid can recover a deauthenticated
link by itself, does not exercise the kernel's standing re-join, and shows that the
keepalive cannot act while the system is suspended (it acts on the queued events after a
resume, which is what the teardown join was).

The read the tool declined on is now explained: the root domain answers 
(0xe0000001) when no record for  exists yet, with the value 0
() -- powerd sets the record later in a run. 
now treats that answer as "absent": it holds, and on exit sets a minute back rather than the
absent 0. Whether the set itself is permitted from the user session is the next run answer.

One more run with the hold working (`sleep_hold: idle sleep held off, kPMMinutesToSleep was
0`, `logs/095245-run.log`) still did not run the scene: only the first background touch
fired and the join never happened -- the display had gone away and the demo job with it. So
the display timer is held too now (`kPMMinutesToDim`, same absent-record handling,
`sleep_hold`); the next run answers whether both timers held keep the job running and let
the power cycle and its address/name checks happen.

### The standing re-join works: after the power cycle the station joins by itself (2026-11-15, later)

The scene ran at last once Sileo was opened first -- an application in front that sets
idleTimerDisabled is the one thing that keeps SpringBoard from locking the display (PM
timers held and injected touches did not; `logs/095245-run.log`, `logs/100719-run.log`), and
a locked display suspends the job. With Sileo open, `sleep_hold` holding both timers, and the
jb system image (`logs/101852-run.log`, kernel `dd3f6368`, image `7eff8fbf`): the join
returned 1 after 1.8 s, an address came back in 2.8 s and a name resolved in 0.03 s;
`net_test power 0` was taken; five seconds later `net_test power 1` was taken and, with no
joinfile between them, the station layer itself re-joined the standing network -- `lx: iOS
join reached the station driver`, authentication accepted, association accepted as station
15, `station driver join returned 0`, and wifid logged the link-state change at 10:20:38. The
scene then asked an address and a name again and both came back. So the kernel change (a
userland join is the standing network; `hakios.thyme_apple80211.c`) does what it was built to
do: a Wi-Fi power cycle brings the network back without userland asking. What is not yet
shown is that the system's own sleep takes the radio through this same power transition;
that is the owner's hand-on-screen test.

`rc.jobs.wifidrop` now opens Sileo first for the same reason (an application in front keeps
the display from locking; `devices/thyme/wifi-verify.sh` points both scenes at the jb system
image), so the keepalive\x27s event path can be tested with the job actually running.

### The keepalive brings a dropped link back (2026-11-15, late)

The drop scene ran with Sileo opened first (display held), both PM timers held, and the jb
system image (`logs/103116-run.log`, kernel `dd3f6368`, image `fc6638e5`): the join returned
1 after 2.0 s, an address came in 2.5 s and a name resolved; then `net_test leave` was taken
and the link went down at 301.96 s. Five seconds later -- the keepalive's confirmation
window -- the daemon printed `the link is gone; joining /redwood/fw/network again`, its join
returned 1 after 0.3 s, and the kernel logged the link up at 307.75 s; the scene's last
address and name came back (resolve in 0.02 s). The daemon had read the kernel's data-link
event and acted on it by itself: the path the sleep case needs, now shown on the phone for a
link that goes away while the system runs.

### The owner test: no black on a language change or a restart (2026-11-15, night)

The owner reported on the current daily build, in their own use, that switching Sileo's
language and quitting and restarting it no longer shows the black screen ("不管切换语言还是
退出重启都没有出现黑屏问题"). That is the user-facing confirmation of the work recorded
above: the image table's exhaustion (IMAGES 1024), the /var/jb container link that stopped
the dead10cc kills, and the two-second cap on the launch's blocking connects
(`net.inet.tcp.keepinit = 2000`, `logs/040630-run.log`: one dark frame, about two seconds).

The same day's short launch profile (`logs/104511-run.log`, `appprof=1 appprof_secs=180`)
shows the first ten seconds of Sileo after the fix: the first five-second window has the
main thread in `mach_msg2_trap` for 93 of 250 samples and `__connect` for 83 -- the launch's
blocking connects, now bounded by the two-second keepinit -- and the second window is
`mach_msg2_trap` for 199 of 250 and a semaphore for 23, the run loop waiting; the sampler
then stopped printing because the run ended there: the demo job'''s commands ran only at
about 400 s of the run (the job had been suspended before, as in the power-cycle attempts),
so the profile covers the launch at the run'''s end -- a sketch, not the whole scene.

### Location Services: locationd was never loaded (2026-11-15, the owner's finding)

The owner looked for where Location Services is turned on and found no entry. It is not a
missing toggle: the system's locationd is in the system image, its plist has RunAtLoad, and
launchd never loaded it because the build's job list (userland/build.sh, DARWIN_JOBS) never
named it -- every client's lookup fails (`failed lookup: name =
com.apple.locationd.registration`, from wifid, SpringBoard, chronod and parsecd in
logs/101852-run.log and the Sileo runs), so CoreLocation has no daemon and Settings shows no
Location Services row. `com.apple.locationd` is now named (LOCATIOND=0 leaves it out), and the run that
followed is in the last section of this file: the daemon loads, the switch does not take. Naming it brings the daemon, its Mach services and the API, not a source
of positions -- the GPS paths (the modem's QMI LOC service and the SSC's GNSS sensor) are
not brought up (cellular is TODO; the sensor framework's client manager still stalls), and
network positioning asks Apple's service.

The first boot with the job named (`logs/115319-run.log`, image `34243eb0`) shows the next
step of the same story: launchd queues the job, spawns it, and fails with
`Could not find and/or execute program specified by service: 2: No such file or directory:
/usr/libexec/locationd` -- the plist was in the image but the program is not. The OS cryptex
is where Darwin 16.5 keeps locationd, and this tree's cryptex extraction (`sysimage/cryptex`,
7z over `cryptex-os.dmg`) reported 979 "Unsupported Method" sub-item errors and kept only a
fraction of it; its `usr/` holds `lib` and no `libexec`. The IPSW those materials came from
(6.17 GB, iPhone10,3/10,6 16.5 20F66) is not on this machine now, so the binary cannot be
re-extracted until it is found again.

The extraction path for that is now proven on the one cryptex we still have: the python
reader (`tools/apfs/apfs.py`) fails on a sealed volume (the root inode object at block 21
has a zeroed header), but the kernel's own APFS code built as a host program
(`tools/apfs/build-tool.sh` -> `apfs-work/apfs-tool`) reads the same image: `ls` lists
`/usr/libexec`, and `cat /usr/libexec/webinspectord` returns a Mach-O (decmpfs type 14, the
bytes in the resource fork, 168,416 bytes uncompressed). So when the IPSW returns,
`cryptex-os.dmg` -> `apfs-tool ls/cat` gives locationd and whatever else the OS cryptex
holds, without 7z's unsupported-item holes.

### Location Services: the daemon runs, the switch does not take (2026-11-15, night)

With the IPSW re-downloaded (checksum verified against IPSW.me) and the rootfs re-extracted
with locationd kept (`sysimage/select-system.py`), the system image built
(`userland/system.apfs`, 2.57 GB) and the phone booted from it, launchd now starts the
daemon and it stays: `Successfully spawned locationd[18]` and no failed lookup of
`com.apple.locationd.registration` in any later log (`logs/130702-run.log`,
`logs/131736-run.log`), and locationd opens TLS connections of its own (TCP summaries in
130702-run.log, 63-103 s sessions). But Settings still has no Location Services row, and
CoreLocation answers for itself (`userland/src/loc_test.m`): `locationServicesEnabled = 0`,
`authorizationStatus = 2`. `rc.jobs.locenable2` then wrote `LocationServicesEnabled = true`
for `com.apple.locationd` into all four preferences domains (each read back true), terminated
locationd (`killby locationd 15`, launchd spawned a fresh one) and asked again: still 0
(`logs/141445-run.log`). So the master switch is not that preference, or not read from there
by iOS 16 locationd/framework -- the next probe is locationd state files
(`/var/mobile/Library/Preferences`, `/var/db/locationd`, `/var/mobile/Library/Caches/locationd`)
and, beyond that, the daemon's private XPC. What exists now is the daemon and its Mach
services; the Settings entry, a working switch and any real fix still need that switch path
and the GPS providers (modem QMI LOC or SSC GNSS), neither of which is up.

Two more probes closed the obvious candidates and left the daemon's own initialization as the
remaining question. `rc.jobs.locfiles` (`logs/142915-run.log`) shows no `/var/db/locationd`,
no `/var/mobile/Library/Caches/locationd` -- the state directories a stock data partition
comes with, which the daemon cannot make itself (/var/db is root's and it runs as mobile) --
while `com.apple.locationd.plist` (134 bytes) and
`com.apple.locationd.StatusBarIconManager.plist` do exist under the mobile preferences. The
var skeleton now creates both directories (`sysimage/mkvarskel.py`), the root image was
rebuilt, and the switch written again: CoreLocation still answers
`locationServicesEnabled = 0` (`logs/144102-run.log`). So it is not the preference and not
those directories: what is left is locationd's own start, which keeps looking for daemons
this image does not carry (`com.apple.fairplayd`, `com.apple.commcenter.xpc`,
`com.apple.routined.registration`, `com.apple.icloud.searchpartyd.*`), and the private XPC
by which it publishes the switch. A real fix needs that path and, for positions, the GPS
providers (modem QMI LOC or SSC GNSS), neither of which is up.

Adding routined -- the one daemon of those lookups whose program is in the image -- to the
job list starts it (`com.apple.routined [11]`, `logs/145410-run.log`) and changes nothing:
`locationServicesEnabled = 0` still. What locationd also asks for, `com.apple.fairplayd` and
`com.apple.icloud.searchpartyd`, has no program in the rootfs extraction at all. The switch
therefore stays with locationd's private path (its XPC, or a platform service this image does
not carry), and the thread is parked there: what is proven is the daemon and its registration,
not a working switch and not a position.

The daily artifacts carry all of it as of tonight: `out/thyme-boot/root-wirelessfull.apfs`
`f98c088d` (the var skeleton with locationd's directories, the job list with the daemon, and
`loc_test enable` as the scene's first line so Location Services is on at every boot) and, as
the runners' default system image, the rebuilt `userland/system.apfs` -- the same file as
`devices/thyme/prebuilt/system.apfs` (one inode), rebuilt clean at 18:18 after the plist
experiment was reverted, with locationd in it -- and the kernel `dd3f6368` with the
standing-network re-join.

A no-tap cold boot of the default artifacts (`rc.jobs.dailycheck`, image `03f6377d`, the
runners default system image; `logs/151118-run.log`) starts what tonight put there:
`Successfully spawned locationd[19]`, the keepalive reading its link events, and the Wi-Fi
join returning 1 after 0.9 s. The scenes later steps (address, name, CoreLocation) were
suspended when the display locked, as in every scene without an application in front -- no
new fault, and the short check was not meant to hold the screen.

### Cellular: the SDX55M's PBL answers on the current kernel (2026-11-15, night)

The modem firmware was pulled from the phone itself into `fw-modem-sdx55m/` (18 files,
85 MB, from `/vendor/firmware_mnt/image/sdx55m` through a root copy in
`/data/local/tmp`: `sbl1.mbn` 548,056 bytes, `qdsp6sw.mbn` 84,004,996, plus
`multi_image.mbn`, `tz.mbn`, `xbl_cfg.elf`, the EFS images and the rest; the directory is
Git-ignored, it is the vendor's). With `sbl1.mbn` staged and the scene
`root-modemprobe.apfs` `7dcd63b6`, `devices/thyme/modem-probe.sh` ran on kernel
`dd3f6368` and produced its whole evidence trail (`logs/153232-run.log`, SHA-256
`3be7c2e6...c38053`): the loader handed the image at `0xc8b68000` (first words
`0x464c457f` -- an ELF), the doorbell answered `status 0xffffffff (result 3), errcode
0xef120700, dbg 0xc0000000 0 0x85cd8, MHISTATUS 0` after 80 ms, AER stayed clean
(uncorrectable 0, correctable 0, root 0), PARF reported int status 0x2000 / status 0x4c60,
and the SMMU recorded `CB0 FSR 0x400 FAR 0xd55bef8e`. The probe PASSes as evidence, not as
a boot -- and the first run, without the BDF-to-SID table programmed, is the `0xef120700`
case the cellular document calls "no transfer at all". With `pcie2_sid=5` the doorbell
answers the documented best state instead (`logs/154028-run.log`, SHA-256
`d589b169...cc9738`): `0xef0e0003` in 30 ms, dbg `0x20000000 0 0x85cd8`, AER clean. So the
transfer, the size and the signature pass and the refusal is after them -- exactly where
`docs/cellular.md` left it: the secure side of the modem's boot or inside the SBL's start,
which this kernel cannot observe. The firmware set now in the tree is what any further probe
needs.


### Location Services: the switch is locationd's own preference of the mobile user (2026-11-15, late night)

The daemon was never the reason the switch stayed off: the writes went to the wrong home. `loc_test
enable` ran from the rc job as root, and CFPreferences' "current user" for root is /var/root; locationd
starts as mobile (user/501 in every run, `Successfully spawned locationd[19]`) and reads the switch from
/var/mobile/Library/Preferences/com.apple.locationd.plist. The tool now calls setgid/setuid(501) before
writing, writes the key the dyld cache names -- "LocationServicesEnabledIn8.0" (earlier probes wrote the
legacy "LocationServicesEnabled" only) -- as a CFBoolean in current-user/any-host, and prints the type it
reads back. With root image `2220401a...` on kernel `dd3f6368` and `userland/system.apfs`
(`logs/163514-run.log`):

    loc_test: writing as uid 501 gid 501 (boolean)
    loc_test: current user, any host: write ok, read back boolean true
    loc_test: any user, any host: write FAILED, read back boolean true

(the any-user write needs root and is refused after the drop to 501; it is not the domain locationd
reads). `killby locationd 15` restarts the daemon and the next question answers
`locationServicesEnabled = 1`, `authorizationStatus = 0` -- 2, denied, in every earlier boot. The second
pass with the number 1 (`enablenum`) reads back as a number and answers 1 as well, so the boolean form is
the one to keep. What changed is CoreLocation's answer, the privilege the framework reports for the
caller, and not yet the user-visible pane: the Settings Privacy page still leaves Location Services out
even when it is opened after the switch reads 1 (`logs/sileo-220.png` of the 16:46 run), and
`prefs:root=Privacy&path=LOCATION` still lands on the page without the row. The privacy plist names the
row (`PrivacySettingsUI.framework/Privacy.plist`: PSLinkListCell, detail PUILocationServicesListController,
get locationServicesEnabled:, label LOCATION), and a `clcheck` probe (`logs/164626-run.log`) shows the
framework dlopens and PUILocationServicesListController, PUILocationServicesCell,
PUILocationServicesAuthLevelCell, PUITCCAccessController and PUITrackersController all resolve -- so the
row is dropped above class resolution; the code path that names it (`locationServicesSpecifier` is a
selector in the cache of Preferences.framework) is the next thing to read.  `userland/patch-privacy.py`
(a few lines of experiment: it takes the row's only `get` out as the system image is built) was the
test of whether the getter is what drops it -- it is not: the rebuilt image carries the row without
the getter (checked in the image with `tools/apfs/apfs.py cat`), and the page is unchanged
(`logs/sileo-120.png` of the 17:54 boot, rc.jobs.locrow).  The setup-completion preferences a new
phone has (purplebuddy: SetupDone, SetupFinishedAllSteps, SetupLastExit, UserHasFinishedSetup, and
Preferences' first-time location-consent marker) are what rc.jobs.locrow3 then wrote as the mobile
user before opening the page -- and the row is still out, on a clean page with no alert over it
(`logs/sileo-120.png` of the 18:00 run).  So the getter, the classes, the switch value and the
setup state are all ruled out.  The framework-load question is answered too: a few lines injected
into Preferences (`libhakios.prefs.dylib`, then named for Preferences in INJECT) opened
PrivacySettingsUI in the app's own constructor -- init's `library insertion:` line carries it in
`logs/181029-run.log` and Settings started normally with it -- and the row is still out
(`logs/sileo-120.png` of the 18:10 run).  So the getter, the classes, the framework load, the
switch value and the setup state are all ruled out; what is left is Preferences' own specifier
code (the `locationServicesSpecifier` method the cache names) and whatever it asks CoreLocation
about the hardware.  The plist patch and the injection were experiments and are reverted; the
system image is rebuilt without them.  The row stays an open item, and location itself does not
depend on it: the switch is on at every boot and applications get Wi-Fi positions.

For positions the command line is the wrong kind of client: `loc_test watch 45` (`logs/164626-run.log`)
answers `services 1, authorization 0`, then CoreLocation itself says "This app has attempted to access
privacy-sensitive data without a usage description. The app's Info.plist must contain an
NSLocationWhenInUseUsageDescription key", the authorization change comes back 0 and the request fails
with `kCLErrorDomain 1` -- denied before TCC or locationd is asked. The smallest real client is now
`LocProbe.app` (`userland/src/LocProbe.m`: bundle id com.hakios.locprobe, usage description, URL scheme
hakiosloc), built into the root image and registered by a scene from /var/jb/Applications; whether it
gets a fix, and whether a consent alert appears at all (tccd is not in the spawned list of the run), is
what `rc.jobs.locapp` asks.

Until /private/var is persistent the switch has to be written at every boot; the daily scene
`userland/rc.jobs.wirelessfull` (and the base `userland/rc.jobs`) now does what Setup Assistant
does on a stock phone, with `loc_test enable` as its first line.

### Location Services: the consent prompt renders, and locationd scans Wi-Fi (2026-11-15, late night)

`LocProbe.app` answered the question the command line could not. Built into the root image
(`5ca0a253...`, kernel `dd3f6368`, system image `userland/system.apfs`), registered from
/var/jb/Applications by `uicache -a`, SpringBoard launches it (`Successfully spawned LocProbe[224]`,
bundle path `/private/var/jb/Applications/LocProbe.app`, a data container made for it), and the
CoreLocation consent alert renders exactly as it does on a stock phone --
`CoreLocationVanillaWhenInUseAuthPromptPlugin[226]` is spawned and `logs/sileo-160.png` of the 17:04
run shows "Allow "LocProbe" to use your location?", the usage description, "Precise: On", and Allow
Once / Allow While Using App / Don't Allow (`logs/170645-run.log`). The alert was left unanswered in
that run, so the application stayed in notDetermined; `rc.jobs.locapp2` taps "Allow While Using App"
at (0.5, 0.694) of the screen, the place that screenshot puts the middle button.

The run also showed that the provider below the switch is alive. With the application in front,
locationd asks wifid for scans (`scanType=2 client='locationd'`) every few seconds and gets real access
points back -- the driver logs the scan it is asked for and what it heard ("THE SCAN HEARD 3 NETWORKS
(access points): 0 at 2.4 GHz, 3 at 5 GHz ... the strongest at -43 dBm", `lx:` lines beside the wifid
`WiFiSettlementObserver` results). What locationd's own Wi-Fi database makes of them is nothing yet:
`TBCoreDataSource _executeFetchRequest: fetched 0 results` for each scan, and wifid's own
`WiFiLocationManager locationManager:didFailWithError:` reports `kCLErrorDomain Code=0`
(kCLErrorLocationUnknown, not denied) -- scans without a position. Whether the network path to Apple's
location service then supplies one is what the answered run will show.

The application cannot write /var/jb as a sandboxed bundle (its first attempt left no
/var/jb/locprobe.log, and `cat` said so); the scene now makes /var/jb/locprobe world-writable before
the launch and the application falls back to its container's Caches directory, printing both.

### Location Services: consent answered, no position yet (2026-11-15, late night)

The one scripted tap works.  `rc.jobs.locapp2` (root image `657ebe15...`) opens LocProbe first and
taps (0.5, 0.694) twelve seconds later; the application's log, written to the world-writable
/var/jb/locprobe the scene makes before the launch, says what happened (`logs/171653-run.log`):

    17:17:54 launched: services 1, authorization 0, home /var/mobile/Containers/Data/Application/6A39739E-...
    17:17:54 authorization 0
    17:17:56 failed: kCLErrorDomain 1
    17:18:06 authorization 4
    17:18:09 waiting: authorization 4
    ...

Authorization 4 is kCLAuthorizationStatusAuthorizedWhenInUse: the alert was the one the 17:04 run
photographed, and the tap landed on Allow While Using App.  The kCLErrorDomain 1 two seconds before
that is the request made while the alert was still up.  From 17:18:06 the application is authorized
and asks for updates, and for the rest of the run (two minutes) it gets none -- no location lines and
no errors after the consent.

Below it, locationd kept asking wifid for scans (62 `client='locationd'` results in the run) and wifid
kept scanning through the driver; every scan's `TBCoreDataSource _executeFetchRequest:` still
`fetched 0 results`, and wifid's own `requestLowAccuracyLocationUpdate` (its locale path) repeatedly
reports "location is nil".  So the input to Wi-Fi positioning exists and the client is authorized, but
nothing resolves the access points to a position -- locationd's own os_log is what the next scene
puts on the console (`OSLOG=com.apple.locationd`, rc.jobs.locapp3).

### Location Services: locationd skips the ALS query for "connectivity" (2026-11-15, late night)

With locationd's own os_log on the console (`OSLOG=com.apple.locationd` in the root image,
`9933462c...`, `logs/172613-run.log`), the Wi-Fi positioning flow says where it stops.  The scans
reach it -- each ALS request carries twelve access points -- and locationd builds the query:

    [WifiPosition] @WifiFlow, sufficiency, 0, nofix
    [WifiPosition] #alsquery, do_query, 1, reason, unknownratio
    [WifiPosition] @WifiFlow, alsrequest, 1, unknownratio, aps, 12, type, 0, vni, 0, 12, 0
    [GeneralCLX] @AlsFlow, query, in, cells, 0, 0, 0, 0, 0, wifi, 12, cache, 1, piggyback, 1
    [GeneralCLX] @AlsFlow, query, skip, connectivity

and then answers itself `alsreply, aps, 0, tag, -1` and `outcome, nofix`.  Every one of the run's
51 ALS queries is skipped the same way (`skip, connectivity`), so no request ever leaves for
Apple's location service.  The next scene (`rc.jobs.locnet`, `logs/173718-run.log`) shows that
skip was the truth of that boot rather than a SystemConfiguration fault: `reach_test` (src/reach_test.m)
gets flags 0 for gs-loc.apple.com and for the default route and `getaddrinfo` fails for both hosts
(nothing resolves), and the kernel's own link summary says `the network is NOT joined, 0 s after it
was joined: 0 frames sent`.  configd itself is spawned (`com.apple.configd [35] ... launching: ipc
(mach)`), so the gate below is the link, not the reachability service -- these scenes never ran the
`net_test joinfile` that the scenes carrying traffic run, and the boot keepalive alone did not join
this time.  `rc.jobs.locfinal` joins explicitly first, checks the sockets, and only then asks
locationd.

That run's scene also showed an application-launch ordering that matters: LocProbe opened three
seconds after `uicache -a` did not come to the front (the screenshots are of the home screen and
/var/jb/locprobe stays empty), while the same lines with Sileo opened first (rc.jobs.locapp2) work.
Give LaunchServices the time, or open Sileo first.

### Location Services: Wi-Fi positions reach the application (2026-11-15, night)

The run that got the link up answered the question.  `rc.jobs.locfinal` (root image `479ba13b...`,
kernel `dd3f6368`, the system image with the privacy-plist experiment) opens with `net_test joinfile`,
whose call is refused at ten seconds -- `net_test: joinfile: the kernel refuses ASSOCIATE: Device not
configured` -- because the radio is not up that early; the link then comes up on its own and the
kernel's summary at 216 s says `the network is joined, ... 14001 frames sent ..., 57772 received`
(`logs/174434-run.log`).  From there the whole path works:

- no ALS query is skipped for connectivity in the run (0, against 51 of 51 in the link-down run);
- after the first ALS query the flow logs `#alsquery, do_query, 0, reason, sufficient`, its Wi-Fi
  database now knowing the access points;
- locationd delivers to the application, five hundred times in the run:

      [Client] {"msg":"Sending location to client", "client":"[mobile]com.hakios.locprobe",
      "location":{..., "type":"WiFi", "lat":34.14937511011594, "lon":108.90276852097956,
      "horizontalAccuracy":141.81, "confidence":66, "altitude":449.05, ...}}

  and at its best 44 m of horizontal accuracy; the authorization the daemon reports for the client
  is `kCLClientAuthorizationStatusAuthorizedWhenInUse`.

What this proves is network positioning end to end -- the master switch written as the mobile user,
the consent alert answered by one tap, wifid's scans, the ALS lookup over the phone's own Wi-Fi, and
CoreLocation delivering `CLLocation`s of type WiFi to a third-party application.  It is not GNSS: the
satellite and cell sources stay where `docs/cellular.md` and the SSC notes leave them (the modem does
not boot past the secure side; the sensor framework's client manager does not open).  Accuracy is
tens to about 150 m, which is what Wi-Fi positioning gives; maps and apps that ask for a location
work, a car-navigation-grade fix does not.  A scene that needs the network should retry `joinfile`
once the radio is up (or wait for the link), not call it once at ten seconds.

### The daily image turns Location Services on at boot (2026-11-15, night)

The rebuilt daily artifact (`root-wirelessfull` `aafd733a...`, the clean system image of 18:18,
kernel `dd3f6368`) runs its first line as the mobile user, and the scene's last question confirms
it (`logs/182439-run.log`): `loc_test: current user, any host: write ok, read back boolean true`
at 18:24:50, and at 18:28:19 -- after the whole wireless gate -- `locationServicesEnabled = 1`,
`authorizationStatus = 0`.  The gate itself is as before: Safari's TCP session completes (5984
bytes in, `FIN_WAIT_1`, 53.7 s), Bluetooth preferences open, and the link is joined late in the run
-- the scene's first `net_test wait 20 joinfile` is still refused (`Device not configured`, the
radio is not up at half a minute) and the standing/keepalive join brings it up at 165 s.  That
early joinfile is the scene's known call; it is not a location regression.

### Charging: the daily boot still discharges through the cable (2026-11-15, night)

The bytes of the charging question are at the very top of the daily run's log (`logs/182439-run.log`):
`battery: flags=1f SOC=59% V=3948 mV I=-208 mA T=32 C action=7` at boot, and the guard's own
verdict later in the same run -- `battguard: charge 56% (battery present, charger attached,
charging no); stop at or below 10%`.  The firmware charger path (QcomChargerDxeLA behind the
loader) is attached but is not putting charge in under the kernel's load, which is the known
"PC port does not keep up" limit and not a fault of any one run; what the port still lacks is the
*system's own* charger control -- the kernel has never written an input current limit or enabled
the PM8150B charger, and every charge so far has been whatever the bootloader left behind.  The
next step is a read-only one: a kernel probe of the charger peripheral's registers (the SPMI
arbiter and its read/write path are already in `bsd/dev/hakios.pmic.c`) to see the state the
firmware leaves and what the local input current limit is, before anything is written.

### Charging: the charger is enabled and "fullon", so the input side is next (2026-11-15, night)

A read-only dump of the PM8150B's charger block (boot-arg `pmic_charger=1`, kernel `f28f6f98`,
`logs/184201-run.log`) shows what the firmware left behind, and it is not "charging switched off":
the block answers at slave 2 0x1000 (type 0x02 subtype 0x80, channel 30), and

- `0x1042 = 0x01` -- the charging enable bit is set;
- `0x1006 = 0x03` -- battery charger status 1 says FULLON (fast charge), not inhibit or disable;
- `0x1061 = 0x1e` -- fast-charge current 30 x 50 mA = 1.5 A;
- `0x1070 = 0x50` -- float voltage 3.6 V + 80 x 10 mV = 4.40 V;
- `0x1051 = 0x38` -- the analog charge inhibit bit (BIT(0)) is clear;
- `0x1090 = 0x1b` -- hardware JEITA is enabled with several of its limit bits.

So the charger believes it is charging, while the loader reads `I = -208 mA` and the guard says
"charger attached, charging no".  What the dump did not cover is the input side -- the USBIN block
(0x1300, where the input current limit lives at 0x1370), the Type-C block (0x1500) and the MISC
block (0x1600, the watchdog) -- and that is where the rest of the state is.  Reading those through
the charger's channel was the first try and it is wrong: the arbiter selects the peripheral by the
channel and takes only the low address byte, so 0x1370 answered exactly what 0x1070 had
(`logs/184759-run.log`: the "USBIN" 0x1370 line is byte-identical to the charger's 0x1070 line).
Each block is its own peripheral with its own channel on the same slave -- from the inventory,
slave 2 has 0x1000 on channel 30, 0x1100 on 31, 0x1200 on 32, 0x1300 on 33, 0x1400 on 34, 0x1500 on
35 and 0x1600 on 36 -- and the probe now finds each by its ppid and reads it there.

The register facts are now sourced rather than remembered.  Xiaomi publishes no thyme branch
(`MiCode/Xiaomi_Kernel_OpenSource` has none; the closest is `cmi-r-oss`, Mi 10 Pro, same SM8250 +
PM8150B) and the DTS facts come from the third-party thyme tree, whose `smb5-reg.h` is byte-identical
to Xiaomi's.  From `smb5-reg.h`, `smb5-lib.c` and `qpnp-smb5.c`: ICL is 0x1370 with code = uA / 50 mA
(500 mA = 0x0a), FCC is 0x1061 with the same 50 mA step, FV is 0x1070 with (uV - 3.6 V) / 10 mV,
charging enable is 0x1042 BIT(0), and the vendor driver enables the charging path at init, disables
hardware JEITA, and lets APSD/cable events program ICL -- thyme's DT asks for 3 A USB ICL, 6 A FCC
and 4.45 V, none of which a first bring-up should copy.  The MISC watchdog matters too: the vendor
pets 0x1643 on the bark interrupt and configures a bite to disable charging (0x1653 BIT(7)), so a
path of ours with no interrupt must either pet it or turn the timer off (0x1651 BIT(1)/BIT(6)).

### Charging: the charger works; the port's 500 mA is the limit (2026-11-15, late night)

The per-channel dump completes the picture (`logs/185404-run.log`, kernel `3cf83ada`), and nothing
on the phone's side is broken:

- Type-C (channel 35): `0x150B = 0x2b` (attached, sink), `0x1544 = 0x02` (sink only, not disabled,
  not source-only), `0x1506 = 0x08` (the port advertises standard current);
- USBIN (channel 33): `0x1308 = 0x01` (APSD classified the port SDP), `0x1340 = 0x00` (input not
  suspended), `0x1370 = 0x0a` (input current limit 500 mA);
- DCDC (channel 31): `0x1106`, `0x1107` and `0x1108` are all `0x0a` -- the maximum, the status and
  the AICL result agree on 500 mA;
- charger (channel 30): enable `0x1042 = 1`, status `0x1006 = 3` (fullon), FCC `0x1061 = 0x1e`
  (1.5 A), float `0x1070 = 0x50` (4.40 V), inhibit `0x1051` BIT(0) clear;
- MISC (channel 36): `0x1651 = 0x62` (timer enabled on plugin, bark interrupt on),
  `0x1653 = 0x0c` (bark 128 s, and BIT(7) clear: a bite does not disable charging), so the
  unserviced watchdog does not cut charging.

The phone is charging at up to 500 mA into a PC USB port that advertises standard current (SDP),
and the load exceeds it, so the cell falls: the port is the limit, not the charger. On a DCP or a
Type-C port advertising 1.5/3 A the charger would still be capped at 500 mA, because the ICL
register stays 0x0a until software raises it -- the vendor's driver raises ICL on the APSD result
(DCP 1.6 A, CDP 1.5 A) and thyme's DT asks for 3 A. That software step is the next piece of the
port, and verifying it needs a wall charger; on the PC port the right answer is the 500 mA it
already has. So "the kernel's charging path is not written" (docs/install-safe.md) stands for
software control, not for charging itself: the firmware path charges within the port's advertised
limit, which is why a 30-minute soak held 91-92 % on its charger.

For live battery numbers, the gauge is not where it was assumed: PM8150B's fuel gauge is FG_GEN4 at
peripherals 0x4000 (SOC), 0x4100 (battery info), 0x4200 (RRADC) and 0x4300 (memif) -- the type 0x0d
blocks the inventory lists -- while 0x7200 (type 0x16 subtype 0x0a) is the PBS, not a QBG. The next
reading of current and voltage would use FG_GEN4's registers (`qpnp-fg-gen4.c`, `fg-reg.h`).

The software step is now in the driver too.  Boot-arg `pmic_charge=1` dumps the same blocks and then
does what the vendor's driver does with the cable's APSD result (0x1308): SDP 500 mA, CDP 1.5 A,
DCP 1.6 A, written to 0x1370 as the vendor's 50 mA-per-step code, with the high-current sources
also given the vendor's override (0x1366 BIT(0), 0x1365 BIT(4), software ICL override clear); the
watchdog is petted once when a write happens.  Nothing else is touched -- no float voltage, no
fast-charge current.  On this desk's PC port the run logs `pmic: charge: SDP, input current limit
already 500 mA` (`logs/190559-run.log`, kernel `dba84133`): the classification is right and the code
left the port's own limit alone.  The DCP and CDP branches are the vendor's values and wait for a
wall charger to be verified; they cannot be exercised on the port the loader needs.

The thorough part of location is the modem, and the firmware says so itself: the module's
`modemr.jsn` (`fw-modem-sdx55m/`, the phone's own `/vendor/firmware_mnt/image/sdx55m`) declares the
services the modem registers when it boots -- `servreg`, `pdr_enabled` and **`gps_service`**.  The
Wi-Fi fix works without it, but satellite positioning is inside the SDX55M, so the PCIe2/MHI boot
wall (the PBL refusing the SBL after the signature checks, `0xef0e0003` with the BID-to-SID table
programmed; `docs/cellular.md`) is the gate for GNSS and for cellular data alike.  The next work is
on that wall: the BHI doorbell's image type, the image the PBL expects, and what the bootloader does
after the SBL that a bare host must replicate -- researched against mainline's `mhi_pci_generic` and
the SDX55 firmware packaging before the probe is changed again.

### The modem wall is past authentication: the BL rejects the ELF load (2026-11-15, night)

The probe's own control experiments, read against the register evidence, put the wall one stage
further than "the image is refused".  With the untouched vendor `sbl1.mbn` (sha256
`c9027c9b...`, the same file Linux's `mhi_pci_generic` requests for SDX55) the doorbell answers
`0xef0e0003`; zeroing the 548,056-byte image in place (same address and size) answers
`0xef120700`; changing one body byte at file offset 0x8000 answers `0xef0a0105` -- the
authentication group, the same family a QCA6490 PBL uses for a body-hash mismatch.  So the PBL
does read the buffer, authenticate the image, and refuse a modified one: the untouched image
fails after authentication, in the ELF program-header/load group (0x0e), not for its signature.
The BHI identity registers agree with a genuine part (`serial 0x9721197d`, antirollback 0,
numseg 3, OEM PK hash fused), and the BHI doorbell is only a nonzero sequence token -- there is
no image ID in it and mainline writes a random one.

The SMMU side is verified in the same run (`logs/195239-run.log`, kernel `a83a8a0f`): the
BDF-to-SID table reads back `0x1000100` in all 256 entries (BDF 1:0.0 -> SID 0x1d01), SMR 6
carries stream `0x1d01` into context 5, and a walk of context 5's tables for 0x20000000 prints
`page 0xc0000e43 -> physical 0xc0000000`, the image buffer.  The stale context FARs (the SMMU is
not reset between boots) do not belong to this transfer.  What is left is the SBL's ELF load:
its segments' p_paddrs are 0x14682000..0x148e7000 and 0x8fd00000/0x8fd3c000, and the phone also
ships `mdmddr.mbn` (40 bytes) beside the firmware -- whether the PBL needs the modem's DDR
configured or those destination ranges mapped before it can place the segments is the question
the next research pass answers.

One more state is ruled out: the row does not appear when the location subsystem is live either.
`rc.jobs.locrow5` opens LocProbe first, answers its alert, and only then opens Privacy & Security;
the status bar shows Settings was opened from LocProbe, and the page -- scrolled to the top on
purpose -- still begins with Tracking and Contacts (`logs/sileo-160.png` of the 20:09 run).  The
deep link's scroll landing at Local Network in that run is a scroll position, not a row: a swipe
back to the top shows the same page as always.  So the filter is inside Preferences' own specifier
handling (the `locationServicesSpecifier` path) and does not depend on the switch, an authorized
client, the framework's classes or the subsystem's state.  Location itself does not need the row:
the switch is on at boot and applications get their Wi-Fi fixes.

### The modem's BHI door is authenticated and then refused at the ELF load (2026-11-15, late night)

The stock flow, established from a public Sony Xperia SDX55M crash/reload log, is hybrid: the
kernel's MHI/BHI loads **only `sbl1.mbn`**; the SBL then exposes MHI UCI pipes, and userspace
(`mdm_helper`/kickstart) serves Sahara images over them -- **mdmddr (Sahara ID 34) first**, then
apdp, devcfg, tz, hyp, sec, aop, the EFS images, acdb, qdsp6sw and apps.  The `mdmddr.mbn` in
`/vendor/firmware_mnt/image/sdx55m` is a 40-byte header-only stub (image ID 0x22, no payload); the
real training/configuration data is the `mdmddr` partition (1 MB; stock transfers 1,636 bytes of
it).  The stock SDX55M power-on is regulators, an SPMI warm reset, 150 ms, AP2MDM_STATUS high and
the PBLRDY poll -- the probe's default path skips the warm reset, and `pcie2_bhi=2` performs it.

Against that, the experiments on this phone now read as one line: the host side is fine and the
PBL refuses the ELF load.  The source mapping is proven (context 5 maps IOVA 0x20000000 to the
image's physical 0xc0000000, stream 0x1d01 through SMR 6), the untouched vendor image
authenticates (a one-byte body change turns the answer into the authentication group's
`0xef0a0105`, a zeroed image into `0xef120700`), and with the PMIC warm reset performed
(`pcie2_bhi=2` with `pcie2_sid=5`, `logs/202334-run.log`) the answer is the same `0xef0e0003` in
30 ms.  By the public QCA6490 PBL's grouping, group 0x0e is the ELF program-header/loader class:
the PBL reads and authenticates the image, then fails to place its segments.  Those destinations
are modem-internal -- SRAM at 0x14682000..0x148e7000 and the SBL's DDR (`sbl_mem`,
0x8fd00000, the one PT_LOAD segment whose `p_memsz > p_filesz`) -- not host addresses, so they
cannot be fixed from the host SMMU; the only DDR load segment is that BSS.

So the wall for GNSS is exactly where `docs/cellular.md` left the cellular wall: before any
MHI/Sahara step the stock flow has, the PBL will not complete the SBL placement on this host.
The remaining hypotheses are device-state ones (the regulators/PON sequence the stock ESOC driver
performs, the DDR state the PBL expects, an image variant/platform check) and a known-good BHI
load of the same SBL on the same phone; the public sources do not settle them.  The full stock
path after the SBL (MHI UCI + Sahara + mdmddr) is a larger bring-up of its own.  Until that door
opens, satellite positioning stays unavailable and the working provider is Wi-Fi, as verified
above; the Settings row stays the separate open item recorded earlier.

The last host-side lever was checked and is not there.  The thyme/LineageOS stock `esoc-mdm-pon`
driver has **no regulator handling at all**: its modem node in the public thyme DTS carries only
the AP2MDM/MDM2AP GPIOs (`ap2mdm-status` 56, `mdm2ap-status` 3, errfatal 57/1), no `reg-names`, no
supplies; the regulator loop that appears in some trees is a different SoC's driver.  The modem's
own power (its DDR/SRAM) comes from a separate modem PMIC (`pmxprairie`) on the modem's SPMI at
SID 8, which the apps arbiter cannot reach -- the port's own inventory already lists slave 8 as
"failed" (no chip on this bus).  The stock PON sequence is exactly what the probe's `pcie2_bhi=2`
does -- SPMI warm reset, 150 ms, AP2MDM_STATUS high, then PBLRDY or an image request -- and the
stock apps-SMMU mapping is the one the probe programs (`iommu-map` BDF 0x100 -> SID 0x1d01;
`pcie2_sid=5` is the probe's *mode*, not a SID value).  So no AP-side regulator, reset or SID
change remains to try: with the image read and authenticated and the ELF load refused, the door is
on the modem's side of the PCIe link, where this host has no interface.  Satellite positioning
stays unavailable; Wi-Fi positioning is the working provider and the Settings row stays the
separate open item.

The modem PMIC is reachable after all, and the reset now goes where stock's does.  The live phone's
own device tree (`/sys/firmware/fdt`, pulled with `adb`, decompiled) has `qcom,pmxprairie@8` on the
apps SPMI bus with a single child -- `qcom,power-on@800` marked `qcom,modem-reset` -- and no
regulators and no supplies on `qcom,mdm0`; Android enumerates `spmi0-08` and its MHI channels
(`0306_02.01.00_*`, SAHARA among them) are online, so under stock the modem is fully booted.  The
port's earlier inventory line that made slave 8 look dead did not apply to this block: with
`pmic_modem_pon_sid=8` the reset finds and writes it (`pmic: the modem's PON: slave 8 0x0800
(channel 9), reset type 1`, `logs/205703-run.log`), and `pmic_modem_pon` selects the vendor's
reset type (1 warm, 2 shutdown, 3 hard).  With the stock target for the reset now used, the BHI
answer is unchanged (`0xef0e0003`, 30 ms) -- so the reset block was not the difference either.

What the same DT rules out: no AP-side regulator property on the modem PMIC or the modem node, so
there is no AP rail to add.  The remaining difference between this boot chain and stock is the
boot chain itself: Silicium's UEFI (the umi package in `Mu-Silicium`, which this phone's loader
build uses) contains no modem/PCIe-modem driver at all, while stock's XBL/ABL is what leaves the
modem running with its MHI channels up before Linux.  The exact 4.19 driver's BHI path is the next
thing to read against the probe, to learn whether the stock kernel's load differs in a detail
(MHICFG/INTVEC/EE handling) or whether only the bootloader can do it here.

The decisive comparison is in, and it moves the wall from the driver to the boot path.  The
LineageOS 4.19 driver that runs on this phone (commit `6b34d247`, `drivers/bus/mhi/core/
mhi_boot.c`, `mhi_pm.c`, `mhi_qcom.c`) does exactly what the probe does before the SBL transfer:
`BHI_STATUS = 0`, image address high/low, size, a random nonzero 30-bit doorbell -- no MHICFG, no
event-ring counts, no M0 (those come after READY) -- and `BHI_INTVEC = 0` exactly as the probe
writes it.  Its `mhi_async_power_up()` reads `BHI_EXECENV` and, when the EE is AMSS, deliberately
skips the firmware request and goes straight to READY.  Android's own boot has no `sbl1.mbn`
firmware request (ueventd logs every request, and the ipa images appear there), so under stock the
modem is already in AMSS when Linux starts: the boot chain booted it, not the kernel.

And in our path the endpoint is in the PBL before any reset of ours.  With `pcie2_keep=1` the probe
leaves PERST alone (`logs/211817-run.log`: `pcie2: PERST left alone (pcie2_keep=1)`, link up at
`0xb0230008`), and the BHI still answers `execenv 0` before and after the link comes up.  So the
Silicium/xnuloader path (its `umi` UEFI package in `Mu-Silicium` has no modem or PCIe-modem driver)
never starts the modem, while the path that boots Android does -- that is the structural difference,
not a register the probe is missing.  Every host-visible input has now been matched to stock: the
PON at pmxprairie slave 8, warm/hard reset types, AP2MDM_STATUS and the 150 ms pauses, the
BHI sequence, the doorbell, the 32-bit/IOVA source, the SMMU mapping, and the image itself.

What remains for satellite positioning is therefore not a kernel fix under this boot chain: it would
need the modem to be started by the path that can (the stock boot chain, as it does for Android), or
the PBL's post-authentication ELF load to be solved from the device side.  Neither is reachable from
the present sources.  Wi-Fi positioning stays the working provider, and this is the boundary the
checklist records.

The previous section's reading of Android is corrected by its own kernel log.  Android's dmesg
carries, at 13.26 s of its boot:

    mhi 0002:01:00.0: Falling back to syfs fallback for: sdx55m/sbl1.mbn

-- the firmware loader's message when the MHI driver's `request_firmware("sdx55m/sbl1.mbn")`
went to the sysfs fallback (which is why no ueventd line appeared).  The 4.19 driver only asks
for that file when `BHI_EXECENV` is PBL, so **on this phone the Android kernel does load the
SBL over BHI** and it succeeds; the modem is in AMSS a second later and the MHI channels come
up.  A working kernel-side reference exists on exactly this hardware, and the difference between
it and the probe is findable.

The first difference found is an ordering one.  In the stock flow the PCIe link is initialized
first (the controller driver asserts PERST as part of its enable), the ESOC power-on then does
the PMIC warm reset through the modem's PON (pmxprairie, slave 8), and the MHI driver loads the
SBL right after -- no PERST between the reset and the doorbell.  The probe's `pcie2_bhi=2` did
the PMIC reset and then asserted PERST inside its own link training, i.e. reset the modem again
after its warm reset.  `pcie2_keep=1` skips the PERST but also, by its condition, the PMIC reset
(one run showed the PON line missing: `logs/212852-run.log`).  `pcie2_perst=0` now separates the
two: the PMIC reset still runs, PERST does not, and the doorbell follows the reset directly --
the stock order.  The next run is `pcie2_bhi=2 pcie2_perst=0 pcie2_sid=5
pmic_modem_pon_sid=8`.

The stock order itself is now tested and it is not the difference.  `pcie2_pon_late=1` with
`pcie2_bhi=2 pcie2_sid=5 pmic_modem_pon_sid=8` brings the link up first (PERST included, as the
controller driver does), then resets the modem through pmxprairie's PON, raises AP2MDM_STATUS and
rings the doorbell ten milliseconds later -- the 4.19/ESOC order -- and the answer is the same
`0xef0e0003` in 30 ms (`logs/215121-run.log`, kernel `a8474aa8`).  The link parameters match the
stock ones too: Android reports 2.5 GT/s x2, 64-bit DMA and an IOMMU DMA group on 0002:01:00.0,
and the probe's link status is the same width/speed class.

So every AP-visible input of the SBL transfer is now identical to the kernel that succeeds on this
phone: image and size, IOVA 0x20000000 in a verified stage-1 context with SID 0x1d01, the BHI
register sequence and zero boot vector, the random-nonzero doorbell, the PMIC PON reset at the
modem's own block, the AP2MDM_STATUS handshake, the link, and the order of all of them.  The PBL
still reads and authenticates the image and then fails to place its ELF segments.  What is left is
not visible from the apps processor -- the modem-side secure state the PBL checks at that stage --
and closing it needs either the stock bootloader's modem bring-up (whose code is not public) or
device-side reverse engineering of the PBL.  The Wi-Fi provider remains the working location, and
this is the boundary this checklist records for GPS.
### The stock kernel's own modem boot, read and captured (2026-11-15, late night)

The phone's Android is HyperOS 3 (`OS3.0.10.0.WPACNXM`, Android 16, kernel
`4.19.157-HyperOS3-A16-OurSky-Developers+`), not the OS1.0.4.0 package the earlier
research had used.  Its `boot_a` (`/dev/block/sde12`) kernel Image was pulled (48,781,336
bytes) and `vmlinux-to-elf` rebuilt a symbolized ELF from the in-kernel kallsyms, so the
stock ESOC/PON/MHI code could be read as code rather than inferred:

- the ops tables name the path: `mdm4x_do_first_power_on` -> `sdx55m_toggle_soft_reset`
  -> `qpnp_pon_modem_pwr_off(1)`;
- `qpnp_pon_modem_pwr_off` writes `(base + 0x63) = 0`, `udelay(500)`,
  `(base + 0x62) = type`, `(base + 0x63) = 0x80`, `udelay(500)`, `(base + 0x64) = 0xA5`;
  `base` is the first u32 of the power-on node's `reg` (0x800);
- the PON it uses is `modem_reset_dev`, assigned to the node with **`qcom,modem-reset`**
  (pmxprairie@8); `qcom,system-reset` belongs to a different PON (pm8150@0) and is not
  this path;
- `mdm4x_do_first_power_on`: the reset above, `msleep(150)`, AP2MDM_STATUS high, poll the
  optional pblrdy GPIO (the live DT has none), `msleep(200)` or queue `ESOC_REQ_IMG`.

The port had written only the first two values, so its pulse was loaded but never fired.
`hakios_pmic_pon_modem_reset` now writes all five and reads 0x862/0x863 back before the
trigger -- the phone answers `0x863 = 0x80`, so the writes land; `hakios.thyme_pcie2.c`
drives AP2MDM_STATUS low before the pulse, raises it after the vendor's 150 ms, watches
MDM2AP_STATUS for up to 100 ms and waits another 200 ms before touching the link.  The
doorbell's answer did not change (`0xef0e0003` in 30 ms, `logs/225051-run.log`).

The stock reset path itself is real, and was watched live.  A small static tool (raw
syscalls, no libc) drove `/dev/esoc-0`: `ESOC_NOTIFY` with `ESOC_PRIMARY_REBOOT` made the
driver call `qpnp_pon_modem_pwr_off(type=1)` (kprobe `p:pon
qpnp_pon_modem_pwr_off type=%x0`, event at 1112.361 s) and the ESOC status fell from
0x1 to 0x0 -- the modem was actually powered down by the same sequence.  The modem does
not come back from that notification alone; the next Android boot restores it.

A kprobe capture of a stock Android boot (`mhi_write_reg` offset/value pairs and
`mhi_fw_load_handler`) then gave the successful SBL load exactly, at 13.2548 s of that
boot:

    mwr off=0x2c val=0x0          BHI_STATUS
    mwr off=0xc  val=0x0          BHI_IMGADDR_HIGH
    mwr off=0x8  val=0x20100000   BHI_IMGADDR_LOW
    mwr off=0x10 val=0x85cd8      BHI_IMGSIZE
    mwr off=0x18 val=0x3dbe2bc7   BHI_IMGTXDB (the session id)
    flh: mhi_fw_load_handler +0x0  (the entry, at the same instant)

and at 13.5266 s the MHI context and window writes: CCABAP 0x20002000, ECABAP
0x20004000, CRCBAP 0x20048000, MHICTRLBASE/LIMIT 0x0/0x3fffffff, MHIDATABASE/LIMIT
0x0/0x3fffffff.  The image is the DMA pool's base + 1 MB, above the ring area, while
every probe so far had put it at the pool base.  `pcie2_iova` now moves the probe's image
(0x20100000 tried), and `pcie2_win_base`/`pcie2_win_stop` reproduce the stock windows.
With the image at 0x20100000 the answer is unchanged (`logs/231953-run.log`, `dbg
0x20100000 0 0x85cd8`); the stock-window run is recorded below when it lands.

The file-variant hypothesis is closed as well: the phone's installed
`/vendor/firmware_mnt/image/sdx55m/sbl1.mbn` hashes `c9027c9b...e1de5f` (548,056 bytes),
exactly the file the probe hands the PBL.  The stock kernel requests that same file into a
coherent buffer and writes the same five BHI registers the probe does, with the same size.

One correction: `0xb0230008`'s bits 23:16 are 0x23 -- speed 3 and width 2 -- so the port's
link is Gen3 x2, the same class as Android's "8 GT/s x2"; the earlier note's "2.5 GT/s x2"
read an intermediate training value.

So every AP-visible input now matches the stock success path: the image and its size, the
BHI register sequence and the image's IOVA, the SMMU context and stream, the PMIC pulse
and its readback, the AP2MDM/MDM2AP handshake and pauses, the PHY sequence (identical to
the live DT's `qcom,phy-sequence`), and the link.  What remains is the modem's internal
state at the moment the PBL loads the ELF, which the AP cannot see or write.  One stock
step is knowingly not reproduced: `mhi_async_power_up` initializes the MHI context rings
and MSI before the BHI load (`mhi_init_dev_ctxt`, `mhi_init_irq_setup`); those structures
are host-side rings the PBL transfer does not read, and the probe's BHI writes match the
capture without them.

The stock-window run landed and repeats the same answer.  `pcie2_win_base=0
pcie2_win_stop=0x3fffffff` with `pcie2_iova=0x20100000` gives
`logs/232541-run.log` (SHA-256 `d83adecd...8af79`): the probe prints `MHI: windows
base 0 stop 0x3fffff00` (the boot line carried the value with its low byte zeroed, a
command-line parsing artifact; the image at 0x20100000 is below either limit) and
`BHI: status 0xffffffff (result 3), errcode 0xef0e0003, dbg 0x20100000 0 0x85cd8,
execenv 0, MHISTATUS 0 after 30 ms`.  So neither the image's place in the pool nor the
MHI window pair is the difference.

Two corrections and the last AP-side variable complete this pass.  The 200 ms tail in
`mdm4x_do_first_power_on` belongs to the pblrdy branch; this phone's DT has no pblrdy
line, so stock queues the image request right after AP2MDM_STATUS and the
reset-to-doorbell gap is about 250 ms.  The port had added the status poll and the
200 ms unconditionally; both are removed (a 10 ms settle only), and AP2MDM_ERRFATAL is
now driven low before the pulse as the vendor's cmd-exe does.  With the full pulse, the
stock image IOVA and this stock-short timing the answer is still `0xef0e0003`
(`logs/233453-run.log`, SHA-256 `f7117a58...45024b`).

The image's physical placement is closed too.  With `pcie2_img=2` the file sits in
ordinary contiguous kernel memory (physical 0x1970d8000, above 4 GB) instead of the
SLPI heap at 0xc0000000; with the full pulse, the stock-short timing and
`pcie2_iova=0x20100000` the answer is unchanged (`logs/234027-run.log`, SHA-256
`73b3fd73...97add`).  So the carveout is not the difference either.

### Device-side reverse engineering of the modem wall (2026-11-15, night)

With every AP-visible input matched, the work moved to the device side.  The phone's own
boot chain was pulled read-only (it has root): `xbl_a` (`/dev/block/sdb1`, 8 MB, ELF64
aarch64, SHA-256 `b04fc2ca...`), `abl_a` (`/dev/block/sde9`, 2 MB, ELF32 ARM wrapping a
UEFI FV at offset 0x3028, SHA-256 `3752087d...`).  XBL contains the AP's DDR training
(`CD_BTDDR.BIN`, "DDR Training Init/Restore") and the SPMI/PCIe2 drivers (including the
`PCIE_MODEM_*` register blocks); ABL's UEFI volume carries no readable strings.  XBL runs
in both the Android boot and a `fastboot boot` of the Silicium loader, so the boot chain
is common up to ABL's boot-mode branch; neither image contains a modem firmware loader
(only `has-slot:modem` strings).  The modem PMIC's regulators are not in the live DT (its
PON is the only pmxprairie child), so the AP has no visible modem rail to cut.

Public device-side research found no SDX55 PBL source, error table or decode, but two
independent Qualcomm PBL analyses establish the error word: `0xEF | group | status |
detail`, with the group masked at bits 23:16 and status at 15:8 (the QCA6490/WCN6855
work in `derekross/omarchy-mobile` `device/lynx/WIFI-PLAN.md`, and the Nokia 6 PBL
disassembly in `alephsecurity/firehorse` `host/target/nokia6/pbl-nokia6-bbdb.txt`).  In
that format `0xef0e0003` is group 0x0e -- the post-authentication ELF-loader group --
with status 0x00/detail 0x03; `0xef0a0105` is the authentication/hash group (0x0a) and
`0xef120700` the ELF/hash-read group (0x12).  The SBL's ELF flag high bits decode with
Qualcomm's `MI_PBT` definitions (`coreboot util/qualcomm/mbn_tools.py`) as AMSS
(`0x01000007`) and SWAPPED/XBL_SEC (`0x05000005`) segments, and the hash segment matches
U-Boot `tools/qcom/mkmbn/hashseg.py` exactly: 48-byte header, 120-byte metadata, 17
SHA-384 hashes, 256-byte signature, 6,144-byte certificate chain -- 7,384 bytes ending
exactly at the file's 0x85cd8.  The only DDR-resident PT_LOAD is the BSS at 0x8fd00000
(filesz 0, memsz 0x3b1ec); the hash segment's `p_paddr` is 0x8fd3c000.

No public source shows `mdmddr` or any DDR training being provided over BHI before SBL:
the documented order is userspace Sahara image 34 *after* the SBL runs (the MHI UCI
SAHARA channel exists only in SBL EE), and `mdmddr.mbn` is a 40-byte unsigned header, not
an ELF hash-segment image.  No public report of `0xef0e0003`, nor of a host-only BHI load
failing at the ELF stage for DDR/PMIC/secure-boot reasons, was found; the closest is the
QCA6490 PBL parking after authentication for chip-side boot/security state, without a
fix.  The phone's installed `sbl1.mbn` is the same `c9027c9b...` file, so a
firmware-variant difference is ruled out.  The leading device-side hypothesis is the
modem's DDR/protection state at a cold host-only BHI load: the stock kernel's successful
load may happen with the modem's DDR already up, and the decisive test -- making the
stock kernel load from a genuinely cold modem -- needs a modem power cut the AP cannot
perform (no AP-visible regulators; a physical power cycle would do it).  An EDL trigger
exists in mainline MHI (`MHI_EDL_DB 91`, cookie `0xEDEDEDED`), but there is no public
host tool for Sahara over the MHI UCI pipe to use it.

The last AP-side interface hypothesis was stock's whole-pool IOMMU mapping: if the PBL's
ELF stage prefetches or rounds past the 548,056-byte image, the probe's image-only
mapping would fault.  The SMMU's own fault registers argue against it -- after each
doorbell the global FSR is 0 and the context FSR is the stale 0x400/FAR pair, not a new
translation fault for the image window -- and an attempt to map 8 MB of the SLPI heap
behind the image hung the kernel before the image line: `logs/234912-run.log.live` ends
at `pcie2: BHI: the SMMU maps 0x800000 bytes from the image on` and the loader's watchdog
returned the phone to Android.  The experiment and its boot argument were reverted; the
image-only mapping stands as the last tested state.

### The cold-boot control: stock loads the SBL from a cold modem (2026-11-16, after midnight)

The decisive control the device-side research asked for was run: the phone was powered off
completely (long power press, a user-requested shutdown -- `ro.boot.bootreason` reads
`shutdown,userrequested`) and powered on into Android after a pause.  On that cold boot the
stock kernel did exactly what it does on a warm one:

    13.025674 subsys-restart: Changing subsys fw_name to esoc0
    13.242583 mhi 0002:01:00.0: enabling device (0000 -> 0002)
    13.244076 mhi 0002:01:00.0: Falling back to syfs fallback for: sdx55m/sbl1.mbn
    15.400365 mhi 0002:01:00.0: enabling device (0000 -> 0002)
    15.516130 sysmon-qmi: ssctl_new_server: Connection established ... esoc0's SSCTL service

(`coldboot-dmesg.txt`, 478.8 KB; uptime 85 s when captured, so this is the cold boot
itself).  A cold modem's PBL places the stock SBL over BHI just as well as a warm one, and
the MHI channels come up.  So the cold-DDR state hypothesis is closed: the wall is not
"the PBL needs pre-trained DDR that only a previous SBL run provides".  What is left is the
difference between the stock boot path and the port's: the stock path is
XBL -> ABL normal boot -> kernel, while HakiOS is XBL -> ABL fastboot -> Silicium UEFI
(from RAM) -> xnuloader -> kernel, and every AP-visible register/file input in the transfer
has already been matched.  The next control is a cold power-on *into the bootloader*
(volume-down + power), then the same BHI probe without letting Android run first: if that
load also succeeds, the boot-chain state after ABL's normal boot is what the PBL is
reacting to; if it fails like every other HakiOS probe, the remaining difference is inside
the port's own host setup despite the register-level matches.

Two controls closed the last device-side hypotheses.  The cold-boot control: after a full
power-off and power-on (`ro.boot.bootreason` `shutdown,userrequested`; uptime 85 s when
captured) the stock kernel still loads `sdx55m/sbl1.mbn` over BHI at 13.244 s and MHI is up
at 15.4 s (`coldboot-dmesg.txt`) -- a cold modem's PBL places the stock SBL, so the
cold-DDR/protection-state hypothesis is closed.  The RC-config control: the stock
`pci-msm` pieces the port never wrote -- DBI 0xb40 = 0x14 for the 19.2 MHz aux clock
(`qcom,use-19p2mhz-aux-clk`), ACK_F_ASPM_CTRL 0x70c bit 15 for the absent `qcom,n-fts`,
BRIDGE_CTRL 0x3c bits 16/17 and the device control's four reporting enables for AER -- are
now in `hakios.thyme_pcie2.c`; the phone reads them back (`aux 0x14, ack_f 0x1b909000,
bridge 0x300ff, devctl 0x10581f`) and the doorbell still answers `0xef0e0003` in 30 ms
with the same AER view (`logs/004339-run.log`).  A cold-fastboot control -- power off,
volume-down + power into the bootloader, then the same probe with Android never running --
also answers `0xef0e0003` (`logs/003452-run.log`).  So neither the modem's cold state nor
the last stock RC-configuration item is the wall.

The step this section used to list as "still not reproduced" -- the MHI context and interrupt
setup that `mhi_async_power_up` performs -- turned out not to be a pre-load step at all
(2026-11-20).  In the stock kernel the only writer of the channel/event/command context bases
(0x58-0x6c), the MHI windows (0x80-0x8c, 0x98-0xa4) and MHICFG is `mhi_init_mmio`
(0xffffff80105edbd8), and its only caller is `mhi_ready_state_transition`
(0xffffff80105f6f6c, call at 0x5f714c) -- the path that runs when the device reports MHI
READY, i.e. only after the PBL has accepted the firmware.  The capture agrees: the doorbell
lands at 13.2548 s and the context/window writes at 13.5266 s.  `mhi_init_dev_ctxt`
(0x5ecfc8, called from `mhi_prepare_for_power_up` 0x5ef8a8 and `mhi_async_power_up`
0x5f8f80) allocates only the host rings (`kmem_cache_alloc_trace`,
`dma_alloc_from_dev_coherent`) and writes no device register, so stock's whole pre-doorbell
register state is the five BHI writes the probe already sends (0x2c, 0xc, 0x8, 0x10, 0x18).
The control closes it: `pcie2_mhiraw=1` with the full probe in the verified environment (the
resident system.apfs is the root, the payload root.apfs lands on /redwood with
fw/modem/sbl1.mbn) leaves the context bases and windows untouched, the SMMU context 5 maps
the image IOVA 0x20100000 to the SLPI heap at 0xc0000000, and the doorbell answers the same
verdict in the same 30 ms -- `BHI: status 0xffffffff (result 3), errcode 0xef0e0003, dbg
0x20100000 0 0x85cd8, execenv 0, MHISTATUS 0 after 30 ms` (`logs/171914-run.log`).  Two
earlier attempts in the same session are void and no evidence: one carried a cmdline
truncated after the loader's own prefix, so no `pcie2_*` argument arrived
(`logs/170604-run.log`), the other used the payload as the root, which lacks /sbin/launchd
and panics at exec before the probe (`logs/171349-run.log`).  MSI enable
(`mhi_init_irq_setup`, the one other thing `mhi_async_power_up` does before queueing the
state transition) is the only remaining config-space delta and cannot matter to this path:
the BHI transaction polls BHI_STATUS and never uses an MSI.  The boot-chain isolation that
would test "ABL normal boot vs fastboot + Silicium" cannot be done without flashing a
modified boot image, which needs explicit authorization.  The Wi-Fi location provider
remains the working location, as recorded earlier.

### Weather gets a location (2026-11-16, user-observed)

The locationd preference is now seeded into every image instead of written by a boot
job: `userland/var/mobile/Library/Preferences/com.apple.locationd.plist`
(`LocationServicesEnabledIn8.0` = true, mode 644, owner 501) is copied into
`/private/var` by init from `etc/var.skel` before launchd starts, so locationd and the
first application both see the switch on.  On a RAM boot with Wi-Fi up the user opened
Weather, answered CoreLocation's consent alert, and Weather showed the current place --
Xi'an, Chang'an District.  That is the Wi-Fi positioning path end to end in a stock
application, not only in LocProbe: the same locationd, the same ALS provider, and the
consent prompt.  The satellite path remains unavailable (the modem wall above).

The Settings row work continues separately.  The plist's LOCATION specifier is filtered
by Preferences whatever its detail class, getter, label or icon says, so the fix is a
dylib injected into Preferences (`userland/src/inject_prefs.c`,
`libhakios.prefs.dylib`): it hooks PSListController's own `viewDidLoad` and
`viewWillAppear:` (PSListController is always present; the privacy framework is dlopened
later) and, when the instance is `PUIPrivacyController`, inserts a PSLinkListCell for
`PUILocationServicesListController` at the top of the page with the identifier
`LOCATION`.  The library and its INJECT line are in the image; the automated scenes that
tried to open Settings with it did not come up (the prefs: URL as a first launch is
dropped here and the screenshots/logs of those runs were lost to the screenshot/log
staging collision), so its state is "code in the image, real-device verification still
open".  The probe rows added to Privacy.plist for the earlier filter experiments are
removed from the sysimage source again.

### The wedged submission's own stream: a GMEM tile's store blit (2026-11-17)

The command processor's report names the submission and the words it has left in
the indirect buffer, but not the words themselves: by the time the next submission
arrives, the window server has written its next frame into the same buffer
(submission 41's 7964 words at 216a0000 over 40's).  The user client now copies the
first command buffer (up to 128 KB) after a submission is accepted, and when a
submission two seconds late is still the one the core is in, it prints the words
around the core's position, the waits' words with their values, and the shader
programs the stream last named (`fbib`, `fbstop`, `fbwait`, `fbvs`, `fbfs`; kernel
`c910c2ccc939`, scene `a0a3c70130d1`, `logs/052522-run.log`).

Submission 40: 11483 words at 216a0000, the core at word 4064 of them (7419 to
go), the second indirect buffer at 23990000 with 0 to go.  Decoded, word 4064 is
inside the GMEM tile loop of the window server's frame (tiled rendering is on by
default, `gmem_ = true`): each tile emits a load blit (RB_BLIT_DST_INFO to the
attachment at 200a0000, BLIT event 30), the tile's draws as an IB2 at 23990000
(9248 words), RM6_BIN_END_OF_DRAWS, `CP_SKIP_IB2_ENABLE_GLOBAL(0)`, RM6_BIN_RESOLVE,
and the store blits (BLIT event 30 to 200a0000, then to 22140000 with
BASE_GMEM 0x74000), then a WFI.  The core stopped at the RB_BLIT_INFO register
write of the tile's first store blit (word 4063), with the IB2 behind it fully
consumed: the command processor had issued the tile's draws and stalled in the
register writes that follow them, which is what a draw that does not drain looks
like from the command processor's side rather than a wait packet.  No
CP_WAIT_REG_MEM comes within 96 words of the stop, so the framebuffer-read flush
of the earlier run was not the stop here.

Two lessons for the next run: the fbstage file was probably written all along --
the scene's rc.jobs invoked `/bin/cat`, which this root does not have (every cat
exited 127, "No such file or directory"), and cat lives at /var/jb/usr/bin/cat
(the Sileo bootstrap); and the hung draw's shader is not in the outer stream but
in the tile's IB2, so the trap now reads SP_VS_OBJ_START and SP_FS_OBJ_START from
the GPU's own registers and prints the first words at each (`fbprog`, `fbvs`,
`fbfs`) -- to be matched against the fbstage lines once they are read with the
right cat.

The next run (scene `metal650o`) proved the cat right and the register read
wrong: the fbstage file was read and holds the stage words, but the live read of
SP_VS_OBJ_START at the trap stopped the machine -- `xl run` ended with "the
kernel hung", the trap's `fbib` and 14 `fbstop` lines in
`logs/053718-run.log.live` and no `fbprog` after them.  A wedged 3D block does
not answer its register bus, so an MMIO read of its registers stalls the CPU.
The trap no longer reads any SP register: at submit time it copies the first
CP_INDIRECT_BUFFER the submission runs (the pass's draws: `0x2393e000`, `0x1e6f`
words in this run) and the first eight words at each `SP_VS_OBJ_START` /
`SP_FS_OBJ_START` program that body names, and prints those copies
(`fbbody`, `fbprog`).  The fbstage label was garbage there (`yAcr!!`): `[vs
name]` no longer answers once the AIR has been taken, so the line now carries
`hakios_metal_program_name(program)`, the same label as the `linked` line.  The
stop itself is reproducible: submission 40, word 4064 of 11489, the GMEM store
blit of a tile, the IB2 behind it consumed.

With the body kept (`logs/054757-run.log`, scene `metal650q`, kernel
`82abf2539d68`) the trap names the pass's draws: `fbbody: 7575 words at
0x2396e000`, and the first eight words at each of the four (VS, FS) program
pairs it runs, matched against the 315 `fbstage` rows of the same run (labels
now `hakios_metal_program_name`):

- 0x22100000 (VS `00001000 4070000a ...`) / 0x22110000 (FS `00000007 03820000
  ...`): `fixed_vert_lph_spc` + `fixed_frag_lph_cpf`/`_cph` (the VS head is
  shared with `std_vert1_lph`'s luma_boost/home_affordance pipelines, the FS
  head by cpf and cph);
- 0x237a0000 / 0x237b0000 (FS `00002004 4730090a ...`): cpf/cph again;
- 0x21670000 / 0x21680000 (FS `00002000 47308902 ... a1000f0a`): the cph
  variant only;
- 0x21a80000 / 0x21a90000: cpf/cph again.

So the wedged pass is CoreAnimation's own fixed-function compositor; the first
eight words do not separate the cpf and cph variants except at 0x21680000 (cph),
and the body is replayed once per bin, so any one of its draws hangs the tile
loop.  What that leaves open is where the hang is: the GMEM tile machinery (a
store blit or the bin state) or one of these draws.  The next run answers it by
turning the plug-in's own GMEM switch off (`metal_gmem_draws=0`, which the
`hm_gmem_draws() != 0` clause otherwise leaves at its default 56 and thus on).

### GMEM off: the window server stops wedging (2026-11-17)

The same scene and kernel (`metal650q`, `82abf2539d68`) with `metal_gmem_draws=0`
in the command line (`logs/055707-run.log`): no "no answer", no refused
submission, every commit completes (0.4 ms on average, 21.2 at most), 23 command
buffers every two seconds, and the framebuffer swaps 2280 times -- the early
swaps black, the late ones `black 3/2046` of the sampled pixels, a drawn frame.
With GMEM at the plug-in's default the same image wedged in submission 40's tile
loop every time (054757 and 055707 are the two sides).  So the hang is the GMEM
path's tile loop of CoreAnimation's fixed-function draws, not a draw that sysmem
cannot run.

The plug-in now turns GMEM on only with `metal_gmem=1`: the condition was
`hm_gmem_on() || hm_gmem_draws() != 0`, and `hm_gmem_draws()` is 56 when the
boot-arg does not name it, so tiled rendering was on by default -- against the
comment above `hm_gmem_on()` ("Off by default: for CoreAnimation it won
nothing").  The parallel render encoder's sub-buffers, which start from the
library's GMEM-on defaults and never took the plug-in's choice, now get the same
settings.  The GMEM path itself (a tile's store blit stalling the command
processor after the IB2 is consumed) remains to be fixed for the performance it
was meant to bring; it is not on the daily path any more.

### The home screen through Metal, no wedge (2026-11-17, screenshot)

The fix built into a scene and run with the default arguments (`metal650r`,
kernel `82abf2539d68`, `logs/060526-run.log`): not one `no answer` line, the
framebuffer's swaps reach 2400 with `black 3/2046` of the sampled pixels, and two
screenshots taken during the run (`logs/metal650r-1.png`, `-2.png`) show the whole
home screen -- the wallpaper, the status bar at 6:06 with 64% battery, the Weather
(`No weather data`) and Calendar (`TUESDAY 17`) widgets, every icon and the dock.
The plug-in's fps line still says 0 presented because it counts `presentDrawable`
calls; the display's swap log and the screenshots are the evidence.  This is the
first stable full UI with the Metal window server, and Maps takes the same path.

### Maps: the app opens, its renderer runs; the map view is the next wall (2026-11-17)

With `metal_all=1` first in the command line and the Maps scene (`rc.jobs.maps`,
`/var/jb/usr/bin/uiopen --bundleid com.apple.Maps`), Maps runs on the Metal
window server: its first launch draws "What's New in Maps" (`logs/maps-1.png`),
`rwtouch`'s taps at (0.5, 0.79) dismiss it, the notification screen follows
("Not Now" at (0.5, 0.918)), and the app reaches its search, favorites and guides
screen with the location alert up (`logs/maps6-1.png`; scene `metal650maps6`,
kernel `82abf2539d68`).  No GPU wedge in any run (`no answer` 0).

What it asked the GPU for, and what it got:

- a 1125x2436 4-sample Depth32Float_Stencil8 render target (usage 4): refused 16
  times in the first run (`logs/061230-run.log`); the runtime now lays it out as
  its depth plane alone, and a pass that binds the stencil plane of a multisample
  depth texture is refused with its own sentence (`ffad6b7b`).  The refusals are
  gone and Maps' renderer proceeds;
- its VectorKit pipeline library opens through the mirror with an empty cache for
  `pipelines.Adreno(hakios-metal,chip0x6050002)` ("had no pipelines... opened
  through /private/var"), the same path every pipeline library without a
  precompiled file for this GPU takes; the pipelines are built from its functions
  by this runtime's compiler, as CoreAnimation's are;
- 64 command buffers a queue, memoryless textures given memory, and command
  buffers run in commit order (each said once in the log).

The map area is black in these runs, and the cause is the scene, not the
network stack: none of these scenes ever asked for a join.  The earlier scenes
that had Wi-Fi run `net_test wait 20 joinfile /redwood/fw/network`
(the join pattern those scenes used; both were removed on 2026-11-18 with the self-built apps); the Maps scenes only opened the app,
so the link stayed down and there were no tiles.  The driver lines that looked
like a mid-run Wi-Fi failure -- "background MHI polling failed: 5", "endpoint
reset", "service stopped" -- come after `loader_return_after expired` in the
log (`064413-run.log`: the return at line 10850, the polling failure at 10945):
they are the run's teardown, not a fault of the boot.  The location alert's rows,
measured off the 1080x2340 screenshots, are at 0.655 (Allow Once), 0.712 (Allow
While Using App) and 0.770 (Don't Allow); the scene's taps at 0.918 serve both
the What's New "Continue" and the notification screen's "Not Now", which sit at
about 0.92 of the screen.  The first attempt at 0.609 was the display-scaled
estimate, not the picture's, and only put the finger in the alert's text; the
taps are corrected in the scene.

With the corrected taps (scene `metal650maps8`, `logs/070220-run.log`) the alert
is dismissed and Maps sits in its normal state: the search field, Favorites
(Home/Work/Add), My Guides and New Guide drawn, the map's location button
spinning while locationd positions by Wi-Fi (`logs/maps8-2.png`).  The map area
is still black: the link was not joined (the scene never asked for a join; see
above), so there are no tiles and VectorKit has nothing to draw them from.  The
Metal side of Maps is now verified end to end -- launch, onboarding, permissions,
idle UI -- with no wedge; the map content waits on the Wi-Fi join, which the next
scene starts with `net_test joinfile` and `wifi_wide=0` (the 2.4 GHz BSS the
earlier A/B joined).

That join works: in scene `metal650maps9` (`logs/071452-run.log`, `wifi_wide=0`)
the link is up -- "the network is joined ... 1960 frames sent, 4805 received",
TCP connections from 192.168.0.103 to 219.144.75.246:443 and 23.11.38.161:80 with
14 ms and 328 ms RTT, 103,983,048 bytes received on en0 -- and Maps' map view
fetches its data.  The screenshot shows the blue location dot drawn at the map's
centre and the Amap attribution, so VectorKit's renderer runs.

What it does not do is draw the map: in that run Maps made 42 buffers of 44 MB
(the 1125x2436 RGBA/A16F colour and 4-sample depth-stencil targets) and its
command buffers contain only "copy 1125x2436" passes -- no draws, and the
plug-in's log has not one "linked" line from Maps (0 render pipelines in any of
061230, 070220, 071452).  Its pipelines come from VectorKit's
`iphoneos.pipelinelib`, which opens through the mirror with an empty cache for
`pipelines.Adreno(hakios-metal,chip0x6050002)`; whatever path builds its pipeline
states from that library does not come through this plug-in's
`newRenderPipelineStateWithDescriptor:` -- that is the wall the map view is
behind.  The 44 MB texture allocations that fail with "the driver gave no buffer
... Input/output error" (IOReturn timeout, request 66) and the 796 "pass has no
attachment" refusals, and Maps' SIGABRT "VectorKit received null buffer from
Metal" at 07:20:55, all come after `loader_return_after expired` (line 10155
against 10244+): the run's teardown, not the run.

### Why the map view draws nothing: VectorKit's pipeline library has no functions (2026-11-17)

VectorKit's `iphoneos.pipelinelib` is not like CoreAnimation's.  Its `functions`
file is **0 bytes**; the pipeline descriptions are in `pipelines.desc` (502,020
bytes) and the compiled pipelines are per Apple GPU (`pipelines.g10p` ...
`pipelines.g15p`, ~1.6 MB each, none for Adreno).  CoreAnimation's
`default.pipelinelib/functions` is 3,584,636 bytes beginning `LPPM`: a table of
451 entries, each a 16-byte hash, an offset and a size into an embedded `MTLB`
blob (`tools/ca-metallib.py` already scans for those `MTLB`s).  So CA's library
carries the AIR of its uber shaders and this runtime compiles them; VectorKit's
carries nothing this GPU can build, and Metal's builder returns nil for every
pipeline by name -- which is why Maps encodes only "copy 1125x2436" passes and
the plug-in never sees a `newRenderPipelineStateWithDescriptor:` from it.

VectorKit's `default.metallib` (2,466,580 bytes, carried under
`/redwood/share/ca-metallibs`) does contain the base functions the descriptions
name (`DaVinci::ground_base_vertex`, ...), and `pipelines.desc`'s names are the
library's `default_` prefix plus the function name plus a 64-hex-digit
specialisation hash, so the pieces to build them are on the device.

The M2V tree's iOS 16.5 patch names the path Apple's own pipeline-library
builder takes: `-newFunctionWithName:constantValues:functionCache:error:` on the
library ("with Metal's function cache, which holds nothing of this device's").
The next piece of work is to answer that call when the library cannot: look the
base function up in the carried `default.metallib` with the same constants, so
the builder's descriptor reaches this runtime's compiler exactly as
CoreAnimation's does.  That is a pipeline-library feature, not a GPU one; the
mountains (`pipelines.desc` parsing) are Apple's own code, which is why the
function fallback is the smaller door.

### Probing the lookup from inside the system is not safe (2026-11-17)

Two attempts to watch Metal's function lookups from the plug-in itself:

- a global swizzle of `_MTLLibrary`'s `newFunctionWithName:constantValues:error:`
  and its `functionCache` form, installed for every library (scene
  `metal650maps10`): backboardd aborted (SIGABRT) in a loop right after its
  first "Metal failed to load library" at boot -- launchd's "service has crashed
  7 times in a row" -- and the run's screen stayed black;
- a gated install, only for a library whose functions archive is empty:
  `metal650maps11` (an NSFileManager test -- the gate never fired) and
  `metal650maps12` (stat(); the same SIGABRT loop, and the once-per-process
  "pipeline library functions:" line never reached the log).

The committed plug-in has no probe: the source is back to the GMEM fix and the
fbstage logging, and `metal650maps13` is the baseline run after the revert.  The
library's open path is not somewhere to add instrumentation on a booting
system.  The next attempt is a standalone tool, one step of a scene and no window
server: a small Metal program that opens VectorKit's `iphoneos.pipelinelib`
itself, asks for a pipeline by name, prints what Metal answers, and then tries
the carried `default.metallib`'s base function with the same constants.

### The pipeline library opens now: a 0-byte functions archive given an empty LPPM cache (2026-11-17)

The refusal that kept VectorKit's library nil was not the missing
`pipelines.Adreno(...)` file -- the mirror already gives that miss an empty cache
Metal writes itself.  It was the library's `functions` file being 0 bytes: the
opened mirror answered "Could not open function cache file:
.../iphoneos.pipelinelib/functions" (`logs/061230-run.log`, `070220-run.log`).
Metal reads that file as its function cache, in LPPM: the magic, a 0x30-byte
header (two version words, a zero, the file's size at 0x10, 0x28 at 0x18, the
offset the blobs would start at in 0x20, the entry count at 0x28), then 48 bytes
per entry -- a 32-byte key, the blob's offset, its size -- as CoreAnimation's
3,584,636-byte file shows (451 entries; its first key is
`cccbc6fb6e0127d1fecff072d46105ca ece4ac02b29c0da3038b6c12902d1a35`).  A cache
with no entries is exactly this library's truth: nothing on this OS is
precompiled for this GPU.  The mirror now writes that 48-byte file where the
library's own `functions` is 0 bytes (`HakiosMetalCA.m`,
-newPipelineLibraryWithFilePath:error:), and in scene `metal650maps17` (run
`093154`) Maps' library opens: "the functions of .../iphoneos.pipelinelib are 0
bytes: the mirror gets an empty LPPM cache (48 bytes) for Metal to open" and
"had no pipelines.Adreno(hakios-metal,chip0x6050002); opened through ...: it
opened".  The map area was still black in that run (`logs/maps17-1.png`, the
search sheet, the location dot and the Amap attribution drawn over a black
canvas), so what Metal then makes of each pipeline by name is the next question;
`src/pipelinelib_test.m` (`RC_JOBS=rc.jobs.pipetest`) asks it of the same
library outside the window server.

Two facts from that run's plumbing: the loader's final log fetch can be empty
(`logs/093154-run.log` is 0 bytes where the `.live` file had 1.3 MB), so evidence
has to be taken from the `.live` file during the run; and the cmdline truncated
again by 2 characters -- the last `wifi_wide=0` lost its `=0` -- because the
loader's own prefix (the kernel version string included) leaves less room than
the payload's 378 characters suggest.  The arguments that matter (`ca_metallib`
whole) were intact.

The probe answers the next question (scene `pipetest`, run `094819`, captured in
`logs/094819-pipetest-captured.log`): the library opens as `_MTLPipelineLibrary`,
but every name the descriptions list answers nil -- "render pipeline
default_DaVinci::ground_base_vertex_909AB... not found in pipelineLibrary" -- so
the by-name road is a lookup with no build-on-demand; what is not precompiled
for this GPU is simply absent.  The other pieces are there: VectorKit's
`default.metallib` opens as `_MTLLibrary`, and `DaVinci::ground_base_vertex`
(23,296 bytes of AIR), `Grid::grid_fragment` (6,640) and
`DaVinci::ground_fragment` (35,632) all answer `newFunctionWithName:`.  So a
pipeline for Maps has to be built from those functions and the description's
state, not looked up by name; `pipelinelib_test` now also asks the library for
`pipelineNames` and builds a Grid pipeline from the two plain functions, to tell
which of those two roads this runtime can take.

The answer came back clear: the library says its `pipelineNames` are 0, and a
Grid pipeline built by hand from `Grid::grid_vertex` + `Grid::grid_fragment`
compiled to a `HakiosMetalRenderPipelineState` -- the runtime's compiler is not
the limit; the by-name lookup is (`logs/095313-pipetest2-captured.log`).  So the
plug-in now answers VectorKit's by-name calls itself: when
`-newRenderPipelineStateWithName:options:reflection:error:` finds nothing in the
library, it takes the two functions the name's description pairs, resolves them
to the base functions of the carried `default.metallib`, and builds the pipeline
through the device (`HakiosMetalCA.m`, `hm_vk_new_pipeline`).  Until
`pipelines.desc`'s own attachment state is decoded, the pipeline is built for
the canvas's kinds first -- BGRA8_sRGB (81) with a 4-sample
Depth32Float_Stencil8 (260), what VectorKit's colour and depth textures were --
and, only if this runtime refuses those, for one plain BGRA8 target with no
depth; `vkc=`/`vkd=`/`vks=` override the three for an A/B run (the arguments
cost 21 characters of the 607, so an A/B run drops the camera ones).  The first run
meant to exercise it (`metal650maps19`) never reached Maps: the battery guard
ended it at 24%, at or below `battery_stop=25` ("battguard: ... back to the
loader", `logs/102725-run.log`), before the scene's `uiopen` at rc+90 s.  The
loader does not charge -- a phone at this level has to go back to Android, or to
a charger, before the next run.

The description parser was checked against the metallib before the run: all 235
entries of section 7 hold exactly two "Namespace::name" strings, every one of
them is vertex first, and all 105 distinct base functions they name exist in
`default.metallib` (309 functions there in all).  334 names therefore map to a
pair -- a lookup of any of them can be answered; a name outside that set is
logged and left nil.  `rc.jobs.pipetestmaps` runs the probe first and Maps after
in one boot, so the answered library is exercised both by a name the probe asks
for directly and by whatever names the app itself asks.

The first run of that scene (`logs/105440-run.log`, at 28% battery, the guard
holding) did not settle the question: its log jumps from launchd straight to
10:59:44, so the whole window in which Maps starts -- the probe's output and any
pipeline-library lines with it -- is missing.  What the tail does show is Maps
up and drawn (`logs/ptm-3.png`: search sheet, favorites, the location dot, the
Amap attribution, a transient black band across the favorites) over a black map
canvas, and then, at the return, its 44 MB colour and depth textures refused
with `0xe00002d8` -- kIOReturnNotReady, the kernel's "the run is ending" answer
-- followed by the same "VectorKit received null buffer from Metal" abort seen
in earlier runs.  Those refusals are the run's end, not its cause, for the third
time; do not read them as the map's wall.

The lost window is the log ring: `touch_lat=1` writes a line for every swap and
every frame at 90 Hz (`tl:` lines, MiniIOMobileFramebuffer.cpp), and between the
loader's fetches that flood overwrites what it has not read.  A diagnostic run
can drop it: `ram.args` without `touch_lat=1` is otherwise the same line, and
the next Maps run does that (the taps still work; only their logging is gone).

Answered on the device, at last (quiet run, `logs/111646-run.log` and its live
capture): the probe's by-name request for
`default_DaVinci::ground_base_vertex_909AB73C...` came back a
`HakiosMetalRenderPipelineState`.  The plug-in opened the library ("235
pipelines described, none precompiled for this GPU; its by-name lookups are
answered from its functions here"), opened the carried `default.metallib`, and
built the state from `DaVinci::ground_base_vertex + DaVinci::ground_fragment`
-- "it compiled", with the canvas's own kinds (81 / 260 / 4) accepted on the
first try.  Maps opened the same library and installed the same answer, but in
that run it asked nothing by name: the library's own `pipelineNames` is 0 (the
precompiled file it would be read from is absent), so a client that enumerates
the library learns nothing.  `vkn=1` answers that list with the descriptions'
334 names, which the by-name answer then serves; it is off unless a boot asks
for it, and the next run tries it (with `wifi_wide=0`, so the map has data to
draw).

Then the shared cache itself answered how the app asks.  In
`out/thyme-boot/dsc-arm64/dyld_shared_cache_arm64.09` (the same region holds
VectorKit's function-name table -- every `DaVinci::...`, `Textured::...` base
name -- and, 15 KB on, the string `iphoneos.pipelinelib` and its
`.../VectorKit/geo/GeoGL/GeoGL/MetalRenderer.mm` assert path), the selector
names include:

- `newPipelineLibraryWithFilePath:error:`
- `newFunctionWithName:constantValues:pipelineLibrary:error:`
- `newRenderPipelineStateWithDescriptor:options:reflection:error:`

and **neither `newRenderPipelineStateWithName` nor `pipelineNames` occurs
anywhere in the cache**.  So VectorKit does not look pipelines up by name at
all: it opens the library, asks for a *function* with constant values and the
pipeline library, and builds the state by descriptor.  Metal would answer that
function from the library's `functions` archive, which is 0 bytes on this OS
image -- the missing piece was the function lookup, not a name lookup.  The
plug-in's device now answers
`-newFunctionWithName:constantValues:pipelineLibrary:error:` from the carried
VectorKit `default.metallib` with the same constant values (the name stripped
back to its base function when it is a `default_<base>_<hash>` description
name), and logs every such call either way, so the next run says whether the
device is the receiver.  Which class declares the selector is not written
anywhere readable -- Metal.framework's metadata is inside the cache -- so the
same answer is also installed on `_MTLLibrary` when VectorKit's library opens
(the class M2V's patch put the twin `functionCache:` call on); each answers
only when Metal's own implementation gives nil, and falls through to it
otherwise.

The two runs that carried those answers (36% and 40% at their starts, `wifi_wide=0
vkn=1`) put Maps' UI up over the same black canvas and lost the evidence again:
`logs/ptm2-1..4.png` are the app at 12:01-12:03, and `logs/115838-run.log` has
neither the probe's output nor a single Maps pipeline line -- the loader's live
file is a fixed 1.3 MB window, so the early lines are overwritten before the run
ends.  The next run keeps them in files instead: the job list writes the probe's
output to `/private/var/tmp/pipetest.out` with init's `>>`, the plug-in appends
its VectorKit lines to `/private/var/tmp/hakios-vk.log` under `vklog=1`, and the
same two files are `cat`-ed again after the taps and every 30 s, where the log
ring still holds them (`rc.jobs.pipetestmaps`, image `9c6070d9`).

That worked, and the files answered the isolated question (run `122309`): the
probe asked the function road of **both** receivers and both answered --

```
pipelinelib_test: the device answers the pipeline-library function road: yes
pipelinelib_test:   newFunctionWithName:constantValues:pipelineLibrary:error: = a function
pipelinelib_test:   it carries 6784 bytes of AIR
pipelinelib_test: the library answers the pipeline-library function road: yes
HakiosMetal[pipelinelib_test 123]: VectorKit's function DaVinci::ground_base_vertex,
  asked with constants and a pipeline library: compiled from its metallib
```

`_MTLLibrary` does declare the selector (Metal's own implementation answered the
library receiver with a function without any hook of ours), so the road is an
MTLLibrary method, and the device has been given one too.  Maps wrote its own
lines to `/private/var/tmp/hakios-vk.log` (the sandbox does allow it) and the
log shows it opening the library and installing both answers -- but it never
asks for a function or builds a pipeline: the last Maps line in that file is the
library's names being given, and the canvas stays black (`logs/ptm2-3.png`).

So the app's road to functions is neither the device's nor a library's road, or
it never gets as far as asking.  Whatever it does next after creating the
pipeline library is now said too: the device logs every `newLibraryWithFile:`
and `newLibraryWithData:` (with the data's first four bytes), answers a 0-byte
data -- the pipelinelib's `functions` archive is exactly that -- and the
archive's own path with the carried VectorKit `default.metallib` once the
pipeline library has opened, and the pipeline library itself is the third
receiver of the function road, answered the same way (image `ff064bf2`).
Whether the app's renderer encodes any pass at all is said too: once VectorKit's
library is open in a process, the command buffer logs the first 20 render passes
(colour and depth format and size), which tells an idle renderer from one
encoding passes with no pipeline (image `2299fb92`).

That answered it (run `124956`, image `9f4b5c3d`): Maps **does** call
`newLibraryWithData:` with **0 bytes** -- the pipelinelib's `functions` archive,
read straight out -- and the answer written for it (the carried
`VectorKit.framework/default.metallib`) is taken: "a library from 0 bytes of
data: the carried VectorKit metallib is given instead", then the library opens
with its functions.  Maps also encodes render passes, with exactly the canvas
this session measured before: `a render pass was encoded: colour format 81
1125x2436, depth format 260 1125x2436` -- so VectorKit's renderer is running,
in a process with the right library, and still draws nothing (no `linked`
pipeline in the whole run, canvas black, `logs/ptm2-4.png` after the favorites
sheet was swiped away: only the search bar is left).  The function road is not
called on the device or seen at a library; whether a library is asked for
functions by plain `-newFunctionWithName:` is now said too (the first 40
lookups in a process that opened VectorKit's library), and the function road at
a library is said on every call, not only when Metal's own answer is nil (image
`693be046`).

That hook named the missing functions (run `131739`): Maps asks the library it
was given (the carried `default.metallib`) for

```
a library was asked for function Clear::clear_vertex: NOT FOUND
a library was asked for function clear_vertex: NOT FOUND
a library was asked for function Clear::clear_fragment: NOT FOUND
a library was asked for function clear_fragment: NOT FOUND
```

-- and `default.metallib` has no `Clear` at all.  They are in VectorKit's second
library, `metal_libraries/AlloyCommonLibrary.metallib` (46,569 bytes, a plain
MTLB with `Clear::clear_vertex`, `clear_depth_stencil_fragment`,
`clear_fragment`, `clear_vertex_mrt`, `clear_fragment2..4`, `ShadedColor`,
`ColoredText`, `TextureRect`, ...), which the system image drops by the same
rule.  The plug-in now falls through to that library for any name the default
metallib has not: the plain `-newFunctionWithName:` (where the NOT FOUND names
came from) and the constant-values function road both try it, with a re-entry
guard so the plug-in's own lookups do not loop (image `23b3794b`).  The Alloy
library is carried beside the default one in the same `ca-metallibs` tree.

And that worked on the device (run `133635`, image `23b3794b`): the Alloy library
opens ("VectorKit's other functions: .../AlloyCommonLibrary.metallib: it
opened") and Maps' `Clear::clear_vertex` / `Clear::clear_fragment` come back
"found in the Alloy library".  Maps then encodes 60 passes and draws -- and the
draw is refused 1,636 times with the same sentence:

```
refused: a draw of Clear::clear_vertex+Clear::clear_fragment, primitive 3,
6 vertices from 0, 1 instance(s), load 0 store 0, 2 vertex and 1 fragment
buffer slots: a stencil attachment beside a multisample Depth32Float_Stencil8
texture: the S8 plane's multisample layout is not laid out here
```

So the map's canvas is now blocked by the one thing the runtime left out on
purpose: a multisample Depth32Float_Stencil8 is laid out as its depth plane
alone (`hakios-metal/src/texture.cpp`), and a pass that binds its stencil plane
is refused (`render.cpp`).  VectorKit's clear pass binds exactly that.  The next
change is the S8 plane of a multisample D32S8 (per-sample stencil, laid out
like the depth plane's samples side by side) and the stencil buffer setup for a
multisample pass; the single-sample S8 plane is already served from the same
texture (stencilOffset/stencilPitch, RB stencil branch), so the work is the
multisample pitch and the pass check.

That change is in (`hakios-metal/src/texture.cpp`, `render.cpp`; image `59eade14`):
`plan()` allocates the S8 plane of the multisample memory after the depth
plane (samples side by side, one byte each, `s8Bytes(width * samples)`), the
depth-plane/stencil accessors are samples-aware, the pass check that refused a
multisample stencil is gone (the same-texture and load/store checks stay), and
the stencil clear fills the wider plane (width from its pitch, rows to 32).
Rebuilding the runtime is `make ios MESA=1`: without `MESA=1` the library leaves
the AIR compiler out (`compiler.o` 12 KB instead of 111 KB) and every pipeline
would fail; the scene image then shrinks by about 1.3 MB, which is how the
mistake was caught before a run.

That run (`135944`, image `59eade14`) passed the stencil gate -- the refusal is
gone and Maps' Clear pipeline links -- and then failed on the GPU's own address
space: every canvas texture, colour and depth, came back "Cannot allocate
memory", and the kernel's log named the real reason --
`HakiosGPU: a buffer of 44285952 bytes cannot be entered into the GPU's address
space`.  The Adreno driver's page-table pool was `SPACE_TABLES 1024`, about 2 GB
of mapped addresses, and VectorKit's canvases (each 44 MB colour, now 56 MB
depth with its S8 plane) plus everything else walked past it; `space_new_table`
returned NULL and the mapping failed without a page-table slot.  The pool is now
`2064` (2048 two-megabyte tables, the four level-2 tables they hang off, and the
root: 4 GB, the whole 32-bit window the allocator hands out,
`devices/thyme/drivers/gpu/hakios.thyme_adreno.c` and the kernel tree's copy;
the kernel is rebuilt with `./xnu-build.sh` and the boot image with
`devices/thyme/build-loader.sh`).  This is the same class of limit the annibale
port hit at 1024 slots on the Adreno 830, and the fix is the same.

One packaging trap, learned the hard way in the same hour: `xl run` takes the
kernel from `./hakios-kernel` (its `inputs: kernel=<hash>` line is that file's),
not from the boot image and not from XNU's `BUILD/obj/.../mach.development.*`.
A manual `./xnu-build.sh` leaves `hakios-kernel` untouched; the kernel is
installed only by `tools/make-kernel.sh` (what `make kernel` runs), which also
syncs `drivers/` into the tree and judges the build by what failed -- the kext
symbol-set steps always fail on this configuration and are accepted, anything
else is not. A run started after only `xnu-build.sh` silently ran the old
kernel.

With the fixed kernel actually loaded (run `144139`, `xl: inputs: kernel=4ce8d7ba31d1`,
image `59eade14`) the Metal side of Maps came down to the last wall and through
it: **the canvas textures allocate** (the 69 "driver gave no buffer" that remain
are all after `--- loader_return_after expired ---`, the run's teardown), Maps
encodes 80 render passes, **no draw is refused**, and the Clear pipeline links.
The kernel's own buffer statistics show it: 56 buffers made, 4 kept ones given
again, 0 MB kept.  The screenshot at the end of the run shows the app's own
"My Location" card -- address "Ziwei Street and Xuefu Street Road, Koudong
Wanjing Lizhiwan, Chang'an, Xi'an", Directions / Drop Pin / More -- over a
canvas that is still black (`logs/ptm2-4.png`): the map view now takes a tap,
asks locationd and geocodes, and draws its UI, but nothing is on the canvas yet.
No tile or map-data host appears in the run's connections (only a handful of
Apple 17.x ones), so what the canvas lacks is content to draw, not a pipeline:
the next thing to check is the tile/data path (geod, the map style resources,
and a run long enough after the join for the first tiles to arrive), not the
GPU.

That guess was half wrong: geod *is* fetching.  The same run's connection
records show `geod[57]` connected to Chinese map hosts -- 106.11.43.101:443 (one
connection open 120 s, 13,968 bytes in / 981 out) and 219.144.78.208:443 (15 s,
7,411 in / 1,908 out) -- so the resource/tile path works and the run was simply
short: those are manifests and the first resources, not tiles yet.  The next
scene leaves Maps alone after the sheet is swiped away (the tap at 0.35 opened
the "My Location" card, which pauses the map), runs for 600 s and takes shots at
4, 5.5, 7 and 8.5 minutes (image `8d68102a`).

Result of that long run (`152458`, 600 s): still no map content.  Maps linked
only `Clear::clear_vertex + Clear::clear_fragment`; no `DaVinci`,
`SolidRibbon` or other ground pipeline was created; the four shots
(`logs/ptm3-1..4.png`, 150-186 KB each) show the app over a black canvas.
geod's connections to the Chinese map hosts were seen again, so the data path
is alive, but VectorKit never got to drawing tiles.  **The user's direction on
2026-11-17: set Apple Maps aside for now** and turn to third-party maps --
install a third-party store / IPA apps (Baidu Maps, Amap), check they run, and
use one of them as the daily map if it works.  The way in is already in the
image: `rc.jobs.ssh` starts OpenSSH from the jailbreak prefix (sshd/scp/sftp)
and the host can push an `.app` over Wi-Fi; `/var/jb/usr/bin/uicache` refreshes
the icon cache, and the host can `ldid -S` the bundles before they are pushed.

`vkn=1` remains as an opt-in for anything that does enumerate names, but
nothing in the cache calls `pipelineNames`.

### The boot that stays black: ca_metallib cut by the 607-character command line (2026-11-17)

Three of the last boots (`metal650maps10`, `-12`, `-13`) never reached a drawn
frame: backboardd aborted in a loop ("service has crashed 22 times in a row"),
the screen stayed black, and every instance printed `[Metal] Metal failed to load
library: library not found` immediately after inject.c's answer to
CoreAnimation's bundle lookup ("asks bundle ... default.metallib:
/redwood/share/ca-share/QuartzCore-default.metallib").  The good boots
(`maps9`, `maps11`) have none of those lines.

What can be compared says the inputs were the same: the scenes' carried files
are byte-identical in all five images (sha256 `d7cdaaaf...`), maps9 and maps13
carry the same plug-in binary (`553d251f...`), the kernel is `82abf2539d68` in
every run, and both logs show the same mounts ("resident image ... it is the root
(md0), the payload ramdisk is md1") and the GPU's firmware read from `/dev/md1`
at the same steps.  What differs is the read of that one compressed file at that
one moment -- it is `[decmpfs 4]` in the image, where the plug-in's own binary
is not.

The cause was simpler and in the command line, not in the file system: the bad
boots' `ca_metallib` is cut.  The kernel's boot-args buffer is 607 characters and
truncates the line; the boots that failed carry
`ca_metallib=/redwood/share/ca-share/QuartzCore-defa` or `...-default.me` in
their `cmdline:` line, and the boots that drew carry the whole
`/redwood/share/ca-share/QuartzCore-default.metallib`.  Every one of the eight
boots matches its own line: `071452` and `075207` complete and good, `073849`,
`080047`, `081114`, `082608`, `084141`, `085328` cut and black (the last with the
kernel's own "the kernel command line is TRUNCATED: 97 characters ... did not
fit").  Metal was handed a path that does not exist -- the "library not found"
is literal.  What made the line long was this session's own diagnostics: a
duplicate `wifi_wide=0` (one from ram.args, one from the run script),
`metal_fps=1`, and xl's three `*_sha=` words.

The fix is the command line, and a seed that does not depend on it:
`tools`-side the run scripts now pass `XL_NO_HASHES=1`, one `wifi_wide`, and no
`metal_fps` (about 95 characters less; the line fits with room).  On the device,
init (`userland/src/init.c`, `seed_carried_metallibs`) copies CoreAnimation's
metallib and the nine carried framework metallibs into
`/private/var/tmp/hakios-ca` and into the window server's own
`/private/var/tmp/com.apple.backboardd/hakios-ca` before launchd starts;
`inject.c` answers CoreAnimation's lookup with the seed's fixed name
(`QuartzCore-default.metallib`, taken from the seed rather than from the boot
argument, which is what the cut broke) and the plug-in's two carried-metallib
lookups ask the seeded trees first.  ram.args is back to the proven 268-character
set (the `wifi_wide=0` this session had added to it is gone again; the run
scripts pass it when a scene wants it), so the daily line has the margin the
previous ones did not.

Verified: the next boot with the shortened line (`logs/090448-run.log`, kernel
`82abf2539d68`, scene `metal650maps16`) drew the whole home screen again
(`logs/maps16-1.png` at 09:10, `-3.png` at 09:11), Wi-Fi joined in the status
bar, and its screenshots are 2.4 MB where the cut boots' were 15 KB of black.
That run's log came back from the loader as 10 MB of binary noise -- the fetch,
not the boot -- so its `cmdline:` line and the seed messages could not be read;
the drawn frames are the evidence that the argument was whole.  Maps did not
come up in that run (the scene's `uiopen` either lost the race with LaunchServices
or its taps went to the home screen); that is the scene's timing, not the boot.

### Another port to learn from: the Redmi K90 (annibale) (2026-11-17)

The user pointed at <https://github.com/iola1999/hackios-redmi-k90-annibale>: a
HakiOS port to the REDMI K90 (SM8750, Adreno 830) that runs the iOS 26.6.2
userland and states much further app coverage.  Read for what crosses over; its
Metal is another stack (Mesa freedreno behind an AIR-to-NIR translator,
`annibalemetal`), so its shader fixes (`alias.rt`/`fsat`, sampler swizzle,
`rgb2p`) do not bear on hakios-metal's own compiler.  On the walls this session
is on:

- Maps' map area is blank there too: its backlog's S12 still lists "Weather 无温度、
  Maps 停在定位提示" (`docs/backlog/system.md`).  Its Today-view map *widget* does
  draw after a blit-over-resource and colour-attachment fix in its Mesa layer
  (`docs/analysis/2026-11-17-transient-render-and-icon-fixes.md` §4), not by
  solving VectorKit's pipeline library -- so the by-name pipeline wall here has
  no ready-made answer there.
- FairPlay: it masks `fairplayd.H2` so `locationd`'s `HostSpecialPort 17`
  request fails fast (Wallet's 10 s black screen); we answer those services
  with `fairplaystub` instead.  Worth remembering if something here blocks on a
  fairplayd request that never returns.
- Its Adreno 830 SMMU had a fixed 1024-slot page table (a 2 GB IOVA window) that
  failed 12-29 MB allocations once the low space fragmented; ours is a per-client
  table of 8192 buffers with a 256 MB maximum each, so that exact limit is not
  here -- but the symptom (a large GPU buffer failing with EIO) is worth
  checking against it if one ever fails before teardown rather than after.
- Its kernel-side framebuffer ring recorder plus `fb_filmstrip.py` (96 MiB,
  104 frames, black-flash/horizontal-band/alternating-flicker detection) is a
  better tool than `xl screenshot` for the transient flicker seen here; a
  candidate for the next display investigation.

### Third-party applications: the store and an App Store app (2026-11-17)

The next daily-use pieces after Maps was set aside: a third-party store, then the
Chinese map apps the user named, if they run.  Two findings gate both, both found
in `logs/170310-run.log` while iSH kept failing:

- A scene line ending in ">> FILE 2>&1" loses BOTH the append and its last words.
  init's `run_command` (src/init.c) understands ">> FILE" only as the line's last
  two words; with "2>&1" after it, append_to stays NULL and ">>", the path and
  "2>&1" are passed to the program as arguments.  apps3.txt was never created
  and the late cats said so.  Scenes now use a shell script for redirection.
- /var/jb is a graft of the read-only /jb (src/init.c, "the rootless prefix"):
  the directories are real, every file is a symbolic link.  chmod and cp
  "into" the bundle therefore write through the link into the read-only volume
  and fail EROFS, which is why iSH's binary stayed 644 in jb/root/var/jb/...
  in the image.  xpcproxy's access(..., X_OK) then failed with EACCES.

The working install path is now `userland/bin/hk-appinstall.sh` (copied to
`/redwood/bin`): it replaces each executable link with a real copy from
`/Applications` and chmods it, then runs `uicache -a`.  Verified on the phone:
TrollStore 2.1.1 launches (pid 89, `logs/172052-run.log`) and draws its
Apps/Settings window (`logs/app5-2.png`..`-4.png`).  For an app the system image
already carries, LaunchServices finds `/Applications/<app>.app` first and that
copy cannot be chmod'ed; `hk-appjb.sh` installs a copy with a CFBundleIdentifier
of its own (`iSHjb.app`, `app.ish.iSH.jb`, its Info.plist carried as
`/redwood/share/ishjb/ishjb.plist`), so the launch takes the writable path.
Verified: iSH spawns (pid 76, "Successfully spawned iSH[76]",
`logs/173118-run.log`), its terminal takes the keyboard (`logs/app6-1.png`), and
the switcher shows the iSH back item (`logs/app6-3.png`).  That is the path a
newly-installed IPA takes; the store and the install mechanism now work.

What was NOT done, and why: the user chose "download from public sources" for the
Baidu/Amap IPAs, but the public pages found are login/VIP-gated, and the only
other source is decrypted ("砸壳") copies of copyrighted apps -- not something
this port will fetch.  The replacement being built instead is HakiOS Maps
(`src/HakiosMaps.m`, `userland/maps/`): a WKWebView over Leaflet and
OpenStreetMap raster tiles, the same WebKit path Safari already renders here, so
the screen does not depend on VectorKit at all.  The stock Apps remain installed;
an IPA the user owns can be installed with TrollStore or the two scripts above.

### HakiOS Maps: the map without VectorKit (2026-11-17)
> **Removed 2026-11-18** on the owner's instruction: no self-built replacement apps; the
> existing-app path (an IPA installed through the store) is the only one this port takes.
> The record below is history, and the files it names are no longer in the tree.

`userland/src/HakiosMaps.m` (and `HakiosMapTileView.m`, with `userland/maps/` for the page
and Leaflet 1.9.4, BSD-2, credited in `LICENSE.leaflet`) is the replacement map screen.
It is installed the LocProbe way (`rc.jobs.mapapp` copies it from `/redwood/fw/hmaps` into
`/var/jb/Applications`, `chmod 755`, `uicache -a`, `uiopen com.hakios.maps`) and logs to
`/private/var/tmp/hmaps.log` (world-writable; the app cannot rely on `/var/jb/hmaps`, which
is a graft like the rest of `/var/jb`).

Two renderers, one screen:

- WKWebView over the bundled Leaflet page, with the CSS and JS inlined and the page loaded
  from a string (a file URL's read access and the GPU process were both suspects).  The
  WebKit knobs are SPI and are set only where the class answers them:
  `_setUseGPUProcessForDOMRenderingEnabled:`, `_setUseGPUProcessForCanvasRenderingEnabled:`
  and `_setUseGPUProcessForWebGLEnabled:`.  Why: in the first map run the GPU process
  (pid 63) created its hakios-metal device, a command queue and a 1125x1536 BGRA IOSurface,
  then died with SIGABRT (`logs/174007-run.log`, "exited due to SIGABRT", the
  `EXC_CORPSE_NOTIFY` error code 5 follows), and the view stayed its own background colour
  (`logs/map-1.png`).  Safari's pages render on this port, so WebContent's own drawing is
  the path that works; whether these preferences exist in 16.5's WebKit is answered in
  hmaps.log at run time.
- A native `HakiosMapTileView` (UIKit + Core Graphics): slippy-map Mercator math over
  OpenStreetMap's 256-pixel tiles fetched one zoom higher and drawn at 128 points (sharp on
  the 3x panel), with pan, pinch, double-tap, the position dot and the required attribution.
  The controller gives WKWebView fourteen seconds and then swaps this in, so the screen is
  a map whether or not WebKit's renderer comes up.  Tiles are NSURLSession GETs to
  `tile.openstreetmap.org` with a `HakiosMaps/1.0 (HakiOS, thyme)` User-Agent; no key.

The first device run of the page showed the app running (`Successfully spawned
HakiosMaps[57]`, `logs/174007-run.log`) but no tile had drawn when the run was returned by
the battery guard: `battguard: the charge is 23%, at or below battery_stop=25 three looks in
a row` (17:42:56).  The phone was then found at 16% on Android's USB charger (level 16,
status 2, 3020 mV, 37.7 C) and left charging; it dropped off adb/fastboot entirely a few
minutes later, so the fallback build's run is waiting for it to come back.  Lesson for the
scene runner: check the battery before a run and keep runs short below ~30%.

What TrollStore's own install path needs, read from its source (opa334/TrollStore, `RootHelper/main.m`,
`TSApplicationsManager.m`): by default (`installationMethod` 1) the helper's `install custom` creates an
app container through MCM (containermanagerd), copies the bundle into it, applies its CoreTrust bypass
and registers the app with LSApplicationWorkspace; the other method (`installd`) goes through
MobileInstallation. Both containermanagerd and lsd are running on this port, so the "custom" path is
the one to try first; the open questions are whether the helper gets root with TrollStore's
entitlements here (entitlement enforcement is thin on this port) and whether the copied bundle's
signature matters at all. `build.sh` now takes `INSTALLD=1`, which adds `com.apple.mobile.installd`
and `com.apple.mobile_installation_proxy` to the job list for the second method; it is off until a
run shows them up harmlessly.

That path is now a scene (`userland/rc.jobs.tstore`, image `out/thyme-boot/root-tstore.apfs`,
`bd369554`): it seeds TrollStore's `installAlertConfiguration` = 2 (install without asking),
installs TrollStore with `hk-appinstall.sh`, and opens `/private/var/tmp/LocProbe.ipa` as a file
URL with `uiopen --url` -- TrollStore is the only app on the system that claims
`com.apple.itunes.ipa` -- so its root helper can do the MCM-container and LS-registration install
with a root persona. The IPA is the LocProbe bundle packed by `tools/ipa.py` (mode-preserving:
a 644 binary was the iSH wall, and `zip(1)` is not on this host). The image carries
`/fw/ipa/LocProbe.ipa` (9,200 bytes) and the preferences file; the run waits for the phone.

The scene now runs TrollStore's helper directly first (`trollstorehelper install custom
/private/var/tmp/LocProbe.ipa`, as root, exactly what the UI spawns with a root persona), so the
sign/MCM/LS steps print to the console through NSLog; then it opens the same IPA through
TrollStore with `uiopen --url` to test the hand-off as well. `launchctl` is not in the image, so
`OS_ACTIVITY_DT_MODE` cannot be set globally for the UI's own os_log. Image: `root-tstore.apfs`
`8e0f2556`.

### QEMU as a bench for the map app (2026-11-17, evening)

With the phone away, the current system image and the map scene were booted under
`qemu/run-darwin.sh` (ROOT=`out/thyme-boot/root-mapapp.apfs`, IMG=`userland/system.apfs`,
QEMU_NET=1, 600 s; qemu-system-arm 8.2.2 installed for it): SpringBoard draws the home
screen through the GL path, Settings opens (`Successfully spawned Preferences`,
`logs/qmap5-1.png`), and the map scene's install steps run and leave the bundle at
`/var/jb/Applications/HakiosMaps.app` with its binary 0755 (`install.log`). But
`uicache -a` + `uiopen --bundleid com.hakios.maps` (and later `--path` and a URL-scheme
open) never reached launchd -- no `UIKitApplication:com.hakios.maps` line at all -- while
Settings opened in the same boot. The phone's own LocProbe launch (`Successfully spawned
LocProbe[224]`, bundle path `/private/var/jb/Applications/LocProbe.app`, 2026-11-15) shows
the runtime registration route can work there, so this is either a QEMU-only limit of
LaunchServices' third-party registration or a real change since that image. The scene now
runs `uicache -p <bundle>` and `uicache -i <id>` and prints both into `install.log`, which
will say which; app launch for the map has not yet been verified on the phone.

The next QEMU boot settled the launch question: with `uicache -p /var/jb/Applications/HakiosMaps.app`
(the bundle named, instead of a rescan) the app is registered and the first `uiopen --bundleid
com.hakios.maps` launches it -- `Successfully spawned HakiosMaps[56]`, and `logs/qmap6-1.png`
shows the app's own screen: the search field carrying 西安大雁塔 (the auto-search query), the
定位 button, + and −, the 500 m scale bar and "© OpenStreetMap contributors".  So `uicache -a`
was the weak step under QEMU, not the app; the phone scene uses `uicache -p` now, with `-i`
printing what LaunchServices holds into `install.log`.

No tile arrived and the search said 0 results in that boot, for a network reason: en0 sent
0 frames (no DHCP in the image, and configd's IPConfiguration -- the only DHCP client the port
has for virtio -- was not a job). `CONFIGD=1` in build.sh adds com.apple.configd, and the
QEMU-only scene `rc.jobs.mapqemu` asks for DHCP and probes both map hosts before opening the app.

One anomaly worth knowing: in a QEMU run, console output masks every byte to 7 bits. The app's
record of the Chinese query reads E8 A5 BF ... as it is written, but the console capture holds
68 25 3F ... (each byte & 0x7F; checked with od). Whether the phone's loader console does the
same has not been measured; either way a mangled 中文 line in a run log is the console's doing,
not necessarily the data's.

### The map's network is blocked here, and the source is now a setting (2026-11-17, late)

The QEMU run with `CONFIGD=1` and `DNS=1` settled the rest: `net_test dhcp en0` got en0 an
address from QEMU's DHCP (`setservice en0 DHCP: 0 (done)`), mDNSResponder then resolved
`tile.openstreetmap.org` in 1.55 s -- but to **185.60.219.41**, a Facebook address, and every
TCP connection to 443 timed out. The host's own resolver gives the same answers for both
`tile.openstreetmap.org` (185.60.219.41) and `nominatim.openstreetmap.org` (157.240.20.8):
this network poisons openstreetmap.org, and the host only reaches it because its own HTTPS
goes through a transparent proxy that the guest (and the phone) do not use. The app's sockets
show the same: `SYN_SENT` to those addresses and a `RST` back (QEMU run log, process
HakiosMaps:56). So on this Wi-Fi neither tiles nor Nominatim can work from the phone as it
stands; the app is not at fault.

The answer is a setting, not a rebuild: `HakiosMaps` reads `/var/jb/hmaps/tiles.conf` (or
`/private/var/tmp/hmaps-tiles.conf`, which wins) at launch --
`tile_url` with {z}/{x}/{y}, `attribution`, `max_zoom`, and `search_url` with {q} -- and logs
both URLs it will use. `userland/maps/tiles.conf.example` carries the defaults and a
Tianditu WMTS line (a free key from tianditu.gov.cn) as the reachable-in-China example.
The example rides in the root image as `/redwood/share/hmaps/tiles.conf.example`.

Also settled: `uicache -p <bundle>` registers the app where `uicache -a` did not under QEMU,
and the first `uiopen --bundleid com.hakios.maps` then launches it
(`Successfully spawned HakiosMaps[56]`) -- the map scene uses `-p` now. And mDNSResponder is
not in the minimal job list: `DNS=1` in build.sh adds it and its helper, which is what turned
"nodename nor servname" into an answer at all. The phone image (`root-mapapp.apfs`,
`165b00c9`) now carries both.

### TrollStore installs an IPA on the device (2026-11-17, night)

It works, through the same helper the store's UI spawns. `logs/225723-run.log`:
`[installApp] doing custom installation using MCMAppContainer`, the container
`/private/var/containers/Bundle/Application/6C370502-.../LocProbe.app`, `trollstorehelper
returning 0`, and then `Successfully spawned LocProbe[83]`. The steps and the walls:

1. **Entitlements are real on this port.** `mcm_test` (src/mcm_test.m) asked
   `MCMAppContainer containerWithIdentifier:createIfNecessary:existed:error:` as root and got
   MCMErrorDomain 98; signed with the keys TrollStore's root helper carries
   (`platform-application`, `com.apple.private.security.container-manager`,
   `com.apple.private.MobileContainerManager.allowed`, ...; src/mcm_test.entitlements) the same
   call **created** a container under /private/var/containers/Bundle/Application
   (`logs/223433-run.log`). An ad-hoc ldid signature is enough; the port's userland honours the
   embedded entitlements. (The private class only exists once its framework is loaded: the probe
   dlopens MobileContainerManager.framework before looking it up.)
2. **TrollStore needs an MCM container of its own.** The helper's `isLdidInstalled()` asks
   `trollStoreAppPath()`, which is the MCM container of com.opa334.TrollStore -- not the
   /var/jb copy. Without one it answered 173 even with ldid sitting in the bundle
   (`logs/214043-run.log`). `install-trollstore` cannot make it either: the system image carries
   TrollStore.app in /Applications, LaunchServices counts that as an immutable system app, and
   the install is refused with 179 (unregistering it with `uicache -u` fails,
   `logs/220354-run.log`). So the scene has `mcm_test` create that container and copy the app
   plus the arm64 ldid (github.com/opa334/ldid/releases/latest/download/ldid, latest) into it.
3. **Sign the IPA with entitlements that are not restricted.** A binary with no entitlements at
   all gets TrollStore's fallback `get-task-allow`, which on iOS 16 makes the helper arm
   developer mode after installing; arming fails here and it returns 183 although the app was
   installed and registered (`logs/224550-run.log`). Signing LocProbe with
   application-identifier / team-identifier / keychain-access-groups and no get-task-allow
   (src/locprobe.entitlements, tools/ipa.py keeps the mode) makes the same install return 0.

The store's UI route (TrollStore's own window, + or a file URL) is still untested on the phone:
`uiopen --bundleid com.opa334.TrollStore` did not reach launchd in that run even after
`uicache -a` (no `Successfully spawned TrollStore`), while the UI itself is known to render
(logs/app5-2.png..-4.png). The helper it calls is the same one proven here. The scene is
`rc.jobs.tstore`, the image `root-tstore.apfs` `ae237c1c`.

### Two things the user saw at the end of a run (2026-11-17, night)

- **A frozen screen after a run is the loader, not a hang.** When `XL_SECS` runs out the kernel
  returns to the loader, which shows the last framebuffer image until something reboots it; touch
  and Wi-Fi are then dead because no OS is running.  `xl reboot` brings Android back
  (`/tmp/vk/backandroid.sh` does the PATH-and-wait dance).  Worth saying to anyone watching the
  phone: a still screen after a run is expected.
- **The ad-hoc scenes did not join Wi-Fi by themselves.** The 23:14 TrollStore run logged
  `lx: the network is NOT joined` and `net_test: wifi: configd has no network service for en0`:
  a fresh RAM boot does not associate, and only scenes that call `net_test joinfile /redwood/fw/network`
  (as rc.jobs.hweather does) get a network.  So the map app's 0 search results at 21:17 were first
  of all "no network", before the DNS poisoning question.  rc.jobs.mapapp now starts the join in
  the background before anything else (image `root-mapapp.apfs` `d6562049`).

The touch screen itself was up in that same boot: `hakios: input: touch screen service 0x1538e60c0`,
`backboardd [TouchEvents] init: <BKDirectTouchState: ... touchScreen; builtin>` and
`hakios: input: touch detection mode 0xff`; the scene's injected taps reached SpringBoard.

### The full-driver image with Wi-Fi and Bluetooth, and the store on top (2026-11-17, late)

The lesson the user made explicit: test images must carry every driver.  `devices/thyme/build-root.sh`
(SCENE=wirelessstore) builds the canonical image -- REST=1, BLUETOOTH=1, ADSP=1, GL=1, and
check-root.py refuses an image that lost any of them (12,028 blocks, `a6725f3e`; the scene later
gained the direct helper install, `b192f3a8`).  Verified on the phone (`logs/235104-run.log`,
`logs/full-1.png`, `logs/full-4.png`):

- Wi-Fi **joined**: "the network is joined ... 12,842 frames sent ... 53,408 received"; DHCP gave
  en0 an address; `net_test resolve www.baidu.com` answered 183.2.172.177 in 0.02 s and TCP 443
  connected in 0.05 s; Safari rendered apple.com's "Success" page (`logs/full-1.png`).
- Bluetooth: Settings > Bluetooth is on and the phone is "discoverable as HakiOS Mi 10S"
  (`logs/full-4.png`); the QCA launch set and the H4 transport are the canonical ones.
- The store: TrollStore's own MCM container was seeded (mcm_test copied the app and ldid in,
  `logs/235104-run.log` line 8955 ff.).  The icon tap + file-URL hand-off did **not** open
  TrollStore that boot (no `Successfully spawned TrollStore`), so the scene now installs the IPA
  through the helper directly -- the route proven to return 0 (`logs/225723-run.log`).
- The home screen before any app was opened is captured at +30 s of the scene, so the wallpaper
  question (black at 23:32 on an ad-hoc image) can be answered from the same run.

Store choice, for the record: TrollStore installs local IPA files (no download source of its own);
Sileo is the package manager that downloads from repositories over the now-working Wi-Fi
(apt fetched the Procursus index in earlier sessions).  AltStore Classic and SideStore need Apple
ID signing with a computer-side server / pairing file and a MobileDevice stack this port does not
carry, so they are not usable here.

### The wallpaper: present on the full-driver image, black frames on the test images (2026-11-18, night)

The user saw a black home screen at 23:xx and thought the wallpaper had gone.  The first reading
(black frames at 21:27 and 23:32, colour at 09:08/17:05) suggested a time-of-day effect; the
full-driver run at 00:15 settled it the other way:

- `logs/full2-1.png` (00:15, the canonical image with REST/BLUETOOTH/ADSP/GL) shows the bright
  blue/green/yellow wallpaper, with the Wi-Fi icon and the newly installed LocProbe icon;
- `logs/ts-3.png` (21:27) and `logs/map8-2.png` (23:32), both from ad-hoc test images, show the
  same icon layout on black.

So it is not the clock: the wallpaper renders on the canonical image at midnight too.  The black
frames belong to the ad-hoc test images (or to the moment a scene's uicache/SpringBoard restart
caught the home screen before the wallpaper redrew).  The canonical image is the reference from
now on; the ad-hoc images are retired.  (`root-wirelessstore.apfs` `b192f3a8`, `logs/001330-run.log`.)

### The full-driver image: Wi-Fi, Bluetooth and the store in one boot (2026-11-18, 00:15)

`root-wirelessstore.apfs` `b192f3a8` (build-root.sh: REST/BLUETOOTH/ADSP/GL, check-root passed),
run `logs/001330-run.log`, screenshots `logs/full2-1.png`..`-5.png`:

- **Wi-Fi**: joined the network `1701`; "the network is joined ... 17,944 frames sent, 61,221
  received"; `net_test resolve www.baidu.com` 0.01 s, TCP 443 connected 0.04 s.  Settings > Wi-Fi
  shows the checkmark, the network list and the status-bar icon (`logs/full2-3.png`).
- **Bluetooth**: 146 `UMI BT` lines with the canonical QCA launch set; Settings > Bluetooth was
  on and the phone discoverable as "HakiOS Mi 10S" (`logs/full-4.png` of the 23:51 run; same
  image family).
- **Home screen / wallpaper**: `logs/full2-1.png` at 00:15 shows the bright wallpaper -- see the
  correction above; the black frames were the ad-hoc images.
- **The store**: `mcm_test` seeded TrollStore's own container with ldid (copied to
  `.../TrollStore.app/ldid`), `trollstorehelper install custom` returned 0 (its ldid runs exited
  0), the app landed in `/private/var/containers/Bundle/Application/149ABD62-.../LocProbe.app`,
  and LocProbe launched twice (pid 250, 253; `Successfully spawned LocProbe`).  The TrollStore UI
  also opened (`Successfully spawned TrollStore[288]`) and lists the app
  ("LocProbe 1.0 • com.hakios.locprobe"); on launch it began its own "Installing ldid" download
  (`logs/full2-5.png`) -- a real network download through the store, on the Wi-Fi this boot joined.
- Safari opened apple.com's "Success" page on the same link (`logs/full-1.png` of the 23:51 run;
  this boot's `full2-2.png` is HakiOS LocProbe in front, `full2-3.png` the Wi-Fi pane).

### A store front-end for TrollStore: TrollApps on the phone (2026-11-18, night)

The ask: a store that behaves like an app store -- search, browse, download, install, update --
preferring an existing one over writing a new one, with Chinese apps as the goal.  Survey first:

- **TrollApps 2.3** (TheResonanceTeam/TrollApps, 648 stars, `org.resonance.store`): a front-end
  for TrollStore.  Repositories are AltStore-style JSON; it downloads an IPA and hands it to
  TrollStore through `apple-magnifier://install?url=` (TrollStore's scheme, declared in its
  Info.plist).  Needs TrollStore with URL schemes enabled.  English UI.
- **ESign (轻松签)**: a certificate-based signer with its own catalog; needs a signing certificate
  (paid/enterprise, or third-party "free" certificates) and its shared catalog is built on
  decrypted commercial apps.  Its site answered HTTP 522 when probed.  Not chosen.
- **Feather**: certificate-based signer as well; the GitHub repo under the expected name was not
  found (moved/private).  Not chosen.
- No store carries a lawful unencrypted WeChat/QQ/Bilibili/Douyin/Xiaohongshu/Alipay: App Store
  builds are FairPlay-encrypted and every store's Chinese catalog is decrypted/pirated copies
  (often modified).  That is excluded by the task's own rule 6 and by this project's policy; the
  store can therefore be a real ecosystem for legal/open-source apps but not for those six.

**On the phone** (`logs/012656-run.log`, image `root-storeapps.apfs` `a299b51c`/`284b373f`,
screenshots `logs/store-1.png`..`-4.png`):

- The 5.5 MB TrollApps IPA could not ride in the root image: with it the payload upload failed
  ("xl: upload failed"); the payload tops out near 50 MB.  A new `userland/src/hk_download.m`
  (`hk_download URL PATH`, NSURLSession) fetched it from the host on the LAN instead --
  `5549018 bytes (http 200)` -- which also proves arbitrary downloads over the phone's Wi-Fi.
- TrollStore's helper installed TrollApps into its own MCM container
  (`00B49947-.../TrollApps.app`, `trollstorehelper returning 0`) and the app launched
  (`Successfully spawned TrollApps[249]`); its first-run sheet and then the **Featured catalog**
  render, with Resonance Team and haxi0's source listed and GET buttons (`logs/store-2.png`,
  `-3.png`).
- The **sources themselves are reachable from this network**: `hk_download
  https://raw.githubusercontent.com/TheResonanceTeam/.default-sources/main/ResonanceTeamRepo.json`
  returned 8170 bytes (http 200), so the GitHub-hosted catalogs load here.
- The one-tap install was not yet observed end to end: a tap on TrollLock's GET left the list
  unchanged and no third container appeared (the two are TrollStore and TrollApps).  That is the
  next test -- either a detail sheet needs a confirming tap, or the scheme hand-off needs the
  TrollStore URL-scheme setting confirmed; the helper route underneath is proven.

Honest limit, stated for the record: this makes a store *work*, not the Chinese app ecosystem.
The six target apps have no lawful unencrypted IPA; we will not use cracked or modified copies of
them, and the stock App Store cannot run them here (FairPlay).

### TrollApps' install path, read from its source (2026-11-18, after midnight)

The earlier note said TrollApps hands IPAs to TrollStore through `apple-magnifier://`; that is
only how it updates itself from a source that supplies such a URL.  The install path
(`Managers/InstallManager.swift`) is different and simpler:

1. GET (`Views/Supplements/DynamicButton.swift`, `getButton`) only adds a `QueueItem`; the button
   turns red **QUEUED** (seen in `logs/store-3.png` of the 01:53 run), and a bottom sheet appears.
2. In that sheet, **Confirm** downloads every queued IPA to `/var/mobile/.TrollApps/tmp/<uuid>.ipa`
   (`downloadIPA`); when the downloads finish it either shows **Install Apps** or, with
   `skipInstallPrompt`, goes straight on.
3. `InstallIPA` finds TrollStore with `SBFApplication(applicationBundleIdentifier:
   "com.opa334.TrollStore")` and spawns **its trollstorehelper** as root (`spawnRoot`, the persona
   spawn) with `install <ipa>` -- the same helper and arguments this project's scenes run
   directly, only through a root persona instead of a root scene.

So the remaining work for the one-tap test is UI automation inside its own sheet (swipe the sheet
up, Confirm, then Install Apps), not a different install mechanism.  `SBFApplication` and the
persona spawn are the two things that may still fail on this port; both have fallbacks in our
scene-side path (mcm_test + helper as root).

By the way: TrollApps ships `zh-Hans.lproj` and `zh-Hant.lproj`, so with the system language set
to Chinese its interface is Chinese -- the English screenshots are only because the last boots
ran in English.

### The queue sheet measured off the source, and a source list without a race (2026-11-18)

The 02:53 run (`logs/025341-run.log`) never reached the taps: `hk_download` waited out the 60 s
GitHub timeout and the 240 s budget was spent before the `trollapps://add` deliveries, so the
screen still showed the bundled GitHub sources (`logs/store-2.png`..`-4.png`, no HakiOS Local,
`/var/mobile/.TrollApps` absent).  Four facts from the app's source make the next scene
deterministic instead of a race:

- **Where the source list lives.**  `RepositoryManager` declares
  `@AppStorage("repos") var RepoList: [String]`, and the app adds
  `extension Array: RawRepresentable where Element: Codable`: the UserDefaults value is one
  **JSON string** (`["http://..."]`), not an NSArray.  A scene can therefore seed
  `~/Library/Preferences/org.resonance.store.plist` (501:501, 0644) before first launch; the
  app's first fetch then has the LAN source and Featured's first row is known.  This also
  replaces the double `trollapps://add` delivery, which worked only when the Sources view
  happened to be mounted when the second URL arrived.
- **No second prompt.**  `confirm` runs `downloadIPA` per item and then, when
  `userSettings.skipInstallPrompt` is true (its default), calls `processInstalls` directly.
  **Install Apps** appears only with the setting off.  So the whole flow is GET -> Confirm ->
  download -> `spawnRoot(trollstorehelper, ["install", ipa])` -> the app removes the IPA.
- **The buttons' place.**  The queue is a BottomSheet: `absoluteBottom(165)` for two seconds
  after GET, then `absoluteBottom(105)`; the user drags it to `relativeTop(0.975)`.  The
  content is laid out bottom-up with `.padding(.bottom, canClose ? 95 : 40)` (canClose is true
  before Confirm, false after), two `LongButtonStyle` buttons 50 pt tall, 350 pt wide:
  Confirm first, Clear Queue second.  On a 780 pt screen that puts Confirm near y≈0.73 and
  Clear Queue near y≈0.80, i.e. **taps must stay above ~0.78** or they clear the queue; the
  0.87 used in the 02:43 scene was in the sheet's empty bottom padding.
- **The app's own language.**  `UserSettings.lang` is fed to `.environment(\.locale)` at launch
  (`TrollAppsApp`), and Settings offers `zh-hans`/`zh-hant`; seeding `lang = zh-hans` gives the
  Chinese interface without changing the system language.  It takes effect on the next launch,
  which the scene's launch is.

The prepared scene (`rc.jobs.storeapps`, image `root-storeapps.apfs` `1675f323`) seeds the LAN
source and the language, drops the GitHub probe, taps GET, drags the sheet up, taps 0.71 and
0.75 (Confirm is the blue button above Clear Queue), then lists the queue directory and the
MCM containers, runs `uicache -a` and opens `com.hakios.locprobe` to show the store-installed
app runs.  Its result belongs in the next paragraph once observed.

### The first half, observed on the phone (2026-11-18, `logs/033530-run.log`)

The scene `rc.jobs.storeapps` (image `root-storeapps.apfs` `8c6b53d1` was the next build; the
run below used `1675f323`) proved the seeding and the language on the device:

- `/var/mobile/.TrollApps` did not exist before the app ran, and **Featured already showed the
  LAN source**: `HakiOS Local`, `LocProbe 1.0`, the location icon and a GET button, with no
  `trollapps://add` and no GitHub fetch (`logs/sb-04.png`).  So the `repos` JSON string in
  `~/Library/Preferences/org.resonance.store.plist` is read the way the source implies.
- The UI is **Chinese** (`推荐`/`软件源`/`应用程序`/`搜索`, `安装`) from `lang = zh-hans` alone;
  the system language was not touched.
- The GET tap landed (`logs/sb-06.png`): the button turned pink **QUEUED** and the queue sheet
  appeared at its 105 pt bottom position -- about ten seconds after the tap, the app's first
  layout pass on this port is slow.
- The sheet did **not** expand: the 03:35 scene still used a swipe starting at y 0.903, and the
  sheet's bottom 83 pt are hidden by the tab bar (the sheet ignores the bottom safe area, the
  tab bar is drawn over it), so the drag began on the tab bar, not the sheet.  The source says
  why: BottomSheet attaches its drag gesture to the header and the sheet background, and the
  drag indicator at y≈0.885 is itself a **Button**
  (`BottomSheetView+DragIndicatorAction.swift`): a tap switches 105 pt -> `.relativeTop(0.975)`,
  full height.  The scene now taps that point, keeps a slow drag as a fallback, and taps
  Confirm at 0.75/0.78/0.76 -- inside the button's 0.740..0.804 span with the sheet's 95 pt
  bottom padding, and above Clear Queue's 0.814..0.878.  That scene's result is next.

### The store installs an app through its own UI (2026-11-18, `logs/090617-run.log`)

The 09:06 run went past the queue: `/var/mobile/.TrollApps/tmp/` was created (the app deletes
the IPA after the helper returns, so both listings show it empty), a **third MCM container**
`/private/var/containers/Bundle/Application/2ECB4BE0-F8C6-48A4-93F0-C2BB298E44D1/LocProbe.app`
appeared at 09:09, and LocProbe was launched **twice**: `Successfully spawned LocProbe[269]`
at 09:09:10 (right after the install; SpringBoard took it down with `0xdeadfa11` at 09:09:17)
and `LocProbe[285]` at 09:09:56 from the scene's own `uiopen`; `locationd` shows the
CoreLocation activity of both.  No scene command ran TrollStore's helper for LocProbe in this
boot (the only direct `trollstorehelper` run is TrollApps' own custom install at 09:07:53), so
the install came from **TrollApps' `spawnRoot` path** -- the persona 99/uid 0 spawn the kernel
patch supports.  The expanded sheet is photographed in `logs/sb-07.png`: `1 Apps Queued`,
`LocProbe -- Queued Install`, **Confirm** at y 0.832..0.881 and **Clear Queue** below it.

Two cautions belong with that PASS:

- Which touch pressed Confirm is not settled.  `logs/sb-07.png` (09:09:06) shows the sheet
  already expanded with the item downloaded but still queued, and the log has six touch pairs
  between 09:09:00 and 09:09:08 that this scene did not send (its next tap was 0.5/0.885 at
  09:09:09.79, and that point is the *gap* between Confirm and Clear Queue in the expanded
  sheet).  Someone or something pressed Confirm first -- a person tapping the phone, or touch
  noise while the sheet animated; the log cannot tell.  The 09:2x rerun taps Confirm at 0.85
  (the measured centre) and is the clean automation check.
- `persona_spawn_test` failed before its spawn: as root it called `setuid(501)` first and then
  `setgid(501)`, which answers EPERM once the process is no longer root.  It now calls
  `setgroups(0, NULL)`, `setgid`, `setuid` in that order (image `92b830cb`).

**The 09:20 rerun** (`logs/092007-run.log`) cleared the persona question and measured the UI
lag that broke the automation: `persona_spawn_test` prints `running as uid 501`,
`attributes set: 0`, `posix_spawn ok (0)`, `child exit 0` -- with persona 99/uid 0 and again
with the control -- so the kernel does not refuse the spawn TrollApps uses.  The queue,
however, stayed **QUEUED** and `/var/mobile/.TrollApps` never appeared: the GET tap at
09:22:48.9 produced the QUEUED button only at 09:23:02, so the scene's sheet tap at
~09:23:00.9 came before the sheet existed, and the Confirm taps at 09:23:05/08 landed on the
catalog.  The scene now seeds `appVersion = 2.3` (the key the changelog's own Dismiss writes)
so `showChangelog()` stays quiet and the 15-25 s of sheet-dismiss taps are gone, and it
waits 22 s after GET and 8 s after the sheet tap (image `d6193c35`); its run is next.

**The 09:31 run** (`logs/093151-run.log`) kept the queue at QUEUED for a reason one level
down: the sheet was up (`logs/sb-06.png`: the indicator pill at y≈0.873) but the scene tapped
0.885, the edge of the indicator's 20 pt Button, and missed; the Confirm taps then hit the
catalog.  Across the three runs the pill's centre measured 0.873..0.885 (the 105 pt sheet's
top at 0.865 plus 7.5 pt padding), which is exactly the 20 pt hit area's width, so a single
fixed point is not reliable.  The scene now taps **0.873 and 0.878 three seconds apart**: the
first expands the sheet, and the second, once it is up, is inside Confirm's 0.832..0.881
(image `c6b2a9ca`); its run is next.

**The 09:43 run** tapped the indicator at its measured centre 0.873 and still nothing moved:
the Button's label Capsule carries the sheet's own DragGesture
(`BottomSheetView+HelperViews.swift`), and that gesture consumes the tap while a
zero-translation drag stays put -- so the indicator is not a tap target at all.  The sheet
switches only on a real drag; the 09:06 run's 600 ms drag over 0.60 was stolen by a system
gesture, so the scene now drags 500 ms over about 90 pt (0.873 -> 0.76) and keeps the
Confirm taps at 0.85 (image `439cfbd3`).

**The 09:53 run** dragged 0.873 -> 0.76 (about 110 pt) and the sheet snapped back: it has
only two positions, 105 pt and full height (760 pt), and the release was still nearer to
105.  A drag must cross the midpoint (~432 pt from the bottom, i.e. the finger past y≈0.45),
so the scene drags 900 ms to 0.42.  The same run also exposed the Confirm geometry: the
0.832..0.881 measured in `logs/sb-07.png` is the layout **after** Confirm, when canClose is
false and the bottom padding is 40 pt; **before** the press the padding is 95 pt, Confirm
spans 0.740..0.804, and 0.85 is inside **Clear Queue** (0.814..0.878) -- a tap there would
wipe the queue.  The scene now taps 0.77 and 0.79 (image `b2d58967`).

**The 10:03 run** dragged 900 ms to 0.42 (past the midpoint) and the sheet still did not
move: the contact was delivered cleanly (no system steal), so the gesture never reached the
sheet -- the indicator's capsule sits inside a Button, and the Button's tracking swallowed
the pan.  The scene now begins the drag on the sheet's background beside the indicator
(x 0.9 and 0.1 at y 0.872), where BottomSheet's own dragGesture is attached
(image `9fbe74fb`).

### The store installs an app, automated from GET to launch (2026-11-18, `logs/101423-run.log`)

The 10:14 run is the clean one: no stray touches, every step the scene's own.

- GET at 0.86/0.273 turned the button **QUEUED** (the same place earlier runs show it).
- Two drags from the sheet's background -- x 0.9 and x 0.1 at y 0.872, 900 ms up to 0.42 --
  expanded the sheet: `logs/sb-08.png` (collapsed) to `sb-09.png` (full height, `1 Apps
  Queued`, LocProbe, **Confirm** at 0.742..0.810 and Clear Queue at 0.820..0.870).
- The taps at **0.77 and 0.79** are inside Confirm; the press downloaded the IPA:
  `/var/mobile/.TrollApps/tmp/` exists (owned by mobile) and is empty again because the app
  deletes the IPA after the helper returns.
- The helper installed it: a third container,
  `/private/var/containers/Bundle/Application/3CCE02B0-DB16-4D0A-9F8C-31812485BE65/LocProbe.app`,
  created at 10:17.
- The scene's `uiopen` then launched it: `Successfully spawned LocProbe[280]` at 10:18:02.
- `persona_spawn_test` answered as uid 501 in the same boot: `attributes set: 0`,
  `posix_spawn ok (0)`, `child exit 0`.

So the one-tap path is verified end to end on the device: a source list on the LAN, GET,
queue, download, TrollApps' root-persona `spawnRoot` of TrollStore's helper, install into an
MCM container, launch.  What stays outside this PASS is the ecosystem: the sources carry
lawful IPAs only, and the six Chinese target apps have no such IPA (see the honest limit
above).  The update path (1.0 installed, 1.1 in the source) is the next queue exercise; its
assets are already served.

**Update-path assets, ready for after the install.**  An UPDATE needs an older version
installed and a newer one in the source, so a 1.1 build of LocProbe (same binary,
`CFBundleShortVersionString` 1.1) is packed and served beside the 1.0 IPA:
`LocProbe-1.1.ipa` (9646 bytes, `Payload/LocProbe.app/...`) and `repo-1.1.json`, both in
the LAN server's root.  A later scene only has to point the seeded `repos` at
`repo-1.1.json`; TrollApps compares the installed 1.0 with 1.1 and shows UPDATE, whose
queue/Confirm path is the same one this run exercises.

### The store updates an installed app, 1.0 -> 1.1 (2026-11-18, `logs/103043-run.log`)

rc.jobs.storeupdate installs LocProbe 1.0 through TrollStore's helper first, then launches
TrollApps against repo-1.1.json.  `logs/sb-05.png` shows the row as **LocProbe 1.1** with the
button **更新 (UPDATE)** -- the app read the helper-installed 1.0 and the source's 1.1.  The
scene tapped that button, dragged the queue sheet up from its background as in the install
run, and tapped Confirm at 0.77/0.79.  The scene's own probe then read the bundle's
`CFBundleShortVersionString` out of its MCM container:

- before the queue: `AB8CE0AB-B57A-4699-BD8D-ED367D57C018/LocProbe.app` -> **1.0**
- after the queue: the same container -> **1.1** (and again after `uiopen`)

with `/var/mobile/.TrollApps/tmp/` created and emptied, and `Successfully spawned
LocProbe[290]` at 10:34:32.  So the store updates in place (the helper reuses the existing
MCM container for the same bundle id), and the update is the install path plus the version
check -- exactly what the source said.  Image `root-storeupdate.apfs` `0e5131b3`.

### A real third-party app through the store: the open-source Zhihu client (2026-11-18, `logs/111826-run.log`)

`huamurui/zhihu-minus-minus` 0.9.10 is a lightweight open-source Zhihu client whose developer
publishes an unsigned IPA in the GitHub release.  Verified on the build machine before the
run: 14,833,319 bytes, SHA256 matches the release's `SHA256SUMS.txt`
(`8884c6f5...b43910c8`), `Payload/app.app`, `com.huamu013.ZhihuMinusMinus`, arm64,
`MinimumOSVersion` 16.4, `LC_ENCRYPTION_INFO` **cryptid 0** -- unencrypted, so TrollStore's
helper can carry it.

The store ran the whole path (image `root-storezhihu.apfs` `155da2ca`, built with DNS=1):

- the queue downloaded the **full 14,833,319-byte IPA** -- the early listing caught
  `/var/mobile/.TrollApps/tmp/29A509AE-...ipa` on disk mid-flow;
- the helper installed it: container
  `A49090D2-C722-4815-A1A2-48B00BF4597C` at 11:21;
- the scene opened it: `Successfully spawned app[275]` at 11:22:05, scene foreground at
  11:22:06, and `logs/sb-16.png` shows the **live Zhihu feed** (关注/推荐/热榜/日报, images,
  vote counts) fetched over the phone's Wi-Fi -- mDNSResponder is in this image because
  DNS=1 was built into it.

A side observation for the stability list: `com.apple.MobileFileIntegrity` exited with
**SIGABRT** at 11:21:27 during the install (pid 266, `EXC_CORPSE_NOTIFY` error 5); the
install and the launch completed anyway.  If it recurs, it needs a look -- it may be AMFI's
userland daemon unhappy about the helper's ldid signature on a 15 MB binary.

**Why the six mainstream apps have no lawful IPA.**  An App Store IPA carries FairPlay
encryption (`cryptid 1`), and this port has no FairPlay path: `fairplayd` starts but exits
because `com_apple_driver_FairPlayIOKitUserClient` does not exist, and TrollStore cannot
launch an encrypted image.  So an IPA of WeChat/QQ/Alipay/Douyin/Xiaohongshu/Bilibili from
any third-party site is necessarily a **decrypted** copy -- the FairPlay removal is what
makes it installable, regardless of whether the code inside was also changed.
`TouchFriend/BiliBiliMApp`'s release is the material proof: it ships
`bili-universal_8.89.0.app.zip`, the decrypted official Bilibili app, beside its tweak.
TestFlight builds cannot be extracted, and enterprise builds are not public.  This is a
source/DRM fact, not a store limit: the store installs any unencrypted arm64 IPA, as the
Zhihu client above shows.

### Compatibility matrix for real IPAs (2026-11-18)

| App | Channel and lawfulness | Encryption | Installed by | Launch / UI |
| --- | --- | --- | --- | --- |
| LocProbe (this project's test app) | built here | cryptid 0 | store GET -> Confirm | PASS, locationd activity |
| zhihu-minus-minus 0.9.10 (open-source Zhihu client) | developer's GitHub release, unsigned IPA + SHA256SUMS | cryptid 0 | store GET -> Confirm | PASS: live feed, images, 11:22:05 |
| Aidoku 0.9 (open-source manga reader) | developer's GitHub release, IPA | cryptid 0 | store GET -> Confirm | PASS: Library UI, 11:51:17, no crash (`logs/114738-run.log`, `logs/sb-16.png`) |
| PPSSPP 1.20.4 (open-source PSP emulator, GL/3D) | developer's GitHub release, IPA | cryptid 0 | store GET -> Confirm | install PASS (container `B9E8DEDE-...`); launch **FAIL**: SIGABRT 0.17 s after spawn, cause open (`logs/121955-run.log`) |
| iSH (Linux shell) | built from its open-source tree | n/a | helper | PASS earlier (keyboard, switcher) |
| TrollStore / TrollApps | official releases | n/a | helper | PASS |
| WeChat, QQ, Alipay, Douyin, Xiaohongshu, Bilibili | App Store only; third-party IPAs are decrypted copies; TestFlight not extractable; enterprise not public | cryptid 1 (App Store) | -- | not testable: the port has no FairPlay path; an encrypted image cannot start |

The six mainstream apps are therefore **"no lawful installable IPA"**, not "the store cannot
install them": the same store installs any unencrypted arm64 IPA, as the Zhihu row shows.

**How an App's own words were recovered.** An app launched by SpringBoard writes its stderr to
/dev/null, which is why several aborts here had no message. `execwrap` (`src/execwrap.c`) is a
small signed binary that stands in as the bundle's executable, moves the real one to
`<name>.real`, appends both file descriptors to `/private/var/tmp/execwrap.err` and execs the
real binary in the same pid and launch job; `userland/bin/hk-execwrap.sh` installs it and signs
it with the app's own entitlements (`ldid -e` then `ldid -S`). `rc.jobs.almapswrap` used it to
name Alpimaps' `eglInitialize()` failure; the same route can name PPSSPP's launchd abort when
its JIT question is revisited.

Sources, digests and licences for every app the store carries are kept in
`devices/thyme/ipas.md` (the LAN directory itself is not in the tree). Evidence shots:
`shotsb.sh` writes one `logs/sb-NN.png` per run and the next run overwrites it, so a frame
cited as evidence must be copied to a run-named file -- the weather app's frame of
2026-11-18 was lost that way and the run log is what survives.

Aidoku's run (`logs/114738-run.log`) added the second data point: container
`755A6701-AC57-4D54-BC58-828BA3729BD3` at 11:50, `Successfully spawned Aidoku[273]` at
11:51:17, `logs/sb-16.png` its Library screen -- no crash or exit line in the whole run.
Both real apps were installed by the store's own queue (GET -> drag -> Confirm), not by the
helper directly.

PPSSPP's run (`logs/121955-run.log`) is the first install that does not launch: the store
downloaded the full 31,124,277-byte IPA and the helper installed container
`B9E8DEDE-C96F-4136-9276-B01D8D3F933B` at 12:22, but `Successfully spawned PPSSPP[274]` at
12:23:52 was followed **0.17 s later by `exited due to SIGABRT`**.  No dyld or MESA line
reaches the kernel log (an application's stderr does not), and no GPU fault is recorded, so
the abort is before any rendering.  The same install repeated the `MobileFileIntegrity`
SIGABRT (pid 265) seen with Zhihu; AMFI's userland daemon aborts during these helper installs,
yet the install completes.

The missing-framework hypothesis is ruled out on the build machine: `tools/dyld-cache-image.py`'s
reader over `out/thyme-boot/dsc-arm64/dyld_shared_cache_arm64` lists 2710 images, and every
dylib PPSSPP links is among them -- GLKit, OpenAL, Metal, MetalKit, GameController, PhotosUI,
MediaPlayer, CoreMotion, UIKit, QuartzCore, OpenGLES, libc++, libiconv all present.  So the
abort is in the app's own start-up, not in dyld's lookup.

**The diagnostic run** (`logs/124616-run.log`) then named it: no crash report was written
(CrashReporter holds only Assistant/DiagnosticLogs/appstored), but the direct exec printed
`b SceneDelegate class was loaded!` and `ios/main.mm:668 I[SYSTEM]: JIT: ptrace() child spawn
trick`, and the kernel recorded the launchd abort with its call chain:
`userabort: PPSSPP[243] aborts, shared cache slide 0x20120000; called from: 0x1add684b8
0x1f0d38f54 0x1f0d29a4c 0x1a01f2014 0x1f0d38424 0x1f0d383d4 0x1adca7794 0x1adc7f2d0`.  With
the slide removed the frames resolve to `libsystem_c` (abort) <- `libc++abi` (four frames,
the uncaught-exception/`std::terminate` path) <- `libobjc` <- `libdispatch`.  The same binary
exec'd directly prints the JIT line and survives the five seconds the diagnostic gave it.
PPSSPP's JIT start-up -- its `ptrace()` child-spawn trick -- is what fails under launchd on
this port; the install and registration are fine (its icon is on the home screen,
`logs/sb-08.png`).  This is a launch-environment/JIT compatibility limit, not a packaging
one.

**amfid's own abort, seen beside these installs.**  `com.apple.MobileFileIntegrity` (amfid)
crashes with SIGABRT during some helper installs -- Zhihu's (`userabort: amfid[266] aborts`
at 11:21:27, `logs/111826-run.log`) and the PPSSPP diagnostic's (`amfid[237]` at 12:47:47,
`logs/124616-run.log`) -- but not Aidoku's.  The chain
`0x1add684b8 0x100bda3c0 0x100bddb78 0x100bddacc 0x1f0e48788 0x1f0e48b68 0x1adca7834 ...`
resolves to `libsystem_c`(abort) <- amfid's own binary (three frames) <- `libxpc` (an XPC
message handler) <- `libdispatch`: amfid aborts inside the signature-request handling it is
started for.  The installs complete and the apps launch anyway (TrollStore's CoreTrust bypass
does not wait for amfid), so this is a residual instability in the port's AMFI stand-in, not a
store blocker; it belongs in the stability list for a later look at the XPC request amfid is
given.

The binary itself (`/usr/libexec/amfid`, 139,280 bytes) is **stripped** -- `llvm-nm` finds
only `__mh_execute_header` -- so the static read stops at the shape: taking the first frame
(0x100bda3c0) as the return address after the `_abort` call at `__TEXT+0x23bc` puts amfid's
runtime base at 0x100bd8000 (page-aligned; the other abort sites do not admit one), and the
aborting code is the cold path of the function at `__TEXT+0x2114`: an `os_log` through
0x10000b2e4 and then `abort`, the usual "log a fatal condition" shape.  Reading which
condition needs dynamic tracing (a breakpoint or a Frida session), not more disassembly.

**The systrace run** (`logs/130120-run.log`, boot-arg `systrace=1`) ruled out a kernel
refusal: the facility was live -- 923 `systrace:` lines in the run, including other
processes' failing `sysctl`, `ioctl` and `csops_audittoken` calls -- and **not one came from
PPSSPP**.  So no syscall was refused; the C++ exception is the app's own response to
something in its start-up (the `JIT: ptrace() child spawn trick` line is where it announces
the attempt, not where the kernel says no).  A plain process does **not** get a Metal device: `MTLCreateSystemDefaultDevice()`
answers nil in root and uid 501 alike (`logs/202425-run.log`, 2026-11-18), so the app-side
suspect is not a Metal feature the shim lacks but the absence of any device outside the
injected window-server path; PPSSPP's own launchd abort may have the same root.  An
application's exception text is not collected under launchd on this port, so that is where
this thread rests until a device is published to apps.

**The port's own Metal plan puts the boundary in the same place.**  `docs/metal-plan.md`
says the Metal work targets the system's own clients (the window server draws through Metal
on the phone), and `docs/ru/metal.md` §9.4 argues that games are "a firm no": the shader
compiler reaches the 456 modules the system ships, not an arbitrary app's.  PPSSPP is in the
second category.  The store can install it; running it is a graphics-project milestone, not
an install or packaging fix.  The same applies to any 3D app or emulator brought in through
the store.

### The sources the store ships with, checked 2026-11-18

TrollApps' bundled default list is three GitHub JSONs.  **haxi0's source is entirely stale**:
it loads, but all six apps' download URLs (Jaility, TrollBoard, Derootifier, DirtyJIT,
TrollLock, KillMyOTA) answer 404 from GitHub -- the `TrollBoard-Reborn` repository itself is
gone.  The public-source run (`logs/133243-run.log`) shows the store following it faithfully:
GET queued, the download wrote the 404 body -- `Not Found`, **nine bytes** -- as
`/var/mobile/.TrollApps/tmp/090E351A-...ipa`, and no container or launch followed (the file's
nine bytes are the whole story).  That is source rot, not a store defect; TrollApps does not
check the HTTP status before handing the file to the helper, which is its own robustness
point.  The other two sources are alive: Resonance Team's (TrollApps 5,549,018 bytes,
TrollApps Legacy 3,697,409) and Bonnie's (tapgame prototype 18,248,245), each answering 200.

**The Bonnie source run** (`logs/134628-run.log`) did not get that far: with only
BonnieRepo_2.0.json seeded, the Featured page stayed **empty** -- `logs/sb-06.png` shows the
title and tab bar with no section -- so there was no GET to tap.  The same JSON parses on the
build machine and its app URL answers 200, and the Resonance Team source was seen loaded in
the daily run, so the store can fetch public sources; what failed here is the app's fetch or
decode of this one in that run, not the source's absence.  Observed, not explained yet: the
empty-Featured case deserves its own look if a public source is to be relied on.  The LAN
source remains the one that has installed everything (LocProbe, Zhihu client, Aidoku,
PPSSPP); the public-source install is still unproven with these two (haxi0's URLs are dead,
Bonnie's page came up empty).

**The Browse run** (`logs/140158-run.log`) then ruled out the featured-only explanation:
switching to the 搜索/BROWSE tab with the same source also listed **no apps at all**
(`logs/sb-11.png` is an empty BROWSE with only its search field), so the app's `ReposData`
never held Bonnie's repo.  The JSON is schema-complete for the app's decoders (name and
iconURL on the app, version/date/downloadURL on its one version) and the same source was
seen loaded in the 02:53 all-sources run, so the favourite explanation is the **network**:
raw.githubusercontent.com is intermittently unreachable from this Wi-Fi -- `hk_download` to
the same host timed out after 60 s with retransmits in the 02:09 run, while the app's own
fetch sometimes succeeds (the 02:53 and daily runs loaded the same Bonnie source).  The app's
URLSession has no visible retry, so a failed fetch leaves `ReposData` without the repo and
both Featured and Browse empty, which is what these two runs show; it is not a decode
failure.  The LAN source remains the only one that has installed everything.

### The store as a daily screen, on the phone (2026-11-18, `logs/110554-run.log`)

`rc.jobs.storedaily` seeds the three default sources plus the LAN one and leaves TrollApps in
front for the rest of the run.  `logs/sb-30.png` (11:11) shows all of them loaded --
**HakiOS Local**, **Resonance Team** (TrollApps itself as `打开`), **haxi0's Source**
(TrollBoard/Derootifier/TrollLock) -- in Chinese, after 4.5 minutes in the foreground with no
exit or crash (the app's launch at 11:07:41 is its only launchd line; both status dumps at
11:12/11:13 list the two containers).  Image `root-storedaily.apfs` `e38972a2`.  The GitHub
sources load over this network even though `hk_download` had timed out against the same host
in earlier runs; the app's own NSURLSession fetch is the one that matters here.

**The 600 s soak** (`logs/144733-run.log`, same image, `XL_SECS=600`, 14:47-14:57) kept
TrollApps in front from 14:49:22 to the end with **no exit or crash line at all**; the last
screenshot (`logs/sb-30.png`, 14:53) still shows HakiOS Local, Resonance Team and haxi0's
Source in Chinese, with **Bonnie's Repo missing again** -- the same intermittent
raw.githubusercontent fetch.  Battery went from 43% (Android) to 36% (kernel reading) and the
SoC sat at 36 C: ten minutes of an idle foreground store costs very little.  That is the
longest store observation so far.

### The curated daily source, and the ldid a clean build had been dropping (2026-11-18, `logs/161546-run.log`, `logs/163348-run.log`)

The daily store now has a catalog of its own.  `repo-hakios.json`, served from the LAN beside
the IPAs, carries the three lawfully published apps this port has already installed and
launched -- the open-source Zhihu client (0.9.10), Aidoku (0.9) and LocProbe (1.1) -- and
`org.resonance.store.hakios.plist` seeds `[HakiOS Local, Resonance Team]` instead of the
four-source list: the haxi0 source is gone upstream (every URL 404) and Bonnie's came back
empty over this network, so neither earns a row in a daily screen.  `logs/sb-30.png` (16:07,
image `root-storehakios.apfs` `63133b6e`) shows Featured drawing **HakiOS Local** (all three
rows with 安装) above **Resonance Team** (TrollApps, 打开) within the first minute; no GitHub
fetch stands between the launch and the LAN catalog.

That same rebuild exposed a quiet build break.  The 16:02 run (`root-storehakios.apfs`
`2caddec5`, `logs/160209-run.log`) installed TrollApps' helper but it returned **173** and the
store never appeared: `mcm_test`'s copy of `/redwood/share/tsldid/ldid` failed with Code 260,
file not found.  The ldid had been reaching images as an `EXTRA_SHARE` from `/tmp` in earlier
sessions, and `build.sh` starts with `rm -rf root`, so the first clean rebuild without that
switch dropped it; the helper spawns ldid from TrollStore's container bundle and refuses
before it applies CoreTrust without it.  The fix is part of the build now:
`userland/trollstore/tsldid/ldid` (the arm64 ldid **v2.1.5-procursus7**, SHA-256
`b6aa4018e7ef3df9ee3fee5889d162649719109521c5a2136ca98a81fccdf52a`) is copied to
`/redwood/share/tsldid/ldid` by `build.sh`, and `check-root.py` refuses an image that lacks
it or carries something other than a 64-bit Mach-O at that path.

The end-to-end run from the curated source: `logs/163348-run.log` (image
`root-storehakios2.apfs` `cea697d4`) taps 安装 on the first HakiOS Local row -- 0.85 0.265 --
walks the queue sheet exactly as `rc.jobs.storezhihu` proved (drag it open from either side,
Confirm), the app downloads the 14.8 MB IPA into `/var/mobile/.TrollApps/tmp`, the helper
installs it (`created app container`, `returning 0`), and `uiopen` opens
`com.huamu013.ZhihuMinusMinus` at 16:37:35.  `logs/sb-30.png` (16:38) is the client's 推荐
feed with real cards and counters, "< TrollApps" still in its status area.  amfid aborted
once in that window (2202 s, the known stripped-binary SIGABRT) and did not stop the install.
Scene: `rc.jobs.storehakios2`.

### An existing weather app through the store (2026-11-18, `logs/174409-run.log`)

The owner's rule -- existing apps only, no self-written replacements -- is now the tested
path for weather.  贵州天气 (`cn.guizhou.weathergz` 3.2) is the developer's own GitHub
release (`wangvizi/WeatherGZ` v3.2, SHA-256
`e3777b8288f165e8843437087177b5187b971c373e22ea0bf8ba163000e71c25`): a 731 KB arm64 IPA,
**cryptid 0** (no FairPlay), minOS 16.0, signed, carried on the LAN beside the rest and
listed first in the HakiOS Local source.  `rc.jobs.storeweather` installs it the way a
person would -- 安装 on its row (0.85 0.265), the queue sheet opened from either side,
Confirm -- the app downloads the IPA and hands it to TrollStore's helper (`created app
container`, `returning 0`), and `uiopen` opens it (container
`9C771D6E-.../WeatherGZ.app`, pid 273), with no crash and no amfid abort in the run.
`logs/sb-29.png` (17:49) is the app's own weather screen: **安顺·西秀区, 阴, 8°, 78 %,
东北风 2 级** with its 大风蓝色预警 cards, "< TrollApps" as the back link, and the
system's location consent alert over it -- the app asked for a fix and the system asked
the person, which is the normal alert.  Image `root-storeweather.apfs` `6528bd97`.

For maps, Alpimaps (`akylas.alpi.maps`, MIT; the developer's GitHub iOS alpha 3.0.4 build
242, 62,303,169 bytes, SHA-256
`566c5f522d92c4e6ef8df053ae53aa6f5d436f01f9290bbcb8daa1a9232c4f94`, cryptid 0, arm64,
minOS 12) is staged as the source's second row; `rc.jobs.storemaps` installs and opens it
(image `7d4f20be`) installed it on the phone (container `39587D6C-…`) and its launch aborted at `eglInitialize()` -- the Alpimaps row and the wrapper paragraph above carry the evidence and the cause.

**The kernel side of that spawn is already there.**  `xnu/hakios.patch` rewrites
`spawn_validate_persona()` and `spawn_persona_adopt()`: the entitlement and the persona
lookup are gone, and when no persona exists the requested credential (persona 99, uid/gid 0,
the OVERRIDE flag) is applied to the child directly.  The patch says why -- Sileo's APTWrapper
becomes root exactly this way for apt and dpkg -- so TrollApps' `spawnRoot` asks for
precisely the path this port already walks.  The other unknown, `SBFApplication(...)`, needs
no probe of its own: TrollApps launched and drew its catalog, and an Objective-C class
reference such as `_OBJC_CLASS_$_SBFApplication` is bound by dyld at launch.  What remains is
the UI reaching InstallIPA; that is the run `persona_spawn_test` rides along with.

### The loader's fastboot endpoint, wedged by an interrupted upload (2026-11-18 ~03:50)

After the 03:35 run the phone was still in the loader, but `xl state` answered "android":
`emulator-5554` (the QEMU bench emulator) was attached, and xl's `state` falls through to
`adb get-state` when the fastboot answers are slow.  The 2.5 GB `xl ramdisk` upload was in
fact running normally; it was killed on that misreading, and the loader's fastboot endpoint
then enumerated as `xnuloader` but answered nothing:

- `fastboot getvar product` and `fastboot reboot` both fail with
  `AdbWriteEndpointSync failed: ... (121)`;
- `xl enter` reports `loader did not come up on USB (state: fastboot)`;
- `xl reboot` hangs in the same place.

Nothing on this side can reach that endpoint; a USB replug or a power cycle is the way back.
Lessons for the next sessions: never interrupt `xl ramdisk` (FB_TIMEOUT is 1800 s for a
reason), and keep `XL_ADB_SERIAL=5290780a` in every `xl state` call while the emulator is
attached.  Image `root-storeapps.apfs` `4593d249` carries the pill-tap scene and a new
`persona_spawn_test` (drops to 501 and spawns `id` through persona 99 uid/gid 0, then the
control without it); its run waits for the phone to come back.

### The sileolists run: no return, no log, and the loader wedged again (2026-11-18, 23:03)

The one-line test from the Sileo row -- `ln -sfn /var/jb/var/lib/apt/sileolists /sileolists`
before the GET, plus an A/B/C probe that runs the same relative-lists apt query from
`/var/jb/var/lib/apt` and from `/` without and with the link -- was built into
`rc.jobs.sileolists` (`root-sileolists.apfs` `f9533b77`; `check-root.py` found GL, Bluetooth,
audio and camera in it), with `userland/system.apfs` `3b539ee0` as the resident image and
`XL_SECS=450`.  `xl run` printed `kernel=4ce8d7ba31d1 loader=6a57fb6bb147`.

**The kernel did not return.**  At the deadline the loader reported
`no answer — the kernel hung; waiting for the watchdog`, and it stayed in its bootloader.
Nothing was written under `logs/` at all (the newest file there is `204708-run.log`, 20:53);
`xl` named `logs/230355-run.log.live`, which is the **5 November** file of the same name, so
this run left no console output -- the empty-log state already recorded for `xl run`.

**The owner's screen, during the run:** the picture refreshed as garbage, flickered, sat on a
white background without the wallpaper.  Whether that is the hung kernel's framebuffer, a GPU
wedge of the kind fixed by the `metal_gmem=1` change, or the window server failing to draw is
not decidable without a log; it is recorded as observed, not diagnosed.

**The loader endpoint is wedged again.**  `fastboot getvar product` answers
`FAILED (AdbWriteEndpointSync failed ... (121))` and `fastboot devices` shows
`????????????	fastboot`; `adb` sees nothing.  This is the same state as the interrupted
`xl ramdisk` on 2026-11-18 03:50 and only a USB replug or a power cycle clears it.

**Before any rerun, two inputs must be reconciled with `ram-baseline.conf`**: that file
records `BOOT_IMAGE_SHA256=7c5a2cd8` and `SYSTEM_SHA256=4f0f3f8b`, while this run used
`devices/thyme/prebuilt/xnu-usb.img` `213ffaae` (17 November 14:20) and
`userland/system.apfs` `3b539ee0` (17 November 16:26, hard-linked as
`devices/thyme/prebuilt/system.apfs`).  Both were rebuilt on 17 November and the baseline
was not updated with them; `xl enter` could not repack the loader because `u-boot/` is not
present (`mkubootimg.py: no u-boot.bin for redwood`), so the prebuilt image was used
(`XL_BOOT_IMAGE`).  The next run either rebuilds `u-boot/` and repacks, or rebaselines these
two hashes with a verified run -- not both silently.

One environment note for the next session: this WSL had no `adb` or `fastboot` on `PATH`
(the Windows SDK's `adb.exe`/`fastboot.exe` only), so `/home/darling/bin/{adb,fastboot,xl}`
are wrappers that exec the `.exe`s; `xl` drives the phone again through them.

### The plain desktop boot does not return either (2026-11-18, 23:34)

With the phone recovered, a boot that runs no scene work was tried with the same loader and
system image: `root-wirelessfull.apfs` `d9119f52` (fresh `build-root.sh`; `check-root.py`
found GL, Bluetooth, audio and camera), `XL_CMDLINE=` empty, `XL_SECS=180`, system
`userland/system.apfs` `3b539ee0`, loader `6a57fb6bb147`.  The result is the same message:

    xl: no answer — the kernel hung; waiting for the watchdog to reset the phone

and again **nothing was written under `logs/`**.  So the hang is not the Sileo scene and not
its root image; it happens on a plain desktop boot.

**The repack path was tried and is not equivalent.**  `xl enter` could not repack because
`u-boot/` was absent, so `uefi/u-boot/apply.sh redwood` and `build.sh redwood` were run
(U-Boot `v2026.07`, `aarch64-linux-gnu-`): the build produced a **1,541,120-byte**
`u-boot.bin`.  The blob inside the working prebuilt image is **not U-Boot at all**: the
3,216,768 bytes its gzip stream yields carry `_FVH` (an EDK2 firmware volume) and no
`U-Boot 20...` version string, and `devices/thyme/build-loader.sh` packs it with
`mkxnuimg.py -d thyme --fd-base 0x9fc00000 --fd-size 0x300000 --payload-base 0xf0000000`,
the Silicium layout.  `xl enter` fell back to the U-Boot packer only because neither
`Mu-Silicium/Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` nor
`devices/redwood/prebuilt/SILICIUM_UEFI.fd` exists (only `umiPkg` is built), so the U-Boot
images it made (`961e038f`, then `e380248f`) were the wrong firmware for this loader and did
not come up at all (`xl: loader did not come up on USB (state: fastboot)`).  Building U-Boot
was a wrong turn; the way to make `xl enter` repack for thyme is to restore the Silicium
`redwoodPkg` firmware, not to replace it with U-Boot.

**Recovery after the second hang, and the slot that was marked unbootable:** the loader
endpoint wedged exactly as before
(`????????????`, `AdbWriteEndpointSync ... (121)`); a replug brought the phone back, but after
the next `xl enter` attempt `fastboot reboot` left it in fastboot and
`fastboot continue` answered `Failed to load image from partition: Load Error`, and a power
cycle still went logo -> fastboot.  `fastboot getvar all` then showed
`slot-unbootable:a: yes` with `slot-successful:a: yes` and `slot-retry-count:a: 6`: the
current slot had been marked unbootable, which is why ABL refused Android every time.  The
likely author is the `xl` failure path, which repeats `fastboot reboot` while the endpoint
is down; each attempt that does not reach Android counts against the slot.
`fastboot set_active a` cleared the flag (`slot-unbootable:a: no`, retry count 7) and the
next `fastboot reboot` brought Android up with its data intact.  No partition was written at
any point -- `xl enter` only does `fastboot boot` of a RAM image (`xl`, `enter()`), and no
`fastboot flash` was run.

Lesson for the next hang: kill `xl` before it starts looping `fastboot reboot`, check
`fastboot getvar slot-unbootable:a`, and clear it with `fastboot set_active a` rather than
power-cycling repeatedly.  `xl`'s `recover_phone()` now sends at most three times and asks
`getvar` about the slot when it gives up (`fix(xl): stop the recovery loop ...`, commit
`9d86525e`); in the run below it stopped after exactly three sends and left the slot alone.

### The image that worked at 20:47 hangs too (2026-11-19, 00:15)

The controlled run kept everything from the 20:47 success except the environment: root image
`root-storeweather.apfs` `de7bc060` (20:35, the 48,840,704-byte image whose size matches the
payload `ramdisk` size `2e94000` in `logs/204708-run.log`), the same kernel `4ce8d7ba` and
loader `6a57fb6b`, `XL_SECS=60` and `XL_FOLLOW=5`.  The result is the third hang in a row:
`no answer — the kernel hung`, nothing written under `logs/` (so the follow never got a byte),
and the endpoint wedged again (`????????????`, `AdbWriteEndpointSync ... (121)`).  The freshly
built root image is therefore **not** the variable, and neither is lack of a full driver set:
`check-root.py` passed on every image used tonight.

What still differs between the 20:47 success and every hang since is how the loader was
entered and when.  Tonight each attempt began in Android and went through
`adb reboot bootloader`; earlier runs today started from a phone already sitting in fastboot,
and the night before the 20:47 run the phone had been in the loader for hours.  The cheapest
test of that difference is a cold entry: power the phone off, start it into fastboot with
power + volume-down, and run without touching Android first.

### The kernel boots to userland and hangs on the way back (2026-11-19, 00:28)

A clean `fastboot reboot-bootloader` and then the prebuilt loader gave the same `no answer`,
but the owner photographed the phone's console and it settles what happened: **the system
boots**.  From 00:28:37 to 00:29:27 launchd is spawning powerd, passwordsd, nanoappsync,
PassbookUIService, BTServer's responder and the rest, and the scene's own
`net_test: waitaddr en0: NO address after 25.2 s` is at the bottom -- the demo job was running
when the 60 s deadline arrived.  So the kernel and userland are healthy for the whole minute,
and the failure is in the **return to the loader** (`loader_return_after`), not in the boot.
What the owner saw earlier as a broken display -- garbage refresh, flicker, white -- is this
`-v` console stream, not the display driver.

Two things to separate next:

- **the Wi-Fi radio never joined** (`NO address after 25.2 s`), where the 20:47 run had
  joined, and a teardown of a half-started radio is a candidate for the hang;
- **the loader's fastboot endpoint was already unresponsive during the run**: `XL_FOLLOW=5`
  wrote not one byte, so the loader's own log could not be fetched either.

The console tail after the freeze is the evidence to capture: the screen stays on the last
kernel line, and a photograph names the teardown step that hangs.

### Those runs were started without the hardware arguments (2026-11-19, 00:47)

The four RAM runs between 23:03 and 00:39 that "hung" were started with a bare `./xl run`,
whose command line is only `-v`: **`xl` does not add `devices/thyme/ram.args`**.  The canonical
entries do (`devices/thyme/battery-guard.sh`, `ram-regress.sh`):
`ARGS=$(tr -d '\n' < devices/thyme/ram.args)` and then `XL_CMDLINE="$ARGS ..."`.  Without it
the kernel came up with no `gpu=`, no `touchscreen=`, no `battery=`, no `wifi=` -- the console
showed launchd spawning jobs and SpringBoard was spawned, and then the window server hung with
no GPU: the console froze, the kernel never answered `getvar result` or staged a log, the
loader's fastboot endpoint wedged, and the recovery loop once left slot a marked unbootable.
The `-v` console is also what looked like a broken display.

The same loader, kernel and root image with the full arguments pass: `logs/004846-run.log` --
`cmdline: 567 of 607 characters`, 583 `adreno:` lines with the GPU at 587 MHz, 120 SpringBoard
lines, the Wi-Fi network joined, `--- loader_return_after expired ---`, and
`result: returned 1441823`.  `XL_REBOOT_AFTER=1` sent the phone back to Android afterwards.

**A bare `xl run` is not a valid daily-driver boot.**  Use the runners, or pass
`XL_CMDLINE="$(tr -d '\n' < devices/thyme/ram.args) -v"`, and keep the total under the
loader's 607 characters (the log line `cmdline: N of 607 characters` is the check).

### The Sileo queue's working directory was not the problem (2026-11-19, 00:55, `logs/005526-run.log`)

The scene ran with the full `ram.args` and returned cleanly (`result: returned 1441823`).  Its
one-line test could not be planted: **`/` is read-only in this boot** --
`ln: failed to create symbolic link '/sileolists': Read-only file system` -- so the row's
assumption that the RAM boot volume is writable is wrong.

The hypothesis is moot anyway.  apt resolves the relative `-oDir::State::lists=sileolists/`
against its own `Dir::State`, not against the spawned process's working directory.  The
probe's three cases -- from `/var/jb/var/lib/apt`, from `/` with no link, and from `/` with
the link the scene could not make -- answered **the same**: all three `E: Unable to locate
package tree`, rc 100, before Sileo had rebuilt its index, and all three `The following NEW
packages will be installed: tree`, rc 0, after it (`sileolists-probe` lines in the log).  No
chdir is needed and no link is needed.

What the run did establish: Sileo's own spawn is exactly the command the row records
(`apt-get -sqf ... -oDir::State::lists=sileolists/ ... install --reinstall tree=2.0.1`), it ran
twice (00:59:07 and 00:59:46) with the package found, and its `rm -rf`/`mkdir`/`chown` of
`sileolists` is in the log.  So the queue is no longer emptied by a failed recheck.  But
`dpkg-query -W tree` still answers `no packages found matching tree` and
`/var/jb/usr/bin/tree` is absent: the flow stops after the simulation, before a download.
**The Wi-Fi was not at fault**: attempt 1 joined in 0.6 s to the access point on channel 149,
and the link carried 14,290 frames over 276 s; the `net_test: joinfile: the kernel refuses
ASSOCIATE: Device not configured` line is the scene's *rejoin after* the run's teardown had
left the network (`lx: the network is left`, 561 s), not a failure during it.  Sileo's spawns
in the whole run are two `apt-get`, two `dpkg --print-*`, and file operations -- no `dpkg -i`
and no downloader: Sileo fetches through its own NSURLSession, so the next run needs its
download task observed (or its queue sheet photographed) rather than a network test.

The `-v` console also shows what the owner saw on screen during these runs: `hakios-gl: frame
1040: 134 GL calls, 20 draws ...` and the haptic/audio lines -- a running desktop, not a
broken display.

### The Sileo page is drawn; the taps are what miss (2026-11-19, 01:22, `logs/012148-run.log`)

A run with the same scene and `ram.args` but **no `-v`**, with `xl screenshot` every 10 s
(`logs/quiet-NN.png`), shows the UI unobstructed and settles the display question: `quiet-07`
is Sileo's Featured page with its notification alert ("Sileo Would Like to Send You
Notifications", Don't Allow / Allow), the five-tab bar and the GET rows -- it renders normally.
The package page (`quiet-13`/`sileo-13` from the `-v` run) carries **QUEUE** at about
(0.815, 0.368) of the screen and the queue bar with `tree (2.0.1)` at about 0.76 of the height.

What the frames also show is that the old coordinates land on the wrong things: `quiet-14` is
**TrollStore's Settings** with `Error 1 -- Failed to enable developer mode`, and `quiet-19` is a
launching app, while the scene believes it is tapping GET and Confirm.  The queue was entered
(the bar shows `tree (2.0.1)`), but no `dpkg -i` follows and `dpkg-query` still finds no tree.

So the next Sileo run taps from positions measured on these frames, photographs each step, and
keeps `-v` off whenever the screen is the evidence.

### Sileo's analytics consent, and a tap that goes home (2026-11-19, 01:45, `logs/014551-run.log`)

The scene now launches Sileo first (`uiopen sileo://`, wait, then the package deep link) and
answers both screens it puts up: the notification alert (`Don't Allow` at 0.32 0.59) and the
"Download Analytics" consent (`Provided by Canister`, a full-width **Accept** at about
0.5 0.90 with **Deny** above it at 0.835).  The frames show Sileo drawing both normally
(`quiet3-12`), and then the **home screen** (`quiet3-13`: wallpaper, both widgets, every icon,
the dock) -- the 0.5 0.90 tap dismissed Sileo instead of answering the consent, most likely
because 0.90 is close enough to the home indicator to be read as the home gesture.

The run returned cleanly, the scene's last tap line is in the log (`$ /redwood/bin/rwtouch
wait 20 tap 0.86 0.37 wait 15`), but the second probe never printed and no `dpkg -i` followed:
whatever the taps did, they did not leave Sileo in front.  The next attempt answers the consent
at **Deny (0.5 0.835)**, photographs the answer, and only then sends the deep link.

Also worth knowing for every run: the fetched log is always **1,441,823 bytes** -- the loader's
buffer size -- so an early line survives only if the kernel has not logged past it.

### The package page is reached and queued; the sheet does not close (2026-11-19, 01:56, `logs/015611-run.log`)

With the launch-first fix and **Deny at 0.5 0.835**, Sileo's own page for `tree` is reached
(`quiet4-23` and `quiet4-26`: **QUEUE** at about 0.854 0.371, Details/Changelog, Repository
Procursus, the grey `tree (2.0.1)` line at about 0.72).  The QUEUE tap is answered -- Sileo's
apt simulation runs **twice** in the log -- but the package sheet never closes: `notify
com.hakios.close-sheet` leaves the page up in `quiet4-24`..`-26`, so the queue popup bar is
still behind the sheet, the tap at 0.5 0.862 finds nothing, no downloads sheet opens and no
`dpkg -i` follows (`dpkg-query -W tree`: no packages found matching tree).

Next: close the sheet by a route that works for this presentation (the grabber drag the
2026-11-13 runs used, or let the app close it), then tap the bar where it actually is, and
photograph the downloads sheet before the Confirm tap.

### Sileo installs a package through its own UI (2026-11-19, 03:13, `logs/031316-run.log`)

**The chain works end to end.**  The log carries the download and the install:

    -rw-r--r-- 1 0 0 43012 Nov 19 03:18 tree_2.0.1_iphoneos-arm64.deb
    Filename: pool/main/iphoneos-arm64-rootless/1800/tree/tree_2.0.1_iphoneos-arm64.deb
    tree_2.0.1_install ok installed
    -rwxr-xr-x 1 root wheel 109184 Dec 25 2022 /var/jb/usr/bin/tree

What had to be true, in this order: Sileo launched first and the deep link sent second (a
`sileo://package/...` URL on a cold start only opens Featured); the notification alert
answered (Don't Allow at 0.32 0.59); the "Download Analytics" consent answered at **Deny
(0.5 0.835)** -- Accept at 0.90 is read as the home gesture and dismisses Sileo; the package
page's **QUEUE** tapped at (0.854, 0.371); `notify com.hakios.close-sheet` dismissing the page
(the log says `close-sheet: the page on top was dismissed`, so the Darwin notification does
work); a tap at (0.908, 0.09) bringing the queue sheet up (`menu-26`: "Install", the row
`tree -- Queued`); and **Confirm at (0.5 0.833)** -- never 0.93, which is Clear Queue.

Scene `rc.jobs.sileolists`, image `root-sileolists.apfs` `e7d33a23`, 300 s, no `-v`, in
`logs/031316-run.log`.  This closes the queue problem the Sileo row has carried since
2026-11-13.

**Reproduced** in a second, independent run of the same scene and image
(`logs/033909-run.log`, 03:39-03:44): the same `tree_2.0.1_iphoneos-arm64.deb` (43,012 bytes),
`tree_2.0.1_install ok installed`, and `/var/jb/usr/bin/tree` (109,184 bytes, root:wheel).

**The Confirm tap has to wait for the resolver.**  Confirm is grey until Sileo finishes
resolving (`menu-26` and `final-24` are grey, `tree-26` is teal), and a tap on the grey button
does nothing -- the 07:37 and 07:52 runs installed nothing for exactly that reason, five
minutes in and still grey.  The scene now taps Confirm **twelve times at ten-second
intervals**, which covers the window instead of guessing one moment.  With that,
`logs/080705-run.log` (08:07-08:14, image `4d9698ca`) has the whole chain: the .deb, `tree_2.0.1_install
ok installed`, `/var/jb/usr/bin/tree` (109,184 bytes, root:wheel), and
`tree v2.0.1 (c) 1996 - 2022 by Steve Baker, Thomas Moore, Francesc Rocher, Florian Sesser,
Kyosuke Tokoro` from `tree --version` -- **the installed program runs**.  Note that a scene line
is not a shell: `tree -L 1 ... | head` passes `|`, `head` and `-12` to tree as arguments
(`tree: Invalid argument`), so pipes must not appear in a scene.

### The full scene runs to the end; the top-edge swipe locks the phone (2026-11-19, 02:17, `logs/021725-run.log`)

A 480 s run lets the scene complete every line: the sheet close (two swipes and the
notification), the bar taps at 0.885/0.90/0.87, the Confirm taps at 0.93/0.88/0.84, then the
closing probe and `dpkg-query`.  (The 300 s run ended in the middle of the taps, which is why
its probe had been missing -- the scene is simply longer than 300 s.)  The result is
unchanged: `dpkg-query -W tree` answers `no packages found matching tree`.

Sileo's own source (github.com/Sileo/Sileo, cloned 2026-11-19) settles two things about the
queue: the Confirm button that starts a download is `confirmQueued`
(`Sileo/UI/DownloadsViewController/DownloadsTableViewController.swift:336`), so the downloads
view controller has to be opened first; and the `AutoComplete` UserDefaults key
(`Sileo/UI/SettingsViewController/Root/SettingsViewController.swift:191`) only auto-taps
**Done after an install completes** (`DownloadsTableViewController.swift:608`) -- it does not
start a download, so enabling it would not remove the Confirm step.

The frames say why this attempt could not work: **`quiet6-24` is the lock screen** at 2:21.
The grabber swipe starts at the top edge (`swipe 0.5 0.06 0.5 0.92`), which on this system is
the Notification Centre / Cover Sheet gesture, and everything after it lands on the lock
screen instead of the queue bar.  The next run must not swipe from the top edge; if the sheet
cannot be closed, the queue bar has to be reached another way (a tap outside the sheet, or the
app closing it after a successful queue).

What is left to separate, offline, is the kernel (`hakios-kernel` `4ce8d7ba`, 17 November
14:18; `xnu-build.log` from 14:34 ends in `make[1]: *** Error 2`) from the loader: the two
hangs share the prebuilt loader image and the kernel, and nothing in the runs distinguishes
them yet.

**QEMU boots the same inputs without hanging.**  `qemu/run-darwin.sh` with
`ROOT=out/thyme-boot/root-wirelessfull.apfs`, `IMG=userland/system.apfs` and 120 s of guest
time ran the same `hakios-kernel` `4ce8d7ba` and the same root filesystem to SpringBoard:
74 SpringBoard lines, launchd spawning jobs, no panic, and the log ends at 124 s with
`== END OF LOG ==` (`/tmp/qemu-display.log` of that session).  What this separates is the
root filesystem from the phone's hardware init, not the kernel from the loader: QEMU has no
Adreno, no Wi-Fi, no ADSP and no PCIe, so `gpu=1`, `wifi=1`, `adsp=1`, `cam=...` and the rest
of `ram.args` do nothing there.  The two hangs both happen with the phone's own hardware
brought up, and the screen the owner saw was the display path itself.

With that identified, tonight's loader is the canonical one: `XL_BOOT_IMAGE` named
`devices/thyme/prebuilt/xnu-usb.img`, which is exactly what `devices/thyme/build-loader.sh`
writes, and it was rebuilt on 17 November 14:20 together with
`xnuloader/loader-thyme.efi` (`6a57fb6bb147`).  The loader hash earlier sessions reported is
`615d9a67c677` (14-16 November); no run log carries an `xl: inputs` line with `6a57fb6b`, so
whether the 17 November loader has ever completed a boot is not recorded.  That makes the
loader rebuild an untested variable, not a wrong one, and it is the first thing a run with
`xl: inputs` written down should settle.

### After a run, getting Android back needs set_active (2026-11-19)

`XL_REBOOT_AFTER=1` sometimes leaves the phone in its bootloader, and there a plain
`fastboot reboot` does not bring Android back.  What happens on such a failed boot is that the
bootloader sets **`slot-unbootable:a: yes`** -- and the retry count does not have to move for
it to do so.  With the flag set, no reboot reaches Android; `fastboot set_active a` clears it
(`slot-unbootable:a: no`) and the next `fastboot reboot` boots Android, which is what happened
at 04:00 (two attempts, flag cleared, booted) and again at 07:12 (one attempt set the flag,
cleared, booted).

So the check after any failed `fastboot reboot` is `fastboot getvar slot-unbootable:a`, and the
fix is `fastboot set_active a` -- not another reboot, and not a power cycle.  The retry count
(6 here) is not the thing that changes.

### Alpimaps still aborts with metal_all=1 (2026-11-19, 08:55, `logs/085529-run.log`)

The map app was downloaded again from the developer's own release (Akylas/alpimaps 3.0.4
build 242, 62,303,169 bytes, SHA-256 `566c5f52...4f94`, verified), served from the LAN and
installed with the TrollStore helper (`rc.jobs.almapsdiag`, image `218c5b5e`).  The kernel
command line carried the full `ram.args` plus **`metal_all=1`** (568 of 607 characters, so
nothing was cut).  It still aborts: `Successfully spawned alpimaps[266]` at 09:00:46,
`exited due to SIGABRT` at 09:00:49.

So the Metal device was not what it lacked.  Its own output (the scene's direct run,
`cat /private/var/tmp/almaps.txt`) says what kind of app it is: `Runtime initialization took
6ms (version 8.9.6, V8 version 10.3.22)` -- a React Native app on V8, with Sentry started
before it dies and breadcrumbs reading only `in_app_failure`.  That is the same shape as
PPSSPP's abort: a runtime that wants JIT/ptrace, which this port does not give an app.  The
next look goes there, not to another boot argument.

### The Alpimaps abort is an exception nobody caught (2026-11-19, 22:00)

The `userabort:` line of the 08:55 run carries the whole return-address chain, and it can be read
now.  `qemu/symstack.py` resolves it once `ipsw` is installed (v3.1.749; the Linux release
tarball's SHA-256 checked against the published `checksums.txt`).  The chain, innermost first:

    _abort + 124                                    libsystem_c
    libc++abi   (abort_message, its terminate path)
    libobjc.A   func_1800d1f98 + 124
    0x104106e28                                     the app's own image
    libc++abi
    __ZSt9terminatev + 52                           std::terminate
    libdispatch                                     the main-queue block callout
    _CFRunLoopRunSpecific + 584, _UIApplicationMain

`std::terminate` is in it, with libobjc and libc++abi around the application's one frame: this
SIGABRT is an exception nobody caught, not a process that was refused executable memory.  The
JIT/ptrace reading of the entry above does not hold for this app (it may still hold for PPSSPP,
whose abort has not been symbolized).

Two tooling facts this needed: `ipsw dyld a2s` refuses a cache that has no `.symbols` file beside
it, and the copy under `out/thyme-boot/dsc-arm64/` has the `--cache` address-to-symbol file that
works instead (its cache is byte-identical to `sysimage/20F66__iPhone10,3_6/`, sha256
`f0cbc6aa...`), so `symstack.py` now prefers a cache that has the sidecar.

What the exception *is* comes next: `applog=1` (implemented in `xnu/hakios.patch`) sends a
program's standard error to the console, and `applog_name=NAME` limits that to programs whose
executable path *begins with* NAME -- so every app wants `/var/cont`, not `alpimaps`, which no
path begins with.  A run with `applog=1 applog_name=/var/cont` on top of the full `ram.args` plus
`metal_all=1` is 600 of the loader's 607 characters.

### The exception is Carto's ANGLE: `eglInitialize()` (2026-11-19, 22:10, `logs/220821-run.log`)

That run was made (the same full `ram.args`, `metal_all=1 applog=1 applog_name=/var/cont`), and
the app's standard error names the exception:

    *** Terminating app due to uncaught exception 'NativeScript encountered a fatal error:
    Uncaught Error: Failed To call eglInitialize()
     at createNativeView(file:///.../alpimaps.app/app/vendor.js:2741:360)
    ... at _setupUI ... at layoutView ...

The line it is thrown from says who does it: `class CartoMap extends X ... createNativeView(){
return AKMapView.alloc().init() }`.  Alpimaps draws its map with the **CARTO Mobile SDK**
(`AKMapView`, `AKMapEventListener`, ...), whose iOS build embeds a whole **ANGLE**: the app's
26 MB binary carries the `*ContextANGLE` symbols, `eglGetPlatformDisplay`, `eglInitialize`, the
exception `MGLSurfaceException`, and both of ANGLE's Apple backends, `DisplayEAGL` (with
`PbufferSurfaceEAGL`/`WindowSurfaceEAGL`) and `DisplayMtl`; no other file of the IPA contains
`eglInitialize`.  `eglGetPlatformDisplay()` itself succeeded (its failure message
`Failed To call eglGetPlatformDisplay()` is in the binary and never printed), so the backend
display is what cannot come up.  Both Apple routes explain that without the port: **Metal** has
no device to give (nil in every process until `metal=1`), and the **EAGL fallback** has no
driver behind it in an app -- the Mesa stand-in `libhakios.gl.dylib` is loaded only into the
window server (`inject.c`'s `opengl_es()` is called in the `backboardd` branch alone), and the
app's process prints no `hakios-gl:` line at all.

The 08:55 run had been given `metal_all=1` but not `metal=1`, and that is the kernel's half of
the device: `MiniIOMobileFramebuffer.cpp` publishes `MetalPluginName`/`MetalPluginClassName` only
for `PE_parse_boot_argn("metal", ...)`, so the accelerator named no plug-in, `MTLCreateSystem-
DefaultDevice()` answered nil, and `metal_all` had nobody to tell.  The CA plug-in
(`src/HakiosMetalCA.m`) now reads **`metal_apps=1`** as well: the device goes to the programs
whose executable path begins with `/var/containers/Bundle/Application/` and to nothing else --
not the window server (a device it cannot draw with takes the screen down), not SpringBoard or
mediaserverd (the comment above the gate records what they do with pipeline libraries made for
Apple's GPUs).  A run with `metal=1 metal_apps=1` on top of the full `ram.args` plus
`applog=1 applog_name=/var/c` is 605 of the loader's 607 characters (image `c93f05ca`).

### The device reaches the app, and the app reaches its map (2026-11-19, 23:09, `logs/230949-run.log.live`)

The gate above refused the app it was written for: `_NSGetExecutablePath` reports the real path,
`/private/var/containers/Bundle/Application/...` (`/var` is a link to `/private/var` on Darwin),
and the comparison was against `/var/containers/...` alone.  The plug-in now steps over a
leading `/private` before the comparison and prints the path it saw in the refusal.  With that
(image `c45ddcbc`), the same run (full `ram.args` + `metal=1 metal_apps=1 applog=1
applog_name=/var/c`) shows, in order:

- the window server and the system's services still refused, as intended -- e.g.
  `HakiosMetal[backboardd 18]: backboardd (/usr/libexec/backboardd) is not an application: it
  keeps the road it has`, likewise PosterBoard, CollectionsPoster and MapKit's snapshot service;
- the app given a device:
  `HakiosMetal[alpimaps 244]: device: Adreno (hakios-metal, chip 0x6050002) on IOAcceleratorES
  service 0x8a03; the compiler is in this build; feature profile 1 (Apple1, Apple2 and Common1)`;
- **no `eglInitialize()` failure and no `Terminating app`**: ANGLE comes up on its `DisplayMtl`
  backend (the glslang frames, Metal textures and command buffers in the log are ANGLE's);
- the app's own road: `MapView::appDidBecomeActive`, `onMainMapReady {"lat":45.2012,"lon":5.7222}`,
  a Sentry session (`akylas.alpi.maps@ios@3.0.4+242`) and its network requests;
- `logs/screen-alpimaps2.png`: the status bar, the Search field, the app's own dialog ("The true
  power of Alpi Maps lies in using offline generated data..." with OPEN GITHUB / CANCEL) and the
  map's controls all drawn -- **the first third-party app on this port to reach a usable UIKit
  screen**.  The map view itself is black.

What the black map is, the same log says in three refusals:

    refused: a texture: 1125x2436, pixel format 253, usage 0x4: pixel format 253: not one of the
    formats served (texture.hpp: PixelFormat)
    refused: a texture: 1x1, pixel format 70, usage 0x7: a cube RGBA8Unorm texture with usage 7: ...
    refused: -[HakiosMetalDevice supportsSampleCount:] is not implemented in this plug-in; it
    returns zeros

Pixel format 253 is Metal's `Stencil8`: ANGLE gives its default framebuffer a stencil
attachment (the screen's size, a render target) and hakios-metal does not serve the format, so
the attachment is nil -- the app's GL then reads `GL_OUT_OF_MEMORY` (0x505) at `Texture::create`
and `GL_INVALID_OPERATION` (0x502), and `BackgroundRenderer::onDrawFrame` reports 0x505 every
frame.  The 1x1 cube asked for with usage 7 (shader write beside read and render target) is the
second: the plug-in serves a cube as read + render target only, since a kernel cannot store into
one.  Both are general holes, not Alpimaps': any GL app that asks ANGLE for a stencil buffer
meets the first, and ANGLE's Metal backend asks for the cube itself.

### Stencil8 alone is served; what still refuses a map draw (2026-11-20, 00:08, `logs/000810-run.log.live`)

The 23:09 section's first refusal was Stencil8 (253) itself.  It is now in the format table and in
the CA plug-in's own gates: a 1125x2436 texture of format 253 is created, the 1x1 cube asked for
with usage 7 is served, and run 5 (`logs/235037-run.log.live`, image `5d9b6d64`) is the first in
which the app's dialog and controls draw with **no `GL_OUT_OF_MEMORY`/`GL_INVALID_OPERATION` at
all**.  ANGLE does not use one Depth32Float_Stencil8 texture, though: it gives its default
framebuffer a D32_FLOAT texture and an S8_UINT texture of its own.  Run 5 refused each such clear
("a stencil attachment that is not the depth attachment's own Depth32Float_Stencil8 texture", 4
times) and the draws behind them, with six `0xe00002d8` from the GL side.

Run 6 (image `855dcd3b`) is the first with the separate plane served: `RB_STENCIL_INFO` carries
SEPARATE_STENCIL with the stencil texture's own base, pitch and byte count, the depth plane stays
DEPTH6_32, and the same log has **no stencil refusal, no `0xe00002d8`, no GL error** (and
`onMainMapReady` twice).  What remains is three general gaps, named by the log itself:

- `-[HakiosMetalDevice supportsSampleCount:]` is not implemented and returns zeros (8 times):
  ANGLE asks it while choosing render targets; a wrong answer here costs multisampling, not
  correctness, but the method should answer what the GPU serves.
- 9 draws refuse `main0: buffer 16 steps constantly: per-vertex and per-instance fetch are
  served` -- a buffer argument bound with a constant step function, which Metal uses for
  draw-wide constants.  Serving it is the next general vertex-stage fix.
- the 1x1 cube copy still refuses slices 1-5 ("one slice and depth 1 are served", 5 times).

None is specific to one app; every one is a hole any ANGLE/Metal application meets.

### Alpimaps is gone; the map slot takes the IPA the operator stages (2026-11-20, 00:25)

At the operator's request Alpimaps, its four scenes (`rc.jobs.almapsdiag`, `almapsrunas`,
`almapswrap`, `almapswrapq`), its two helpers and the LAN copy are removed.  `hk-execwrap.sh`
stays as a generic wrapper with no default bundle id.  The new scene is `mapdiag`
(`SCENE=mapdiag`, image `3fec58ef`): `check-root` reports `rc.jobs=rc.jobs.mapdiag`, the image
carries `root/bin/hk-mapipa.sh`, and the generated `rc.jobs` has no `almaps` line at all.

What it installs is whatever the host staged, not an app baked into the image:
`tools/map-run.sh APP.ipa` reads the bundle's `Info.plist` and the executable's
`LC_ENCRYPTION_INFO`, writes `target.ipa` and `target.txt` (name, bundle id, version, .app
directory, SHA-256, source) into the LAN directory, and boots the phone.  On the phone
`hk-mapipa.sh` fetches both, hands the digest to `hk_download` (a mismatch returns 4 and nothing
is installed), installs through TrollStore's helper, runs `uicache -a` and
`uiopen --bundleid`, then prints the container and crash-report directory to
`/private/var/tmp/mapipa.txt`.  The LAN server runs on the Windows side
(`tools/ipas-server.ps1`, `--directory \\wsl.localhost\Ubuntu\home\darling\ipas-lan`) because
the phone reaches `192.168.0.104` and not WSL2's `172.24.x` address.

**The FairPlay line is enforced before the image is built.**  `tools/map-run.sh` refuses an
executable whose `cryptid` is not 0: an App Store IPA is FairPlay-encrypted, this port has no
Apple key to decrypt it on a Snapdragon, and installing one would prove only that the helper
copies files.  高德地图 (`com.autonavi.amap`) and 百度地图 (`com.baidu.map`) are App Store-only
and no decrypted copy is downloaded here; the intake waits for a decrypted IPA the operator
lawfully has.  Without one there is nothing installable to test, and this record says so rather
than claim a run.

### The intake's first run: LocProbe installs, registers and starts (2026-11-20, 00:32, `logs/003257-run.log.live`)

The first phone run of image `3fec58ef` carries the project's own LocProbe (self-built,
cryptid 0, 72,672 bytes, SHA-256 `706dbae288574fd3637934e17b93dc0554223c5dc0ff1bef460d51a8cd87f927`)
through the intake, and the log shows each step in order:

    == hk-mapipa Fri Nov 20 00:34:22 CST 2026
    == download rc=0                     (the digest hk_download compared matched)
    [installApp] created app container: com.hakios.locprobe UUID: 4BD6298B-...
    [installApp] new app path: /private/var/containers/Bundle/Application/B0299FA5-.../LocProbe.app
    == install rc=0
    == uiopen rc=0
    Successfully spawned LocProbe[251] because launch job demand

No crash report was written for it (the CrashReporter directory carries only the Assistant,
DiagnosticLogs and appstored subdirectories).  The app did reach its own code: SpringBoard
took its launch snapshot (`XBApplicationSnapshot`, referenceSize 375x812), the app logged
`launched: services 1, authorization 0`, and the one failure it reported was CoreLocation's --
`LocProbe: failed: kCLErrorDomain 1` (denied), the port's location service being absent, not
an install or launch fault.  The 420-second run ended before a screenshot could be taken and
the phone returned to Android; the chain this section is about -- stage, digest, install,
container registration, SpringBoard launch, the app's first frame -- is what the log records.
The intake is therefore the route the next real application takes; the app it installs is
chosen by `tools/map-run.sh APP.ipa`, not by the image.

### 百度地图 22.2.20 goes through the intake and launches (2026-11-20, 00:53 -- 01:48, `logs/005327-run.log`, `logs/011513-run.log`, `logs/013254-run.log`)

The operator staged 百度地图 22.2.20 (`com.baidu.map`, app directory `IphoneCom.app`,
207,202,418 bytes, SHA-256 `299ea64e50d1841499b64822eae236ea0fed7323fea88010fe7a6ea20363ac43`,
`cryptid 0` -- the operator's own lawfully decrypted copy; see `ipas.md`).  The intake
installed it, `uicache -a` registered it and `uiopen` launched it in all three runs -- the
process `IphoneCom` (pid 265 in the first) starts, runs its WebKit and AppTrackingTransparency
services, opens its sockets and is shut down by the end of the run, not by a crash.

Its screen is the app's own UI: search bar, layer and feedback buttons, the tab bar, ads and
the location dot are all drawn (`screen-baidu.png` at 00:56, `screen-baidu3.png` at 01:19,
`screen-baidu4.png`/`-5.png` at 01:35 and 01:37, `screen-baidu6.png` at 01:41), while the map
canvas itself is black and flickers.  The log of the first run carries one MetaL refusal
repeatedly -- a 1125x2436 depth texture, `Depth32Float`, usage `0x7` (read, write, render
target), turned down because the port serves depth textures as render targets only -- and
that was taken for the cause of the black canvas at the time.  It is not: later runs draw
the map with the same refusal in the log, and the section below finds what actually kept it
off the screen.

### The black canvas is the driver's own drawing, not the app's (2026-11-20, 08:01 -- 08:42, `logs/080159-run.log`, `logs/081738-run.log`, `logs/084224-run.log`)

The scene `rc.jobs.mapblack` answers the app's launch dialogs in the order the two runs
before it raised them (the 隐私保护指引 promo's 一键开启, the 欢迎使用百度地图 consent's 同意,
the location prompt's Allow While Using App, the notification prompt's Don't Allow and the
ATT prompt's Ask App Not to Track), then leaves the map alone 45 seconds and pans it.  Its own
comments record what the earlier runs showed: behind the welcome consent, before the app's
Metal engine had drawn a frame, the map was drawn (Chengdu, its districts and highways) -- a
CoreAnimation picture; from the engine's first frame on, every frame with a system alert over
the app had the map area black while the app's own bars stayed bright.

With every prompt answered (`logs/081738-run.log`) the picture is the same: the app's Metal
drawables hold the map -- `hm_draw: surface 359 (1125x2436): 10863 of 10863 samples not zero
(1.000), mean 219,234,199, alpha 254`, the same for surfaces 371 and 417 -- drawn at 3 to 10
frames a second, and the panel shows black where that picture is.  `glcopy=1`, the run of
08:42 which turns off shared surfaces so that every drawable is copied by the CPU, changes
nothing: the drawables then hold mean 221,235,201 at alpha 254 (`logs/084224-run.log`), the
panel is still black.  So the surfaces are the app's picture and are not what the panel gets.

What the panel gets is the window server's composite, drawn after it: the `fbshow` sample of
swap 9600 (`logs/081738-run.log`) has layer 1 (sid 1) with `source mean 96,97,100 ... alpha
101 clear 74/144`, and the panel over the map rectangle `was 96,97,100 ... is 96,97,100` --
the panel is showing the composite, and the composite is clear over most of it.  The kernel's
`drawRows()` put every layer's pixels on the panel as they were: a pixel of a layer meant to
be seen through was written as it stood, black, over the picture under it.  That is one
general fault, not Baidu's: any application whose CAMetalLayer is displayed directly (swap
layer 0) under the window server's composite (layer 1, premultiplied, alpha 0 where the
direct layer is to be seen) had its picture erased by the clear pixels.

### The compositing run: the map is drawn, and the picture tears (2026-11-20, 09:03 -- 09:07, `logs/090328-run.log`)

The kernel now composites: a layer above the bottom one is drawn with premultiplied
source-over (`over()`, the colour of the layer plus what opacity the panel's pixel has left),
while the bottom layer is copied as before.  The change is in
`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (and the build tree it is exported to)
only.

On the phone the map appears.  `screen-black-80.png` (09:06) shows 百度地图's 景点 list drawn
whole -- the search field with its blue focus ring, the category icons, three POI cards with
their photos, ratings and quotes -- and `screen-black-100.png` (09:07) the POI page of
雁南公园 with its photo strip and the bottom 导航/到这去 buttons.  The app's whole UI, not
only its text bars, is on the panel for the first time.  The operator, watching the phone,
reported that the picture now refreshes from top to bottom -- the display still is not right.

The log says why.  `181 frames in 5303 ms (34 a second), 20863 us each in the driver` and,
in the next window, `15 frames in 5066 ms (2 a second), 337476 us each in the driver` -- 20.9
and 337 ms a frame, against the 11.1 ms one scan of the panel takes at 90 Hz.  THYME's panel
is command-mode DSI with no usable frame counter (`fb_beam`, whose comment already described
the moving bands of 3 November), so the panel scans the same memory the driver writes: a
frame that takes longer than a scan is shown part old, part new, and the boundary moves down
the screen -- the refresh the operator saw.  The cost is the compositing itself: `over()`
takes the panel's pixel (`to[x]`) for every pixel of the blended layer even where the layer
is clear, and the panel's memory is slow to read.  The run ended at 09:07 not by a fault but
by the battery guard, `the charge is 24%, at or below battery_stop=25 three looks in a row:
back to the loader`.

### Run 24: the row-classified kernel measured, and 74 ms a frame (2026-11-20, 09:51 -- 10:02, `logs/095107-run.log`)

The kernel of this run (build `3f31370783c0`: the compositing run's kernel plus rows looked
at before they are mixed -- a row of the window server's layer whose source pixels are all
clear skipped before the blending -- and clear source pixels not read from the panel) puts
the map on the panel as correctly as the run before: `screen-black-460.png` (10:00) and
`screen-black-520.png` (10:01) show 百度地图 whole -- `洒金桥早市` in the search field, the
map's building numbers and roads around 渭河, the 图层/加行程/消息 column, the category chips,
the POI sheet of 韦曲街道 with its 导航 and 到这去 buttons, the status bar's clock and charge.

The frame times are worse, not better.  The run's 1378 frames: `shown->done` min 30.3 ms,
median 74.5 ms, p90 82.3 ms, p99 151.7 ms, max 320.9 ms (the `tl: v` lines, tools/latency.py's
method), and the five-second stats agree: `12 frames in 5521 ms (2 a second), 76363 us each
in the driver`.  Against the 11.1 ms of one scan that is six and a half panels a frame: what
the operator saw as the picture refreshing from top to bottom is unchanged, because the
frames are still written into the panel's own memory (mapped non-cached) pixel by pixel.
The pre-mixing row scans made the blending slower, not the panel faster: the fix is not in
the scanning but in where the frame is made.

The run ended as run 23 did, by the battery guard at 25% (kernel time 730 s); the scene's
screenshots at 560, 620 and 680 s failed for want of a loader, the phone having gone back to
the loader at the stop.

Run 24's end left the phone in fastboot, as run 23's had.  `fastboot set_active a`,
`fastboot reboot-bootloader`, `fastboot reboot` reached Android this time
(`sys.boot_completed=1`, kernel `4.19.157-HyperOS3-A16-OurSky-Developers+`), and the phone
charges over USB there (29%, `charging` in Android's `dumpsys battery`).  The earlier failures
to reach Android after the same steps are not explained; the flag is the first thing to look
at when it recurs.

### Run 25: the frame made in a shadow and copied to the panel, 3.7 ms a frame (2026-11-20, 10:22 -- 10:33, `logs/102255-run.log`)

The kernel of this run (build `1df34a4ca6a1223d`) takes the frames out of the panel's
memory.  The panel's memory -- the boot framebuffer -- is mapped write-combining and not
cached (`video_console.c`: VM_WIMG_WCOMB), so every pixel the blending read and wrote of it
was two trips to the memory, some 8 ns each: 2.6 million of them a frame, 74 ms (run 24).
Now every layer is drawn into a shadow of the panel, 1080 x 2340 pixels of the kernel's own,
cached memory (`IOMallocZero`, 10.1 MB, in `start()`), and the finished frame is copied to
the panel's memory in one pass, row after row, before the next scan reads it
(`blitShadow()`, `show()`, `settle()`; the black-fill and the samples follow the shadow too).
The pre-mixing source-row scan run 24 measured has been taken out again: it was the blending's
slow part, not the panel's.  If the shadow cannot be had the driver logs it and goes on as
before.

The frame times say it works: 2039 frames of the run, `shown->done` min 3.08 ms, **median
3.66 ms, p90 10.0 ms**, p99 13.4 ms, max 20.4 ms, mean 4.9 ms, against run 24's median 74.5
and run 23's average 20.9.  One scan of the panel is 11.1 ms: the median frame is a third of
one, and the p90 is just inside.  The swipes of the scene -- the operator's tearing gesture,
at t+235 and t+255 s (10:28:46 and 10:29:06) -- measured in windows of 300 frames: median
3.3 and 3.4 ms, p90 3.7 and 4.9 ms.  The five-second stats line: `45 frames in 5515 ms (8 a
second), 5993 us each in the driver` -- the driver now costs a tenth of a scan per frame,
against `12 frames in 5521 ms (2 a second), 76363 us each` in run 24; the application's own
rate went from 2 to 8 frames a second, so it was the driver that had been holding it back.

The scene's screenshots are whole frames: `screen-black-245.png` (10:28:59, just after the
vertical swipe) is the map at its new place and zoom -- 万景·荔知湾, the 灞河, the chip row
充电站/厕所/酒店/公交站 and the tab bar -- and `screen-black-330.png` (10:30:24, after the
horizontal swipe and the tap) the POI sheet of 韦曲街道 at 315 m.  The samples agree:
`sid 3, source mean 95,97,100 ... alpha 101 clear 74/144; panel ... was 211,230,230 ... is
211,230,230`, the app's drawables `mean 221,235,202, alpha 254`.

What is left of the tear: about one frame in ten is at or over a scan (p90 10.0, p99 13.4),
and those frames can still show a band; the copy to the panel is a single thread's (~2 ms of
the frame), and the filtered redraw of a still screen is the slowest kind of frame.  If the
operator still sees bands, the next step is to share the copy out among the drawer threads
and shorten the filtered redraw.  The run ended at kernel time 623 s by the battery guard at
25%, with the swipes long drawn.

### Run 26: the copy shared out, 3.2 ms a frame and a p90 of 3.6 (2026-11-20, 10:55 -- 11:08, `logs/105725-run.log`)

The kernel of this run (build `0742b242870f`) gives the copy to the panel to the same threads
that draw the frame: `blitPanel()` wakes the `fb_threads - 1` helpers for their share of the
rows, copies its own share, and each helper copies and cleans its own rows with its own
`dc civac` pass (`blitPanel()`, `helperMain()`); with no helper up yet it copies alone, as
before.  The five-second stats line now reports the copy: how many frames, the us each, the
longest.

The tail run 25 left is gone.  2462 frames: `shown->done` min 3.05 ms, **median 3.21 ms, p90
3.57 ms**, p99 9.78 ms, max 20.7 ms, mean 3.44 ms -- the p90 was 10.0 ms in run 25.  Nine
frames in ten are now finished in a third of the 11.1 ms scan, and 99 in 100 inside it.  The
copy itself: 993 to 1202 us each for the ten megabytes of a whole frame, the longest of a
window 1.1 to 4.2 ms -- a third of what it cost one thread.  The driver's lines:
`3610 us each in the driver ... 48 frames copied to the panel in 1202 us each (the longest
1794); 0 of 0 too late for the next scan`.  The application still draws at 8 a second at
rest and 27 in the busiest window.

The screenshots are whole frames: `screen-black-330.png` (11:04:59) and `screen-black-460.png`
(11:07:10) show the 灞河 with 云不知·私宴·湖滨餐厅 and 麻绳湖畔音乐荟, the chip row and the tab
bar, and the search field's suggestion has changed between the two, so the app is live
between the shots.  The samples agree: `panel ... was 193,222,216 ... is 193,222,216`, layer
1 `mean 51,50,50 alpha 51 clear 101/144` over the app's own surface.  (The scene's swipes at
t+235 and t+255 s moved the map less than in run 25's shots -- the tap at t+275 s re-centres
it; the draw times above are the same gesture windows either way.)

The one wild frame is still there (max 20.7 ms), but it is one frame of two and a half
thousand, and no window's line counts a frame late for the scan it aimed at.  The run ended
at kernel time 732 s by the battery guard at 25%; the phone went back to Android to charge
(`XL_REBOOT_AFTER`).

### Run 31: the blank canvas was my own rebuild -- the image lost its Metal plug-in (2026-11-20, 13:41 -- 13:47, `logs/134128-run.log`)

The run set out to watch the dialogs answered and the map left alone, on the image rebuilt at
13:33.  It came back with a white canvas on every frame after the consent, and the question was
whether a kernel change had done it.  It had not: the rebuild ran `devices/thyme/build-root.sh`
without `METAL_PLUGIN=ca`, and that build ships `src/HakiosMetal.m`, the plug-in that refuses
every application, instead of `src/HakiosMetalCA.m`, the one that grants the device to
executables under `/var/containers/Bundle/Application` when `metal_apps=1`.  Not one boot
argument differed from run 26's.

The log says it plainly.  There is no `HakiosMetal[` line in it at all, no `hm_draw`, no
`layer 1`; the only device refusals are backboardd at 13:41:41 and com.apple.WebKit.GPU at
13:45:09, both in the non-CA wording, `is not shown the device (metal=1; 1 is metal_test only,
2 all but backboardd, 3 all)`.  Run 26, with the CA image, logged
`HakiosMetal[IphoneCom 358 ...]: device: Adreno (hakios-metal, chip 0x6050002) on
IOAcceleratorES service` and drew `hm_draw: surface 358 (1125x2436): ... mean 219,235,198`.
So the app had no renderer to draw its engine into, and the canvas stayed what the app's own
views The search field's cyan-to-violet ring is in these frames and in the pre-engine picture
(`screen-r31-40.png`, drawn by the app's CA views before any Metal frame exists), so it comes
with the app's own drawing.  Whether it is the focused-search style the app uses on real
hardware cannot be settled from this tree; what run 32 can settle is whether restoring the
app's device changes it.

The screenshots follow the same line.  `screen-r31-40.png` is the 欢迎使用百度地图 consent
over Chengdu -- the CoreAnimation picture the app draws before its engine begins.
`screen-r31-100.png` has the consent answered: every UI element is drawn and live (the
左安门桥 house-price sheet, the map buttons, the search field with 回民街必吃十大美食 typed
into it) over a canvas with no map in it.  `screen-r31-160.png` is the 停车场 list the search
opened.  The search field's cyan-to-violet ring in these frames is the focused-search style
the app itself draws -- it is there in the pre-engine picture too, drawn by the app's CA views.

The pixel dumps put the blank canvas on the guest's side of the panel.  At the map page the
`fbdump: searchbar row 150 x 0` samples hold `fff4f5fc`-type near-white pixels in `src` and the
drawn frame holds the same run -- the panel presents what the layer has.  The layer itself was
near-white because nothing above the app's views ever drew into it.

The scene itself went as written: three rounds of dialog taps answered everything, a fourth
round of taps (this scene had four then) fell past the last prompt and opened a POI page, and
the battery guard ended the run at kernel 431 s (`battguard: charge 25% ... battery_stop=25`,
three looks) with the shutdown at 461 s.  The image was rebuilt with `METAL_PLUGIN=ca`; its
build log has `build.sh: HakiosMetal for CoreAnimation (src/HakiosMetalCA.m)`, the staged
bundle (8,731,336 bytes) holds `keeps the road it has` and not `metal_test only`, and the image
is `67c75777b65ffa4b016d7faaeeb21f8ae332fffdb1ff443ff4f3204f7b44f852`.  Run 32 uses it.

### Run 32: the app draws the map again on the CA image; the thick rim is not in the app's own drawable (2026-11-20, 14:37 -- 14:49, `logs/143707-run.log`)

The run took the image rebuilt at 13:33 with `METAL_PLUGIN=ca` (`67c75777`, the one run 31
should have used), the prebuilt loader image (`213ffaaeb820` -- a kernel built for this tree is
refused by this bootloader, `Failed to load/authenticate boot image: Load Error`), and run 31's
scene and boot arguments plus `glcensus=1` and the framebuffer dumps:
`fb: boot tunables: fb_dump=10 fb_dump_every=3 fb_dump_times=190`, 607 characters of cmdline.
Kernel `57b424508267`.

The app got its renderer.  The log has `HakiosMetal[IphoneCom 265]` device and fps lines all
run long, `hm_draw: surface 375 (1125x2436): 10863 of 10863 samples not zero (1.000), mean
244,240,228, alpha 251` (and surfaces 387 and 442 the same), and `fbshow: swap 6000: layer 1
sid 3, source mean 217,216,216 ... alpha 233; panel 0,0 to 1080,2340`.  Layer 0 is the app's
1125x2436 drawable, layer 1 the window server's 1080x2340 surface.  The screenshots agree:
`screen-r32-100.png` is the map of 西安 长安区 with its POIs (鲁家湾小区, 万景荔知湾, 李想大虾),
the search field and the side panel; `screen-r32-300/520/580.png` are the 超市 result list with
photos, ratings, hours and distances loaded over the network.  The canvas, the app's views and
the network all work in this run.

The thick rim is measurable and reproducible on this image.  At column x=540, the search
field's top edge in `screen-r32-100.png` is 3 px of background, then 16 px of dark navy
(`#303060 #204860 #204868 #385070 #304860 #284068 #204860 #204858 #304060`), then 5 px of cyan
(`#28a8e0 #28c0f8 #68d0f8`), then the white field.  In `screen-r32-520.png` the same edge has
the same 19 px of band but light (`#d0d0e0` to `#687088`) -- the band is a gradient that moves.
The side panel's left edge at row y=450 in `screen-r32-100.png` is 17 px of navy
(`#303060 #202048 #182048 #001858 #183060 #103060 #002850 #002058 #002060`) before the
`#f8f8f8` interior; run 31's picture of the same panel at the same row has only light grays
(`#f0f0f8 #e8e8f0`) there.

The dumps say where that navy is not.  Every retained `ui@` sample's source line is `src 0,156`
-- the 1125-wide application drawable, panel row 150 read from its own row 156.  At that row
its pixels are `00000000` from x 0 to 14, then `04010102 04010202 06010203 08020304 0a020305
0b020305 0c030406` -- premultiplied near-transparent dark, which unpremultiplied is about
`#404080` at alpha 4-12 (a colored shadow, faint), then `c25086c0` at x 30, the 4 px cyan line
`ff66b2ff` at x 31-34, and white from x 36.  The drawn frame at the same panel row holds
light pixels at x 0-13 (`d6f7fdfb ... d7ffffff`), then *opaque* navy at x 14-29
(`ff333366 ff336666 ff24496d ff404060 ff385571 ff334c66 ...`), `ff599cdd` at x 30 where the
drawable is `c25086c0`, and then the drawable's own values again (`ff66b0fd ff65b2ff`,
`ffffffff`).  Where the drawable is opaque the frame carries it through; where the drawable
holds a faint shadow the frame holds an opaque navy band of the same extent.

The frame is live, not stale: the first navy pixel at x 14 changes every 3 s sample from
`ff78789f` (687 s) through `ff5e5e92` (691 s), `ff4c4c85` (694 s), `ff3a3a73` (697 s),
`ff2b2b64` (701 s) to `ff1e1e57` (704 s) -- a slow animation of the ring itself.  The wide
`searchbar`/`psearchbar` pair at row 150 (four samples) shows the same thing: faint source,
navy frame.

What this does not settle: the retained window holds no `src 0,150` line, so none of the
window server's own surface is in evidence, and the dump labels carry no layer number -- the
lines could be read as either layer's.  The next kernel names the layer in every line and adds
a `pre@Ns/L%u` sample of the frame *before* each layer is drawn into it, which separates what
the last frame left (the window server's layer 1, or a stale panel) from what the blend
produces.  Run 33 carries it.

Two smaller observations.  The last fps line of the run reads `GPU busy -854175 ms (-2845%)`:
hakios-metal's busy-time accounting wraps; it wants a look, it does not affect drawing.  The
run ended by the battery guard at 25% (kernel time 731 s, `battguard: charge 26% ...` then the
three looks; last line 862.9 s) and the phone went back to the loader.

### Run 33: the rim's own row is the same at 186, 190 and 194 s; the frame at it is not (2026-11-20, 15:27 -- 15:41, `logs/152711-run.log`)

Run 33 took the same image (`67c75777b65ffa4b016d7faaeeb21f8ae332fffdb1ff443ff4f3204f7b44f852`)
and loader (`xnu-usb.img`, `213ffaaeb820`) as run 32, and the kernel built at 15:14 with the
layer and surface names in every dump line, `30e252040044`.  The dumps were
`fb: boot tunables: fb_dump=55 fb_dump_every=4 fb_dump_times=140`, 385 characters of cmdline.
`XL_FOLLOW_KEEP=1` copied the console every follow interval to `.live.N`: the log's own ring
keeps only the tail, and that tail begins at 610 s, long after the search bar's row had left
the dumps.  The snapshots are where this run's evidence survives.  The permission dialogs were
answered before the map page was looked at, as asked.

The rim was there to see.  `screen-r33-100.png` is the main map page with the search field's
thick indigo border; `screen-r33-300.png` is the 永辉超市 reviews page, its avatars, stars and
text drawn normally.  The canvas under the search field was black and `正在加载`.

The three samples of the window server's own row are the evidence.  For `L1/s3 row 150`,
`src 0,156` is byte-identical at 186, 190 and 194 s: fifteen `00000000`, then

    04010102 04010202 04010102 04010202 04010202 06010203 06010203 07020203 08020304
    08020304 09020304 0a020305 09020304 ...

-- a premultiplied shadow of alpha 4 to 12, which unpremultiplied is about `#404080` -- then
`c25086c0`, the cyan line `ff66b2ff` four times, `ffdef2ff`, and white from x 36.  The frame
at the same panel row, at the same three seconds:

    186 s: ff013434 ff013434 ff01254a ff212141 ff1d3956 ff1a344d ff182f5e ...
    190 s: ff333366 ff336666 ff24496d ff404060 ff385571 ff334c66 ff2e4574 ...
    194 s: ff797f9b ff79949b ff5b7690 ff65697f ff587089 ff4d647b ff465a7f ...

-- opaque navy at pixels whose only layer content is a shadow of alpha 4.

At each sample the `pre@` line (the frame just before layer 1 is drawn) equals the `at` line
after it: exactly at 186 and 190 s, and at 194 s by about two levels (`ff7b819d` before,
`ff797f9b` after, at x 15).  One pass of the layer moves the row by what a 4/255 shadow is
worth; the row already stands at `ff333366` or `ff797f9b` before the layer is drawn, and the
layer's own row never changed.  The frame's shadow pixels are therefore not this swap's
product: they carry what earlier swaps of the same static row left, one blend at a time.

That is the path `drawRows()` writes.  The frame is persistent (`fShadow`); the bottom layer
repaints only the rectangle it swapped, and a layer above was blended into whatever pixel the
frame already held there -- for a clear source pixel, nothing at all.  A search bar's shadow,
alpha 4 against the bottom layer, applied again every swap, moves the pixel toward the
shadow's own unpremultiplied colour and its alpha toward 255: from black at 186 s
(`ff013434`), from the white page at 194 s (`ff797f9b`).  The row's x 3-14 pixels went
`ff000000` to `ffeaf6fb` between 186 and 190 s, so some layer did repaint the row then; the
dumps of this run do not name which.  Either way the shadow pixels hold the shadow once per
swap, and it is the same alpha-4 shadow, opaque: the thick navy edge.

The change this evidence asks for, built after the run and not yet flown: kernel
`f34461dbb059e988` keeps the bottom layer's pixels by themselves in `fBacking`, mirrored as
the bottom layer draws; a layer above reads its destination there, one blend per swap whatever
the frame holds, and a clear row or pixel shows the bottom layer's pixel again instead of a
stale frame.  The black-screen branch clears it too.  Run 34's dumps and screenshots decide
whether the rim is gone; the cost to watch is one more copy of the bottom layer per swap.

The run ended as written: the battery guard at kernel 730 s (`charge 25%`, then `24%`, three
looks), shutdown at 802 s, `returned 1441823 0`, and the phone went back to Android to charge.
The fps busy-time counter still wraps (`GPU busy -854175 ms (-2845%)`); it wants its own look,
it does not affect drawing.

## Cellular SIM, 4G Data, Settings UI, Two-Way VoLTE Audio (`10099`), CS35L41 Speakers & Global 90 Hz (2026-11-26)

Verified on physical Xiaomi Mi 10S (`thyme`, ADB `5290780a`, China Broadnet `46015` SIM in physical slot 2, RAM boot only with `battery_stop=40`):

1. **SIM Recognition, Carrier Bundle, Settings → Cellular & 4G LTE Data (`PASS`)**:
   - Ported the CommCenter QMUX/Sahara/PDP/IMS bridge (`hakios.thyme_baseband.cpp` + `xnu/files/bsd/dev/{coachz_bb_*,umi_*}`) onto THYME's external SDX55M PCIe2/MHI transport (`hakios.thyme_pcie2.c`), with automatic physical slot 2 → logical slot 1 switch (`UIM 0x0046`) and Primary GW provisioning (`UIM 0x0038`).
   - Added Apple-signed `CMCC_cn.bundle` symlink for China Broadnet (`46015`) in `sysimage/select-system.py` with in-place WDS APN rewrite (`cmnet` → `cbnet`), DSD `0x2a` data arming (`0x0065`/`0x0069`), and `qcrild`-order `WDS 0x00af=1` subscription bind immediately after `WDS 0x00a2` mux bind (eliminating `CM_ACL_FAILURE 3/1042`).
   - Verified in `logs/020956-run.log` (kernel `1d68bd0b254b`, screenshot `logs/cc4-cell1.png`): 4 signal bars + `4G`, Settings → Cellular renders **52 native items** under **`中国广电`** (zero blank page), dual-stack `pdp_ip0` resolves `www.apple.com` (`0.06 s`) and completes HTTPS `200 OK` (`253,027` B in `0.87 s`) over 4G LTE alone.

2. **Two-Way VoLTE RTP Voice Audio on `10099` (`PASS`)**:
   - Parsed `IMSRTP` (`0x28`) `0x0022`/`0x0024`/`0x0025`/`0x0026`/`0x0023`/`0x0027` session TLVs in `umi_ims.inc`, exposed `/dev/hakios-imsrtp` (events) and `/dev/hakios-imsrtp-data` (kernel `pdp_ip1` UDP RTP tap/injector, avoiding `EADDRINUSE` against CommCenter's placeholder port `49120` socket while passing RTCP `49121` through untouched).
   - Implemented userland daemon `userland/src/volte_rtp.c` (`com.hakios.volte-rtp.plist`, `ProcessType=Interactive`, `QOS_CLASS_USER_INTERACTIVE`) with Apache-2.0 AMR-NB/AMR-WB codecs (`tools/mk-amr.sh`), QCI-1 RX media gate (`rx_pkts >= 5`), RFC 4867 pack/unpack (`oa=0` & `oa=1` bit-exact `match=1`), `/dev/hakios-audio` 48 kHz stereo output, `/dev/hakios-mic` 48 kHz mono capture, RFC 4733 DTMF, and periodic RTCP RR.
   - Verified live on China Broadnet **`10099`** in `logs/035640-run.log` (kernel `9ec302c68034`, image `f0335197b22f`, battery `99% -> 98%`, live InCallService UI in `logs/volte7-call.png`):
     - Connected `21.8 s` (`status=1`, `49120 <-> 15822`, AMR-NB `8000 Hz`, `PT=102`, `oa=0`).
     - Downlink RX: **`977/977` RTP packets decoded (`0` bad, `0` dropped, `peak=11648`)**, written to `/dev/hakios-audio` (`19.54 s` at `48001` frames/s, `15.86 s` active voice sound, peak `35%`).
     - Uplink TX: TX gate opened after `1533 ms` (`rx_pkts=5`), sent **`972` AMR-NB RTP packets + `4` RTCP RR (`tx=976`, `0` errors, `mic_reads=972`, `mic_peak=15971`)** from `/dev/hakios-mic`.

3. **Dual CS35L41 Harman Kardon Speakers (`PASS`)**:
   - Fixed missing `adsp_umi=1` in `devices/thyme/ram.args` and defaulted THYME-only `hakios.thyme_sound.c` / `hakios.thyme_adsp.c` to the CS35L41 profile (fixing silent `no /fw/aw882xx_acf.bin -> STOPPED SHORT`).
   - Loaded per-unit factory CS35L41 resistance calibrations (`cs35l41_cal.bin` / `cs35l41_cal_right.bin` in local gitignored `fw-audio/cs35l41/`), lifting HALO-protected digital volume from `-30 dB` bring-up to **`-12.000 dB`**.
   - Verified in `logs/035640-run.log`: both CS35L41 amplifiers started cleanly twice (standalone 1.00 s 523 Hz chime + live 19.54 s `10099` VoLTE voice), delivering **`1,055,552/1,055,552` frames to the DSP with `0` underflows and `0` errors**.

4. **Global Stable 90 Hz Refresh Rate (`PASS`)**:
   - Fixed `hakios_fb_publish_refresh()` in `MiniIOMobileFramebuffer.cpp` to re-run `publishModes()` alongside `publishRefresh()`, so `CADisplayMode.refreshRate = 90` and `UIScreen.maximumFramesPerSecond = 90` (previously stuck at 60).
   - Exported `CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0` from `userland/src/init.c` to `launchd`, defaulted `anim_max=1` in `inject_sb.c`, added kernel `@apps` exec-injection support (`kern_exec.c` / `xnu/hakios.patch`), and added lightweight `/redwood/lib/libhakios.hfr.dylib` (`inject_hfr.c`) so interactive `>= 48 Hz` `CADisplayLink` and `CAAnimation` requests across UIKit apps run at 90 Hz instead of dividing 90 Hz down to 45 Hz (while preserving `< 48 Hz` timers such as the 10 Hz clock).
   - Verified in `logs/032733-run.log` & `logs/034010-run.log`: steady `11.11 ms` (`90.0 Hz`) `tl: a` pacing, `CADisplay`/`CADisplayMode`/`UIScreen` all reporting `90 Hz`, and `hakios: hfr 90Hz active` confirmed on `@apps` launch with zero crashes.

| File | SHA-256 |
|---|---|
| `logs/020956-run.log` | `0378e34f177f4bbbfe0efdd48c376b01b000dfb9bdae6dc7364673c20eb05da4` |
| `logs/cc4-cell1.png` | `106cc4c26b579250e6668798f1ae6dfc355ee5b51ce671c3c6fc867897547d59` |
| `logs/025607-run.log` | `841de12c2857c0d2657e896280e3ca3fd7fc3a702fc073b201cd6441004b1383` |
| `logs/volte6-call.png` | `4249aa4863826043033dcda411e8c0675377543ca1cf656f79af9605ec883e30` |
| `logs/032733-run.log` | `82ff826beb1f26aee8aea86ccd1dbc49c219470ee147f0f7932a3cdc8cfb3885` |
| `logs/volte7-call.png` | `f4f4fb100cafe211d1a363408656de7891a62150c0e1d7c1484053ff1b6b6198` |
| `logs/034010-run.log` | `14835499bb28e93e3394c426c4f3feb9305c9927bca0e8848c220fc2e0838da1` |
| `logs/035640-run.log` | `f0cfd5f5304b9e726bc12068272f47d86dde0a7339ad08653beed9076191c53e` |

## 2026-11-26 Camera Sharpness/Switching Overhaul & Full Checklist Audit (`5290780a`)

All fixes below were verified live via RAM boot (`./xl`, `battery_stop=40` active throughout, zero partition writes, returned cleanly to Android):

1. **Camera Sharpness, AK7377 Autofocus, Anti-Ghosting & Lens Switching (`PASS`)**:
   - **AK7377 Lens Motor (`hakios.thyme_umi_camera.c`)**: Added required `ums(5)` wake delay after `VCM_REG_CONTROL = 0x00`, 3-attempt retry in `uvcm_position()`, and corrected `VCM_OPTIMAL` from `343` to `1152` (`0x4800`). Hardware log (`logs/091824-run.log`, `logs/092759-run.log`): `AK7377 at 0x18: writes taken, control 0 position 0x4800 (read back)` (previously `writes REFUSED` on 154/158 boots).
   - **AF, ISP & Anti-Ghosting (`camera_device.c`, `camera_isp.c`, `hakios.thyme_camera_dma.c`)**: Adaptive `1x1`-cell `isp_sharpness()`, fixed `AF_SWEEP` range (`256..3600`) & peak-before-early-stop guard + shutter AF-wait; replaced unconditional 8-frame still accumulation (motion ghosting) with 1-frame capture in normal light (`gain < 12x`) + motion-checked 3-frame low-light burst; upgraded still demosaic to 5x5 integer Malvar-He-Cutler + 2D 3x3 coring unsharp mask; defaulted rear camera to `cam_mode=1` (`res1` `6016x4512` 27 MP native Tetracell 2x2, ~26 fps viewfinder); skipped redundant cache clean on `real_source` reads & single-bank DMA invalidation.
   - **Lens Switching & Startup AE (`hakios.thyme_camera.c`, `camera_device.c`, `camera_isp.c`)**: Removed `stream_begin()` thrash from `device_request_streams()`, added atomic `HAKIOS_CAMERA_SENSOR` slot invalidation & read gating, bumped `IDLE_MS` `1000 -> 4000` ms, and added moderate startup exposure (`frame_length/2, 3x`) + fast startup AE (`ae_frames < 15`, `ae_hold=4`), eliminating triple sensor power-ups, mid-shutter restarts, and the 4 s Front Camera white-out flash (`logs/092759-run.log`, `camfix3-still-label100.png`, `camfix3-t134.png`, `camfix3-t170.png`).

2. **Display Backlight Blanking & Multi-Level Brightness Control (`PASS` manual + blank/wake; ALS remains `PARTIAL`)**:
   - **Audit Finding**: Prior lock/blank (`Set Blanked: YES`) left `DCS 0x51` unchanged because `inject.c` only intercepted `DisplayBrightness`, ignoring `DisplayBrightnessFactorWithFade`, and `permille_to_level()` never sent `0`.
   - **Fix (`hakios.thyme_display.c`, `userland/src/inject.c`)**: Added `kern.hakios.backlight_factor` (`0..1000`, `0 -> DCS 0x51 = 0x0000` with 3x retry) and forwarded CoreBrightness `DisplayBrightnessFactorWithFade` / `DisplayBrightnessFactor` from `BrightnessSystemInternal`.
   - **Verified (`logs/101918-run.log`)**: Control Center slider live drag swept `485 -> 265 (level 1097) -> 971 (level 3977) -> 857 (level 3512)` with `45/45` consecutive `DCS 0x51` `(ok)` completions (`cc-slider-high.png`, `cc-slider-70.png`); power-button lock and idle blank both sent `DCS 0x51 level 0 (ok)`, and wake restored exact user brightness `level 3512 (ok)` while preserving 90 Hz (`logs/100533-run.log`, `logs/101254-run.log`).

3. **Dual-Speaker Independent Stereo L/R Acoustic Separation (`PASS`)**:
   - Added `sound_channel=3` sequential Left/Right self-test (`hakios.thyme_sound.c`) and dual-channel `rec_sound_mic=1` preservation (`hakios.thyme_mic.c`), plus `0 Hz` channel-muting support in `sound_test.c`.
   - **Verified (`logs/101157-run.log`, `logs/stereo-lr2.wav`)**: Both CS35L41 amps loaded per-unit calibrations (`0x40` bottom/right `R=8942`, `0x42` top/left `R=8534`); 1 kHz Goertzel at bottom mic `ADC1` rose from `-60.30 dBFS` baseline to `-47.47 dBFS` (`+12.83 dB`) on 3 s Left-only, returned to `-62.26 dBFS` during the 2 s silence gap, and rose to `-42.04 dBFS` (`+18.27 dB`, `+5.44 dB` higher at bottom mic than top speaker) on 3 s Right-only (`159/159` periods each, `0` underflows).

4. **Global 90 Hz `@apps` Proof & Consolidated Regression Audit (`PASS`)**:
   - Updated `userland/src/inject_hfr.c` to log activation to `/dev/console`; verified live `@apps` injection in `MobileSafari`, `Preferences`, `MobileCal`, and `identityservicesd` (`logs/095159-run.log`, `logs/100533-run.log`) and 90 Hz retention across Display Sleep/Wake (`logs/100533-run.log`, `logs/101254-run.log`).
   - Re-verified Wi-Fi TLS (`en0` `192.168.0.103`), Bluetooth 3 Mbaud H4 (`0` faults), CoreHaptics on AW8697 (`6` taps + `277 ms` continuous vibration), dual-mic `mic=4`, and battery charging telemetry (`99% -> 94%`, `+653..+1113 mA`) in `logs/095159-run.log` & `logs/100533-run.log`.
   - Reconciled stale contradictions across `CHECKLIST.md` summary tables while keeping genuinely unfinished items (SLPI sensors/ALS/proximity, Metal VectorKit/system `MTLDevice`, BT HFP/reconnect, Wi-Fi SystemSleep wake, flash persistence) honestly marked `PARTIAL`/`TODO` and SMS `DEFERRED`.

| File | SHA-256 |
|---|---|
| `logs/092759-run.log` (Camera 27 MP + AK7377 + switching) | `54e691555cc3ddde0bfd11573f91d1836b06e860c0c4b810f1a1dcdf5bdf7df3` |
| `logs/095159-run.log` (Consolidated regression + HFR + haptics + Wi-Fi/BT) | `06b81527edcf39c09f8812a3e8cbe957e3dde6a8c9ab65cdd475b9c5ee57ac9e` |
| `logs/101157-run.log` (Dual-speaker stereo L/R test) | `c99374fd702bd035ac73272f7788fd92ed85fa92bfe47d4b7a3d48894b536b52` |
| `logs/stereo-lr2.wav` (Dual-speaker stereo L/R acoustic recording) | `57cae7fd384a89c7f78225b3271fb725bba02978ba7c1e3d1080f0b731a58bf8` |
| `logs/101254-run.log` (Display lock/wake + 90 Hz retention) | `065588ffd889c55531f997a720f4c2d425e1bee3d565b2df2e6399bb54215d2f` |
| `logs/101918-run.log` (Control Center brightness sweep + lock/wake `DCS 0x51 = 0`) | `24a06738499055bf00be864cb0892230bcc7b441293d8940fbc6d5cdbc1b2f1f` |
| `out/thyme-boot/root-wirelessfull.apfs` (canonical rebuild) | `909c0939aaa296071e3a2990f06b14f7628fadef4a31ace607ba9850bb97c4ae` |
### Simplified Chinese Default UI, Settings Location Services UI, SDX55M Hardware GNSS Bring-Up & i4Tools Audit (2026-11-26)

1. **Default System Language = Simplified Chinese (`PASS`)**:
   - Added `userland/var/mobile/Library/Preferences/.GlobalPreferences.plist` (`AppleLanguages = ["zh-Hans-CN", "en-US"]`, `AppleLocale = "zh_CN"`, Simplified Pinyin / QWERTY / Emoji keyboards), packaged into `var.skel` and seeded by `init` before `launchd` starts `cfprefsd`/`SpringBoard`.
   - Verified cold boot directly in Simplified Chinese across SpringBoard Home Screen (`天气`/`日历`/`相机`/`设置`/`搜索`), Settings → `隐私与安全性` → `定位服务`, and CoreLocation permission prompts with zero tofu glyphs (`logs/104116-run.log`, `logs/105145-run.log`).

2. **Settings → `隐私与安全性` → `定位服务` UI & Live Toggle (`PASS`)**:
   - Updated `userland/src/inject_prefs.c` to resolve localized `LOCATION_SERVICES` from `PrivacySettingsUI.framework`'s `Location Services.loctable`, attach the native `locationServicesEnabled:` getter (live `打开` / `关闭`), attach the native blue `Location` icon (`iconImage`), and include `"location-services"` in `ensure_capability_override()`.
   - Verified live on `5290780a` (`logs/105145-run.log`): `隐私与安全性` renders the native blue arrow row `定位服务  打开 >`, opens stock `PUILocationServicesListController` (master switch, `位置提醒`, `轻 App`, `地图`, `系统服务` with purple status arrow), and toggling OFF/ON stops and restarts the kernel GNSS lease live. Wi-Fi CoreLocation positioning re-verified via `LocProbe.app` (`+/- 52..69 m`, 5 consecutive fixes, `loc_test: locationServicesEnabled = 1`).

3. **SDX55M Hardware GNSS (`QMI LOC 0x10`) Bring-Up & `umi_gnssd` Bridge (`PASS` engine/session/SV streaming; outdoor 3D fix pending)**:
   - Added root LaunchDaemon `userland/src/umi_gnssd.m` (`com.hakios.gnss.plist`, entitled `com.apple.locationd.simulation=true`) bridging `kern.hakios.gnss_enable` / `kern.hakios.gnss_fix` to `locationd` via `CLSimulationManager`, obeying `LocationServicesEnabledIn8.0` and falling back cleanly to Wi-Fi positioning when no fresh satellite fix is present.
   - **Root Cause of `QMI_LOC_START (0x22)` Error `46` (`QMI_PROTOCOL_ERROR_GENERAL_ERROR`)**: SDX55M firmware defaults clients without client-type TLVs in `QMI_LOC_REG_EVENTS (0x21)` to NFW (Non-Framework) and rejects `QMI_LOC_START` with error `46`. Adding Qualcomm `qmiLocRegEventsReqMsgT_v02` AFW client TLVs (`0x10 = "MHAL"`, `0x11 = 1 / eQMI_LOC_CLIENT_AFW`, matching upstream `libqmi` `532de37`), `QMI_LOC_SET_OPMODE(STANDALONE=4)`, optional-empty `0x10` SV list handling in `ugl_satellites()`, and immediate `UMI_GNSS_VIEW` reporting on SV indications (`xnu/files/bsd/dev/umi_gnss_qmi.h`, `xnu/files/bsd/dev/umi_gnss.inc`) resolved the refusal without touching modem NV/EFS.
   - **Verified (`logs/113308-run.log`, `logs/114028-run.log`, using `devices/thyme/prebuilt/xnu-usb.img` + `pcie2_mhi=2`)**: `0x21 -> 0`, `0x4a(STANDALONE) -> 0`, `0x22 -> 0`, `FIX_SESSION_STATE=1 (STARTED)`, `ENGINE_STATE=1 (ON)`, continuous 1 Hz `UGL_SV` telemetry (`satellites in view 0; fixes 0`, `malformed 0`, zero lease cycling on canonical `root-wirelessfull.apfs`). Indoor desk reception shows `0` satellites in view (matching Android `GnssDfsManagerImpl: svCount=0` on the same desk); outdoor open-sky 3D fix delivery remains to be verified outdoors.

4. **Built-in 爱思助手 (i4Tools) iOS App Audit (`BLOCKED` by Apple FairPlay DRM `cryptid=1`)**:
   - Inspected local PC client IPAs (`C:\Program Files\i4Tools9\files\ipa\{220.ipa,217.ipa}`): both (`Runner.app` v2.2.0 `com.ownbook.notes` and `AsTools.app` v2.1.7 `rn.notes.best`) carry `LC_ENCRYPTION_INFO_64: cryptid=1` (FairPlay App Store DRM). Official `i4.cn` / `apt.i4.cn` do not distribute an unencrypted (`cryptid=0`) arm64 IPA. HakiOS on Snapdragon 870 has no Apple SEP FairPlay hardware decryption, so `cryptid=1` binaries cannot execute. Ready to bundle either a user-supplied decrypted (`cryptid=0`) arm64 IPA or pre-install verified `cryptid=0` Chinese-UI `TrollStore` + `TrollApps` (`org.resonance.store`).

| File | SHA-256 |
|---|---|
| `logs/104116-run.log` (Default Simplified Chinese UI boot) | `2e4d2e2981332fe63673bb6052df39374fdaca68d8b551da8faad3f7d105e952` |
| `logs/105145-run.log` (Settings `隐私与安全性` → `定位服务` icon/toggle + Wi-Fi `LocProbe` fixes) | `7c9622fb2aad8c56c78755097e5d93980c95949f633f033d8b88c02150f8d278` |
| `logs/113308-run.log` (SDX55M QMI LOC AFW bring-up: `START=0`, `ENGINE_STATE=1`, 1 Hz SV) | `145eb9674e5d5bc9414f34f2e91ff9e8b08a96269057491b6bf2d0a6461ce8a8` |
| `logs/114028-run.log` (Canonical `root-wirelessfull.apfs` + polished `umi_gnss` zero-cycle soak) | `363fb53395022597c6d363b21244adfd9ca64c9aab4aafd2172e6ebabb138530` |
| `hakios-kernel` (16,944,256 B) | `506aef22676a25256646b1a89e39a7af27deff5d464c4938541d44d91c5ea042` |
| `out/thyme-boot/root-wirelessfull.apfs` (48,984,064 B; total 65,928,320 B < 66,060,288 B) | `a2e5272fe46417ada6ad620f7bfe18a774c46b2151871e5664a4fc35e06c5a93` |

## 2026-11-26 Hardware 90 Hz TE-Sync Anti-Ghosting Fix & Baidu Maps Live Location Verification (`5290780a`)

All fixes and app tests below were verified live via RAM boot (`./xl`, `XL_BOOT_IMAGE=devices/thyme/prebuilt/xnu-usb.img`, `battery_stop=40` active throughout, zero partition writes, phone returned cleanly to Android):

1. **Root Cause & Fix of Scroll Ghosting / Refresh Rate Mismatch (`PASS` — true hardware 89.1 Hz / 90 Hz, `0.00%` missed TE scans)**:
   - **Diagnosis (NOT GPU)**: Adreno 650 (`hakios-gl` / `HakiosMetalCA`) completed every frame in `0.23–1.6 ms` with `0` GPU drops. Scroll ghosting came from two display-pipeline defects:
     1. **Unsynchronized command-mode VRAM copies**: `MiniIOMobileFramebuffer` disabled `fb_beam` on `thyme`, so `blitPanel()` overwrote live UEFI VRAM mid-DMA transfer without TE synchronization.
     2. **Panel hardware stayed at `59.39 Hz` while `fb_hz=90` paced 90 fps into it (3:2 judder + ghosting)**: Bootloader cmdline selects `qcom,mdss_dsi_j2s_mp_42_02_0a_dsc_cmd` (`timing@1` 90 Hz = `F0 + B4 + 2f 04 + 26 02`), and SM8250 DSI TPG command-DMA FIFO silently ignored `dsi_send_short()` 1-word short packets (`2f 04` / `26 02` never reached the panel; verified live in `logs/135842-run.log` where `dsi_send_short` left TE at `59.43 Hz` while `dsi_send_long` immediately changed TE).
   - **Fix (`devices/thyme/drivers/display/hakios.thyme_display.c`, `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`)**:
     - Added read-only SM8250 DPU 6.0 `INTF_1` (`0x0ae6b800`, 16 KB aligned mapping) command-mode TEAR synchronization (`INTF_TEAR_INT_COUNT_VAL` `0x298` + `INTF_TEAR_LINE_COUNT` `0x2b0`) exposing `hakios_display_command_mode()`, `hakios_display_scan()`, `hakios_display_next_frame_ns()`, and `hakios_display_wait_copy()`.
     - Switched `send_panel_timing_cmd()` to deliver the full `j2s_mp_42_02_0a` 90 Hz sequence (`F0`, `B4`, `2f 04`, `26 02`) via proven `dsi_send_long()` (`0x39`) inside the DMA-idle window (with `j2s_mp_42_02_0b` `2f 02` fallback + auto-restore guard), verifying real hardware TE via `thyme_command_te_calibrate()` before publishing `hakios_panel_refresh_hz`.
     - Wired `MiniIOMobileFramebuffer` (`show()`, `settle()`, `armVSync()`) to lock VSync to real panel TE with an `< frame_ns / 2` double-VSync guard and smoothed 90 Hz DMA-idle copy budget (`fCmdCopyNs`).
   - **Verified Live (`5/5` consecutive boots on `5290780a`)**:
     - Real `INTF_1` hardware TE switched cleanly from **`59.39–59.47 Hz` (`2374–2377` phase lines)** to **`89.08–89.17 Hz` (`1583–1584` phase lines, DMA idle after line `1197–1198`)** on every boot (`logs/141412-run.log`, `logs/142439-run.log`, `logs/143544-run.log`, `logs/144523-run.log`, `logs/145516-run.log`).
     - Sustained 90 Hz rendering (`logs/142439-run.log`, kernel `d1642a54e4ff`): **`446 frames in 5008 ms (89.1 fps)`**, **`0 of 446 too late for the next scan`**, `0` under 10 ms (zero double-VSync), **`446/446` frames in `10–12 ms` (`P50 < 12 ms`, `P95 < 12 ms`, `0` pauses, `0` gave up)**, `blitPanel()` copy `1.00–1.07 ms` inside the `2.73 ms` DMA-idle window. Active Baidu Maps animation (`logs/144523-run.log`, `logs/145516-run.log`): `442 frames / 5010 ms (88 fps)`, **`0 of 442` / `0 of 192` / `0 of 47` / `0 of 45` missed scans**.

2. **Third-Party Map & Location Verification — Baidu Maps (`com.baidu.map` v22.2.20, `PASS`)**:
   - Audited operator-provided `百度地图-路线规划出行必备.ipa` (`207,202,418 B`, SHA-256 `299ea64e50d1841499b64822eae236ea0fed7323fea88010fe7a6ea20363ac43`): `IphoneCom.app` (`com.baidu.map` v22.2.20, arm64, `LC_ENCRYPTION_INFO_64: cryptid=0`, unencrypted).
   - Fixed auto-lock / auto-dim wake & unlock handling and `新人福利` modal dismissal in `userland/rc.jobs.mapdiag`; built `out/thyme-boot/root-mapdiag.apfs` (`SCENE=mapdiag METAL_PLUGIN=ca`, `49,090,560 B`, payload total `66,051,696 B < 66,060,288 B`).
   - **Verified Live (`logs/143544-run.log`, `logs/144523-run.log`, `logs/145516-run.log`, with full SDX55M modem+GNSS + Wi-Fi + Metal `metal=1 metal_apps=1`)**:
     - Downloaded over LAN (`192.168.0.103 -> 192.168.0.104:8000`, HTTP `200`, SHA-256 verified), installed via `trollstorehelper install custom`, registered via `uicache -a`, and launched in Simplified Chinese iOS with active China Broadnet (`46015`) cellular registration and SDX55M QMI LOC GNSS (`gnss: satellites in view 1; fixes 0` indoors in `logs/143544-run.log`).
     - Live framebuffer screenshots (`bmap4-t205.png`, `bmap4-t225.png`, `bmap4-t245.png`) confirm: solid CoreLocation arrow active in the status bar, crisp HD vector map rendering, and accurate blue location dot + heading cone locked directly onto the real Xi'an location (`陕西省西安市长安区郭杜街道...`, ~92 m indoor Wi-Fi CoreLocation accuracy, `locationd` + `BaiduMap` live tiles/POI cards functional, zero crashes).

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,961,136 B, `INTF_1` 89.1 Hz TE-sync + `armVSync` fix) | `d1642a54e4ff4758ae6b34053e69ad2361b7f66664440fbcbec2178b4790decb` |
| `logs/141412-run.log` (First hardware `59.39 -> 89.12 Hz` TE switch on Panel `0A`) | `ae1f74dbfcd8a8e114037fc20ca782cd8b1a75ad71e43e9c56bf120ef3974700` |
| `logs/142439-run.log` (Sustained `89.1 fps`, `446/446` in `10–12 ms`, `0/446` missed scans) | `0950afb4d5ee8f3db324e65b3ef22f35232df6c26e6e41d88e2e0ec4f91d3013` |
| `logs/143544-run.log` (SDX55M GNSS `satellites in view 1` + `0/289` missed scans) | `b631a819b47a855ec5f09ea184268c8a658c8f67d2a2baca9de4228e9e1abbc4` |
| `logs/144523-run.log` (Unlocked Baidu Maps `88 fps`, `0/442` missed scans, active CL arrow) | `1aea7a925d4a8bc5ae4d3e57e126a9ffa15e4971fdb4da9ded893785730bd3e7` |
| `logs/145516-run.log` (Clean live Baidu Maps Xi'an blue-dot fix + `0/192` missed scans) | `67b8ac35963748a291cadef233ff1134a9ce8984c2dc5a2626550e800986f622` |

### Sileo, TrollStore, TrollApps Out-of-the-Box CN Setup & SDX55M 5G NR Telemetry (2026-11-26)

- **One-shot boot setup (`com.hakios.pkgsetup.plist` + `userland/bin/hk-pkgsetup.sh`)**: Runs automatically on every boot (including canonical `wirelessfull`) to bootstrap `/var/jb/usr/libexec/firmware`, seed 10 curated mainland-China-accessible Rootless Sileo repos (`userland/share/sileo/sileo.sources`, `Trusted: yes`) beside Procursus (**11 total repos**), write `org.coolstar.SileoStore.plist` (`CanisterIngest=false`, `ShowProvisional=false`, `UseSystemLanguage=true`), create TrollStore's MCM container with executable `ldid` + `ldid.version` (`v2.1.5-procursus7-new`) in both `/var/jb/Applications/TrollStore.app` and `/var/containers/Bundle/Application/*/TrollStore.app`, seed `com.opa334.TrollStore.plist` and `org.resonance.store.cn.plist` (Simplified Chinese + 6 CN-accessible/accelerated TrollApps JSON sources), and register via a single `uicache -a`.
- **Sileo UX & queue fixes (`userland/src/inject_app.c` + `userland/build.sh`)**: Disables Canister `CanisterIngest` / `ShowProvisional` cleanly on main queue, auto-skips/dismisses `_TtC10Onboarding24OnboardingViewController`, injects a native `"完成"` (`UIBarButtonSystemItemDone`) button on modal `_TtC5Sileo27NativePackageViewController` sheets, and bakes `/sileolists -> /var/jb/var/lib/apt/sileolists` into `system.apfs` (`build-root.sh` also auto-enables `SDX55=1` when `SYSTEM=1`).
- **Live hardware verification (`logs/161515-run.log`, `logs/162623-run.log`)**:
  - `apt-get update` fetched 1,050 kB across all 11 repos (`Reading package lists... Done`, zero `E:` errors).
  - `trollstorehelper install custom /redwood/fw/ipa/LocProbe.ipa` returned `0`; TrollStore Settings shows `ldid: Installed (v2.1.5-procursus7-new)` with no spinner or update nag (`pkg3-t105.png`).
  - TrollApps renders in Simplified Chinese with all 6 CN sources populated (AltStore 6, Bonnie 1, Resonance Team 2, SideStore Team Picks 23, WuXu's Library 36, haxi0 6; `pkg4-t132.png`).
  - Sileo renders in Simplified Chinese with zero Canister modal, all 11 repos + icons (`pkg4-t160.png`), and browsable package lists (`82Flex`: 33 packages, `pkg4-t180.png`).
- **SDX55M 5G NR telemetry (`xnu/files/bsd/dev/umi_voice.inc` + `devices/thyme/drivers/modem/hakios.thyme_baseband.cpp`)**: Read-only 60 s `NAS5G:` heartbeat confirms `mode=80 (0x50 = LTE|NR5G)`, `nr5g_dis=-1`, `lte_svc=2`, `nr5g_svc=-1`, `endc=-1`, `dcnr=-1` (`logs/162623-run.log`). Although Mi 10S hardware (`SDX55M`) camps on 5G `NR_SA` (ARFCN `504990`, Band n41) under Android, HakiOS boots arm64 `iPhone10,3` (`Mav17` / MDM9655 4G-only identity, `capabilities::bb::radio::isMav20Plus() == false`), so 5G NR UI/control requires a future `Mav20` baseband bridge & firmware manifest.

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,961,632 B, always-on `NAS5G` heartbeat) | `84426a9af83d6c425e2b76db0fc23a51c3ac0d6f7a7bd0177c26a99eed0df235` |
| `out/thyme-boot/root-wirelessfull.apfs` (48,988,160 B; sum 65,949,792 B < 66,060,288 B) | `c107456ba728d41c7a32d102411c727a0970bb16cde9c4ed496e096b89fc1595` |
| `logs/161515-run.log` (`pkgtest3`: TrollStore `ldid`, `apt-get update`, `NAS5G`) | `783a6d94f6f2b8bebd26c46dc70fb34a9708f5acd28fc7997ec9ef9aa482c5a9` |
| `logs/162623-run.log` (`pkgtest4`: Sileo 11 repos + `82Flex` 33 pkgs, TrollApps 6 CN sources, `NAS5G`) | `c0683bc377b7c582d6f85429660819c951b6e588488851c3f02dd168c0822bc7` |

### Wi-Fi Sleep/Wake, AP Deauth Auto-Rejoin & `wifid` Locale/Deauth IOCTLs (`PASS`, 2026-11-26)

- **Kernel Wi-Fi recovery (`wifi/lx/src/lx_80211.c`, `devices/thyme/drivers/wifi/hakios.thyme_sta.c`, `devices/thyme/drivers/wifi/hakios.thyme_apple80211.c`)**:
  - Wired `linuxkpi_ieee80211_connection_loss()` to clear `radio->joined`, set `ETIMEDOUT`, and notify `lx_os_net_link(0)` + `lx_os_station_lost(ETIMEDOUT)` (previously log-only).
  - Added generation-guarded `thread_call_t` (`rejoin_call`) automatic kernel standing-network rejoin in `hakios.thyme_sta.c` on involuntary link loss (`hakios_sta_lost`, 2 s initial delay + 5/10/30 s retry backoff, also retrying transient power-on rejoin failures; cancelled cleanly on explicit leave/power-off/join/detach), keeping Wi-Fi self-healing even for Settings-UI-joined networks or suspended userland.
  - Implemented `APPLE80211_IOC_COUNTRY_CODE` (`get 51` / `set 51`, default `"CN"`, eliminating `wifid`'s repeated locale retry errors) and `APPLE80211_IOC_DEAUTH` (`get 29`), and fixed `HAKIOS_STA_LINK` `link.reason` from hardcoded `4` (`SystemSleep`) to `1` (`Deauth`) / `2` (`BeaconLoss`) / `0` (`None`).
- **Keepalive & automated hardware sleep/wake verification (`userland/src/net_test.m`, `devices/thyme/drivers/input/hakios.thyme_keys.c`, `userland/rc.jobs.wifisleepwake`)**:
  - Upgraded `net_test keepalive` to inspect `SIOCGA80211` `POWER (19)` and `STATE (13)` so it respects manual Wi-Fi OFF, avoids double-joining during kernel auto-rejoin, retries across periodic timeouts, and renews DHCP if IPv4 is absent.
  - Added optional `press_power_gap=S` boot-arg to `hakios.thyme_keys.c` for unattended multi-second lock/wake hardware button testing (`press_power=80 press_power_gap=65`).
- **Live hardware verification (`logs/171727-run.log`, `logs/173008-run.log`, zero panics, zero SMMU faults, returned cleanly to Android)**:
  - `wifid` settled locale `<CN>` on first attempt (`get 51`: `8/8`, `set 51`: `1/1`, `get 29`: `2/2`, zero retry errors).
  - Real 65 s locked/blanked sleep (`17:31:35` `DCS 0x51 level 0`, `Set Blanked: YES` -> `17:32:40` `Main will unblank`, `Set Blanked: NO`, `DCS 0x51 level 2056`): immediately post-wake `en0` held `192.168.0.103`, DNS resolved `www.apple.com` in `0.01 s`, and HTTPS returned `200 OK` (`69 bytes in 0.08 s`).
  - Radio power cycle (`power 0` -> `power 1`) auto-rejoined via kernel standing network (`HTTPS 200 OK` in `0.10 s`); explicit `leave` auto-rejoined via keepalive (`0.5 s`, `HTTPS 200 OK` in `0.10 s`); spontaneous AP deauth (`logs/171727-run.log`, `reason 3`) triggered kernel 2 s auto-rejoin (`2.68 s` total, zero double-join, `HTTPS 200 OK` in `0.14 s`).

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,961,920 B, Wi-Fi auto-rejoin + `COUNTRY_CODE`/`DEAUTH` + `press_power_gap`) | `c5dcdb1e065d2774d8c26b561df37899a0ed84fdde7c83a7a3932974272470c1` |
| `out/thyme-boot/root-wirelessfull.apfs` (48,988,160 B; sum 65,950,080 B < 66,060,288 B) | `0e129d00603d134ddca564cb956b2ea43eb312cc39919a6f609d7c743c579791` |
| `out/thyme-boot/root-wifisleepwake.apfs` (48,988,160 B; sum 65,950,080 B < 66,060,288 B) | `533b28cde9ba6a4c922c8ae5fb3dad5ef3573605506f05ea04c63ca7c9f41a24` |
| `logs/171727-run.log` (Spontaneous AP deauth 2.68 s kernel auto-rejoin + power/leave HTTPS `200 OK`) | `9f05475e28a500c95ff9c2018a826364447c7319cb55ca50e842afb0872ab417` |
| `logs/173008-run.log` (Full 65 s lock/wake -> post-wake HTTPS `200 OK` + power/leave + `CN` locale `PASS`) | `e2438360013e557038bba977b058128a2217d3da7094dbd7f3df6d3d46766d8c` |

### SLPI 5-Sensor End-to-End Bring-Up (`PASS`) & Bluetooth `BlueTool`/`AVAudioSession` Fixes (2026-11-26)

- **SLPI FastRPC & Sensor PD Root-Cause Fixes (`devices/thyme/drivers/sensor/hakios.thyme_slpi.c`, `devices/thyme/drivers/display/hakios.thyme_display.c`, `userland/build.sh`)**:
  - Exiting `step_start()` immediately on `DSP_READY | DSP_HANDOVER` (avoiding the 3,000 ms spin) and bringing up `fastrpc_early` before `SNS_REG_INIT` (~290 ms window) reduced `adsp_default_listener` registration to `+141.854 ms` (`+147.598 ms` in `logs/174603-run.log`).
  - Added image-backed `apps_std` `stat` (`31`) and `opendir` (`26`) returning POSIX `ENOENT (2)` on miss, fixed `apps_std_readdir` `char name[256]` struct offset (`is_eof` at `+0x104` instead of `+0x103`), served Mi 10S under-OLED TCS3701 sysfs nodes (`panel_info`, `max_brightness`, `dynamic_fps`, and dynamic `hakios_display_backlight_level_2047()`), and exported pure-integer `ieee754_to_milli()` sensor snapshots via `kern.hakios.sensor_state` (`version = 2`).
  - Staged `fw-slpi/slpi.*` into `sysroot/fw/` on `SYSTEM=1` builds in `userland/build.sh`.
- **Virtual IOHIDService Delivery in `backboardd` (`userland/src/inject.c`)**:
  - Spawned `sensor_thread` polling `kern.hakios.sensor_state` and creating virtual `IOHIDService` instances for `HakiOS accelerometer` (`0xff00/0x0003`), `HakiOS ambient light` (`0xff00/0x0004`), `HakiOS magnetometer` (`0xff00/0x0006`), and `HakiOS gyroscope` (`0xff00/0x0009`), using disassembled iOS 16.5 `IOKit` signatures (`IOHIDEventCreateAmbientLightSensorEvent(NULL, ts, lux, 0)`, `IOHIDEventCreateAccelerometerEvent`, `IOHIDEventCreateGyroEvent`, `IOHIDEventCreateCompassEvent`).
- **Live Verification (`logs/174603-run.log`, `logs/180216-run.log`, `logs/181653-run.log`, zero panics, zero SMMU faults, returned cleanly to Android)**:
  - `55/55` SNS responses with `result 0`, `0` refusals (`logs/181653-run.log`).
  - Live hardware readings delivered to `backboardd` with `ok=1`: `accel` (LSM6DSO, `1,440` samples, `g=(-0.024, -0.918, -0.410)`), `gyro` (LSM6DSO, `1,440` samples with `GyroProperties` / `GyroExtLevelTriggerSync`, `dps=(0.115, -0.344, 1.432)`), `mag` (AK0991x, `563` samples, `uT=(7.58, 55.29, 160.50)`), `ambient_light` (under-OLED TCS3701, `lux=163 -> 148`, `raw=136 -> 124`), and `barometer` (BMP285, `56` samples, `957.65 hPa`).
  - Android `dumpsys sensorservice` on `5290780a` confirms Mi 10S has no SLPI IR proximity sensor (`Xiaomi Proximity` is ultrasonic over ADSP).
- **Bluetooth `BlueTool` / H4 / `AVAudioSession` Fixes (`userland/src/umi_bluetool.c`, `devices/thyme/drivers/bluetooth/hakios.thyme_bluetooth.c`, `userland/src/sound_test.c`)**:
  - Updated `umi_bluetool.c` to accept `RunBuiltinScript` `script="deepsleep"` via `controller_ready()` (replacing `result=45` `ENOTSUP`, since QCA6390 IBS is handled by kernel UART).
  - Reset `waking=false` on H4 generation change in `hakios.thyme_bluetooth.c` and fixed the transport summary format string to include SCO (`bt_tx_packets[3]`) counters.
  - Updated `userland/src/sound_test.c` (`rc.jobs.slpibt`) to test `PlayAndRecord` alongside `Playback`: live log (`logs/181653-run.log`) confirms `PlayAndRecord` accepts `A2DP` (`0x20`), `HFP` (`0x04`), and `A2DP+HFP` (`0x24`) (`yes`), while `Playback`-only refuses those options (`OSStatus -50`).

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,964,312 B, 5 SLPI sensors + dynamic OLED backlight + BT H4 fix) | `691ee1a53b745a766acc93248e0fa10b037e3191b9ec2cd34109dca58be8e3b2` |
| `logs/174603-run.log` (SLPI Stage 1: `+147.598 ms` FastRPC, `31/31` SNS `result 0`, `lsm6dso` stream) | `5ecc76b92a66d8c577b99d7e3df73e54ff6a77a32d782c8a11c3583c7cdc71d0` |
| `logs/180216-run.log` (SLPI Stage 2: `+141.854 ms` FastRPC, accel + OLED sysfs ALS -> `backboardd`) | `5143b4ba7d60682b6e97b1c656bec516030304641ec2a907a1b85da370f87f41` |
| `logs/181653-run.log` (SLPI 5-sensor `55/55` SNS `result 0` -> `backboardd` HID + `PlayAndRecord` HFP/A2DP `yes`) | `7161c7ade72d2446f78da02b841e7504b8541b84645276bac5d89fab9324fde2` |

### Unified Plug-and-Play `slpi=1` Daily-Driver Verification (`ram.args` + `system.apfs` + `root-wirelessfull.apfs`, 2026-11-26)

- **Kernel & daily cmdline defaults (`devices/thyme/drivers/sensor/hakios.thyme_slpi.c`, `devices/thyme/ram.args`)**:
  - When booting in `slpi=1` production mode (`last_step == 0` / `keep`), `hakios.thyme_slpi.c` now defaults `fastrpc_grow_kb=4096`, `fastrpc_grow_after=1`, `fastrpc_early=1` (initialized in `hakios_slpi_init()` so `step_link()` waits for FastRPC before QRTR), `fastrpc_smmu=2`, `fastrpc_call=9`, `fastrpc_phone=2`, `fastrpc_pd=2`, `servreg=1`, `sns=1`, and `sns_start_delay=8` unless overridden by explicit boot arguments.
  - Added `slpi=1` directly to `devices/thyme/ram.args`, while `SYSTEM=1 SCENE=wirelessfull devices/thyme/build-root.sh` staged `fw/slpi.*` into `system.apfs` so `root-wirelessfull.apfs` (`48,992,256 B`) + `hakios-kernel` (`16,964,312 B`) stay within the `66,060,288 B` small-image budget (`103,720 B` margin).
- **Live daily-driver verification (`logs/212544-run.log`, exact `boot-hakios-ram.sh` flow, zero panics, returned cleanly to Android)**:
  - Loaded `slpi.mdt` / `slpi.b00..b20` from `system.apfs` using only `slpi=1` in `ram.args`; registered FastRPC at `+146.574 ms`, received `55/55` SNS `result 0` responses (`accel` 1,905 samples, `gyro` 1,905 samples, `mag` 1,412 samples, `ambient_light` 70 samples `224 -> 59 lux`, `barometer` 74 samples `961.85 hPa`), and dispatched live HID events (`ok=1`) to `backboardd`.
  - Coexisted cleanly with `pcie2_mhi=1` (`SDX55M` Sahara/QMI PDC active), `adsp=1` (stereo speakers/mics/AW8697 haptics), `wifi=1` (`en0` keepalive), `bt=1` (3 Mbaud H4 ready), hardware 89.08 Hz TE-synced OLED display, and `hakios-gl`.

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,964,312 B, `slpi=1` daily production defaults) | `6b4158bbfa828fd07d13ef7f3b8033b72515bbf10f09c2663d145317349a6659` |
| `out/thyme-boot/root-wirelessfull.apfs` / `userland/root.apfs` (48,992,256 B; total `65,956,568 B < 66,060,288 B`) | `0143c5f3eb8d844aa1784954bbb0776b3b5ceaeaa4fdd8531e3289887c59183d` |
| `userland/system.apfs` (with `fw/slpi.*` staged for daily `slpi=1`) | `83e304d77605b489e66042fb0378c2b72e059d2527cfd02ced4cbeafe5f198ef` |
| `logs/212544-run.log` (Unified `boot-hakios-ram.sh` 120 s live run: SLPI 5 sensors + SDX55M + Wi-Fi + BT + 90 Hz) | `b2ab3ff6ef26204366eb22013688d812aec311b7671b614cca34b4e5cc2b9620` |

### Batch 1 Quick-Win Silicon Bring-Up: QRNG, PM8150 RTC, SLPI Sensors & Hall Probe (2026-11-26)

- **SM8250 Hardware TRNG (`qrng@793000` -> `mc_prng`) (`PASS`)** (`xnu/files/osfmk/minicrypto/mc_provider.c`, `devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Exported `hakios_prng_mix()` forwarding to `mc_prng_reseed()`. On boot, `thyme_qrng_init()` votes `GCC_PRNG_AHB_CLK` (`0x00100000 + 0x52004` bit 13), collects 16 distinct 32-bit random words (64 bytes) from `0x00793000`, mixes them into XNU `mc_prng`, restores the GCC clock vote, and sets `kern.hakios.qrng_ready=1`.
- **PM8150 SPMI Hardware RTC (`PPID 0x60`) (`PASS hardware counter`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Discovers `PPID 0x60` (`type=0x7`, `subtype=0x1`, `ctrl=0x80`) via SPMI observer APID `2` (avoiding collision with `hakios.pmic.c`'s `kern.hakios.rtc_seconds`), verifies consistent double-read across byte carry, exposes `kern.hakios.umi_rtc_seconds`, and confirms a live 1 Hz hardware tick (`t0=28394751 -> t1=28394753`, `delta=2 s`). Added `OBS_DROPPED` SPMI read retry in `keys_read()`.
- **SLPI Sensor Expansion & Early FastRPC Timing Fix (`devices/thyme/drivers/sensor/hakios.thyme_slpi.c`, `userland/src/inject.c`)**:
  - Exiting `step_start()` immediately on `fastrpc_early && ready_at != 0 && handed_over` (skipping SMEM debug dumps on the critical path), polling `CHANNEL_QRTR`/`CHANNEL_FASTRPC` at `500 us`, and tightening `fastrpc_phone_chain()` retry intervals (`1-2 ms`) restored FastRPC registration to `+141.383 ms` (`3,481 apps_std calls`, `63/63` SNS `result 0`, zero refusals).
  - Upgraded `kern.hakios.sensor_state` to `version=3` (`flags=0x13b`): added `device_orient` (`PASS`, `suid=1`, `seq=1`, live `hakios: sensor: device_orient #1 state=0` in `backboardd`), `pedometer` (`PARTIAL`, `suid=1`, `seq=0` while stationary), and confirmed `"tsl2540_rear"` returned `suid=0` (needs multi-SUID `"ambient_light"` / `"tsl2540"` lookup).
- **Hall Sensor (`GPIO14` @ `0x0f50e000`) (`TODO` / safely disabled)**:
  - Verified DTB binds `gpio_keys/hall_key` to `TLMM SOUTH GPIO14` active-low, while `0x0f50e000` reads `cfg=0 io=0` (unconfigured/RAZ) and writing bias-pull-up to `0x0f50e000` caused an immediate bus/XPU hang in an earlier test build. Kept strictly read-only and disabled polling when `cfg=0 && io=0`.

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,982,072 B, Batch 1: QRNG + PM8150 RTC + SLPI `version=3` + early FastRPC fix) | `84b2e74d79bca0a6eed3a4822b1a98fe0e5cd37a65e64222f48d5c767ee640aa` |
| `userland/root.apfs` / `out/thyme-boot/root-wirelessfull.apfs` (48,992,256 B; total `65,974,328 B < 66,060,288 B`) | `5aaf9e9466e299b94f93df12897f9742690d3f56042678bc206d0ed940e2a75f` |
| `logs/222034-run.log` (Batch 1 live 100 s verification: QRNG + RTC 2 s tick + SLPI `flags=0x13b` + `inject`) | `50550dc4b85e0eb3e3d0541ce0d91f5fc5ee86fdeb132b53606250c08ea22079` |

### Batch 1B Quick-Win Silicon Probes: Rear ALS (`ambient_light_back`), Watchdog & Charger I2C Telemetry (2026-11-26)

- **SLPI Rear Ambient Light (`TSL2540`) (`PASS`)** (`devices/thyme/drivers/sensor/hakios.thyme_slpi.c`):
  - Changed the SLPI SUID query from `"tsl2540_rear"` to the actual `slpi.b15` `data_type` `"ambient_light_back"`. Live `logs/223423-run.log` registered FastRPC at `+142.939 ms`, received `64/64` SNS `result 0` responses (`flags=0x17b`), streamed `rear_als(suid=1 seq=1 mlux=37099 raw=40)`, and logged `hakios: sensor: rear_als #1 lux=37 (mlux=37099 raw=40)` in `backboardd`.
- **SM8250 Watchdog Read-Only Probe (`qcom,wdt@17c10000`) (`PARTIAL`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Mapped `0x17c10000` read-only and exported `kern.hakios.wdt_ctl` / `kern.hakios.wdt_sts`; observed `ctl=0 sts=0 bark=524287 bite=1048575`.
- **Wired 33W Charger Pump (`bq25970@0x66`), USB-C Audio Switch (`fsa4480@0x42`) & QUP16 Wireless Bus (`PARTIAL`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`):
  - Added guarded SOUTH-tile TLMM open-drain I2C read-only probing (`gpio44/45` QUP15 & `gpio48/49` QUP16) checking non-RAZ and idle-high before toggling lines and restoring `GPIO_CFG`/`GPIO_IO` afterward:
    - QUP15 (`gpio44/45 cfg=0x1/0x1 io=0x1/0x1`): `bq25970@0x66` verified `part=0x16 vbus=5104 mV vac=4960 mV vbat=4406 mV ibus=0 mA` (exported via `kern.hakios.fastchg_*`), and `fsa4480@0x42` ACKed `id=0x9`.
    - QUP16 (`gpio48/49 cfg=0x1/0x1 io=0x1/0x1`): `idtp9415@0x3b` and `ln8282@0x55` NACKed while off wireless pad without driving `GPIO155/156` enable/power (`TODO`).

| File | SHA-256 |
|---|---|
| `hakios-kernel` (16,983,832 B, Batch 1B: `ambient_light_back` + WDT & charger I2C telemetry) | `045bfac1b62fcfa5fbf651dd2d4f159ff781651a6c4fa62fbe52928acb0057c5` |
| `userland/root.apfs` (48,992,256 B; total `65,976,088 B < 66,060,288 B`) | `5aaf9e9466e299b94f93df12897f9742690d3f56042678bc206d0ed940e2a75f` |
| `logs/223423-run.log` (Batch 1B live 100 s verification: SLPI `flags=0x17b` + `rear_als` + `bq25970` + `fsa4480` + `wdt`) | `c19c688089f2a56c523a548e7bdd9851f88fd023ad9f083668f19da55ed618d1` |

### Batch 2A: Adreno 650 GPU Dynamic Frequency Scaling (`PASS`), CPU `WFI` Audit & NFC QUP1 Probe (2026-11-26)

- **Adreno 650 GPU Dynamic Frequency Scaling (DVFS) (`PASS`)** (`devices/thyme/drivers/gpu/hakios.thyme_adreno.c`):
  - Added mutex-protected GMU HFI `gpu_dvfs_thread()` (`250 ms` cadence with hysteresis over stock Mi 10S OPPs `305 / 400 / 441.6 / 525 / 587 MHz`) driven by live `hakios_gpu_submit()` activity and exported `kern.hakios.gpu_freq_mhz`, `kern.hakios.gpu_perf_level`, and `kern.hakios.gpu_dvfs_switches`.
  - Drains `gpu_dvfs_thread` inside `hakios_adreno_stop()` prior to locking `gGPULock` and restores nominal `587 MHz` (`level 5`) before `gpu_block_stop()` `HFI_PREPARE_SLUMBER`.
  - Verified live in `logs/231618-run.log`: `adreno: dvfs: active at 587 MHz`, scaled `587 -> 441 -> 305 MHz` when idle and `441 -> 587 MHz` on burst (`submits_delta=58`), completing `31` switches and clean shutdown (`adreno: dvfs: final freq=587 MHz (level 5), switches=31`) with zero GPU/SMMU faults, alongside `SLPI` `flags=0x17b` (`rear_als seq=42`).
- **NFC (`qcom,nq-nci@0x28`) QUP1 Read-Only Probe & CPU `WFI` Audit (`PARTIAL` / `TODO`)** (`devices/thyme/drivers/input/hakios.thyme_keys.c`, `xnu-src/xnu-8796.121.2/osfmk/arm64/machine_routines.c`):
  - Generalized TLMM open-drain I2C helpers across SOUTH and NORTH (`0x0f900000`) tiles; verified QUP1 `gpio4/gpio5` idle-high (`cfg=0x1/0x1 io=0x1/0x1`), while `nq-nci@0x28` NACKed without VEN/enable GPIO (`kern.hakios.nfc_present=0`).
  - Confirmed `machine_idle()` already executes ARM64 `WFE`/`WFI` (`Idle_context()`) across idle cores (`PARTIAL`); deep SoC power collapse (`PSCI CPU_SUSPEND`) remains `TODO`.

| File | SHA-256 |
|---|---|
| `hakios-kernel` (17,001,720 B, Batch 2A: Adreno 650 GPU DVFS + NFC QUP1 probe) | `d517c7160f15270ffb15e39747699919ef3baca7b334ee9ff01e7b21f8cdd939` |
| `userland/root.apfs` (48,992,256 B; total `65,993,976 B < 66,060,288 B`) | `5aaf9e9466e299b94f93df12897f9742690d3f56042678bc206d0ed940e2a75f` |
| `logs/231618-run.log` (Batch 2A live 100 s verification: 31 GPU DVFS switches + SLPI `flags=0x17b` + NFC bus1) | `0c945b5753693fcab36e330eea0c9a7661a47d7d773f82a288874c4219ba4c87` |
