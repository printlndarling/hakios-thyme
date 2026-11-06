# THYME daily-driver checklist

Status meanings: `PASS` is observed on the physical Mi 10S in the named build;
`PARTIAL` is implemented but incomplete; `FAIL` is a reproduced failure;
`TODO` has not been tested.

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

A 32-minute soak on the 60 Hz default (`logs/035725-run.log`, 2026-11-06)
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
| Dual-speaker stereo balance | PARTIAL | Both CS35L41 amplifiers and the protection DSP initialize; independent left/right output and balance are still unverified |
| Vibration | PARTIAL | AW8697 stock RAM playback is clean. The continuous-mode probe repeatedly measured BEMF peaks `+47/-200` and `+48/-200`, matching Android's stock `+45/-202`, so the coil drive/BEMF path is hardware-verified. F0 is advisory because it is unstable on both HakiOS and stock Android. Physical feel and the iOS actuator service remain unverified |
| Bluetooth | PASS for discovery and connection on the canonical image; A2DP verified earlier; HFP/reconnect open | The 2026-11-04 fix restored `umi_bluetool`, `scname` and the H4 transport; the canonical run reports 315 LE reports with zero H4 faults, shows the `HakiOS Mi 10S` footer and reconnects the OPPO Enco Free4 (`logs/214226-run.log`). The QCA responder now starts at boot before `bluetoothd` (`mkjobs.py` RunAtLoad; `logs/104749-run.log`). The earlier 3 Mbaud A2DP result remains valid for the legacy image. HFP/SCO, AVRCP, microphone and automatic reconnect remain unverified |
| Wi-Fi discovery/startup | PASS | The current run opened Wi-Fi and returned real 2.4 GHz and 5 GHz networks; the user also confirmed Wi-Fi remains usable on the current RAM boot (`logs/210532-run.log`) |
| Wi-Fi power off/on recovery | PASS | The current run switched Wi-Fi off (`set 19`, interface removed), then on again (`set 19`, radio restarted), scanned, associated and retained real network traffic (`logs/210532-run.log`) |
| Wi-Fi association/DHCP | PASS | Both current-run associations completed the four-way handshake and installed pairwise/group keys; DHCP assigned `192.168.0.103`, and en0 traffic remained usable after the power cycle (`logs/210532-run.log`) |
| Wi-Fi TCP/TLS and Safari | PASS | Safari opened through en0; DNS, TCP and TLS connections were established after each association (`logs/210532-run.log`) |
| Combined wireless regression | PASS | The current GPU-enabled cold boot stayed at 3 Mbaud, completed two Wi-Fi associations with CCMP and DHCP, carried Safari traffic, submitted hardware GL frames and reached Bluetooth Settings discovery without H4 faults (`logs/024949-run.log`). The 2026-11-05 canonical run repeated this: two joins, DHCP `192.168.0.102`, Safari TLS, 83 LE reports and 733 GL frames with zero H4 faults (`logs/111252-run.log`). The merged kernel repeated it with 85 LE reports and 735 GL frames (`logs/114117-run.log`) |
| CPU | PASS | 7 CPUs start and all three EPSS domains raise to 1804/2419/3187 MHz |
| GPU acceleration | PASS | With `gpu=1` and the `GL=1` image, the THYME Adreno 650/GMU path reports `THE GPU STAYS ON`, Mesa/FD650 initializes and the current run submits more than 760 hardware `hakios-gl` frames with zero `softpipe` fallback (`logs/024949-run.log`). The 2026-11-04 first rebuild omitted `GL=1` and submitted zero `hakios-gl` frames; `devices/thyme/build-root.sh` and `check-root.py` now refuse that image (the GL jiggle run presents 300 frames per 5 s, `logs/224744-run.log`) |
| UI animation frame pacing | PASS at 90 Hz | The re-announced panel rate drives the GPU-backed jiggle at about 90 fps: 452, 451, 451 and 451 frames in four consecutive five-second windows and an 11.1 ms median after the upstream merge (`logs/115200-run.log`); the same animation was 60 fps before the fix (300 frames per 5 s, `logs/224744-run.log`) |
| 90 Hz panel refresh | PASS (user-confirmed) | The full DTB sequence is accepted and promoted (`logs/200350-run.log`: `seq=0 (ok), framebuffer rate 90 Hz`); the user confirmed smoothness with no dropped frames. CoreAnimation now follows the panel after the display driver re-announces the rate (`logs/110745-run.log`). If the sequence is rejected, the framebuffer falls back to 60 Hz |
| Metal backend | PARTIAL; render blocked | `hakios-metal` cross-builds and the probe passes device open, GPU blit/fill, in-flight command buffers, a 16 MB copy and many AIR compute kernels. A minimal no-SpringBoard bisect still hangs the first render pass; every later variant is poisoned because `gpu_recover=1` has no implemented reset. Metal/CoreAnimation stays disabled (`docs/official-metal-probe.md`, `logs/033837-run.log`). Upstream's Metal-on-Vulkan/Turnip path (`87bf485`) is merged but opt-in (`TURNIP=1 M2V=1`, `metal_m2v=1`) and untested on THYME |
| Touch | PASS | User confirmed touchscreen input on the current RAM boot after `/chosen/hakios-board` was restored; FTS521 polling also reports `errors=0` |
| Brightness adjustment | PARTIAL | DSI `0x51` level 2056 was acknowledged after iOS requested 500 permille; human-visible low/mid/high comparison remains |

## Official-main migration gate

- [x] Register THYME in `devices/manifest.conf`.
- [x] `tools/hakios-device.py check thyme` passes.
- [x] `make thyme_defconfig` and `make config` pass.
- [x] Official-main kernel builds.
- [x] Official-main kernel reaches launchd and SpringBoard on the phone.
- [x] Official-main kernel returns to Android safely.
- [x] Official loader and `xnu-usb.img` boot on the phone.

## Boot and interaction

| Check | Official main | Legacy evidence |
|---|---|---|
| XNU reaches userspace | PASS | PASS |
| 7 CPUs and cpufreq | PASS; three EPSS domains at 1804/2419/3187 MHz | PASS |
| 1080 x 2340 display | PARTIAL: generic framebuffer reaches SpringBoard | PASS |
| Brightness adjustment | PARTIAL: iOS hooks and DCS `0x51` command completion verified | PASS in the legacy display probe |
| Adreno 650 GPU rendering | PASS, more than 2,100 `hakios-gl` frames in four minutes | PASS |
| 30-minute UI stability | PASS; continuous SpringBoard rendering, stable memory and orderly return (`docs/official-ui-soak.md`) | PASS |
| FTS521 touch polling | PASS, 415 events in the GPU run / `errors=0` | PASS |
| Tap, drag, gestures | PASS, backboardd touching/none and gesture handling | PASS |
| Power key | PASS; short press blanks the display and the next press wakes it | PASS |
| Volume keys | PASS, pressed/released captured | PASS |
| Display off and wake | PASS; `Set Blanked: YES` and `Set Blanked: NO` with `changeSource: LockButton` (`logs/075530-run.log`) | PASS |

## Power and charging

| Check | Official main | Legacy evidence |
|---|---|---|
| Battery percentage/voltage/temperature | PASS for telemetry; iOS status bar refresh mismatch remains | PASS |
| Signed current | PASS, `-97 mA` during the probe | PASS |
| Positive current in HakiOS | PASS; observed 1239 mA and 106-140 mA with `charging=1` | PASS |
| One-shot `TakeAction` | TODO | PASS, exactly two calls |
| 30-minute charging endurance | PASS; endpoint online throughout the UI run, 0 failed reads | PASS with endpoint online |
| Net charge under USB load | PARTIAL: SOC stayed 97% over the 30-minute UI run at +129..151 mA; longer net-charge test still required | PARTIAL: load can exceed a 500 mA host port |
| `battery_stop` safety guard | TODO | PASS |
| Thermal frequency caps | PASS; big-core domain 17 -> 16 at 75.9 C, then recovered | PASS |
| `thermal_stop` hand-back | PASS with forced `thermal_stop=30`; GPU stopped in order and loader returned without panic | TODO |
| Automatic Android return after `thermal_stop` | TODO: host must issue `fastboot -s xnuloader reboot` | TODO |

## Audio, wireless, and sensors

| Check | Status | Next action |
|---|---|---|
| ADSP firmware | PASS on official main; APR services up | port and validate official ADSP path |
| Speaker playback | PASS for acoustic output: MI2S, both CS35L41 amps and protection DSP run, and a 1 kHz tone raises the microphone's 1 kHz component by 25.78 dB at the matching time | confirm independent left/right output and stereo balance |
| Microphone capture | PASS: DEC1/ADC1 + DEC0/ADC4 captures dual-channel PCM (`rms 4854-7930`); Darwin AVAudioRecorder through `/dev/hakios-mic` ran 4.9 s with `0 frames skipped` and `0` short reads | keep a tone/intelligibility regression check in future soak runs |
| Haptics | PARTIAL: repeated BEMF peaks match stock, proving continuous drive and back-EMF; F0 is advisory and unstable in both HakiOS and Android; BSTERRI is the vendor-ignored boost latch | confirm physical feel, then test iOS taps and long vibrations from the Actuator HAL |
| QCA6390 PCIe/MHI | PASS on official main; firmware reaches mission mode | port and retest |
| QMI / WLFW service | PASS; QCA6390 firmware `0x1016046e` ready | diagnose any long-run reconnect failures |
| Wi-Fi station / join / DHCP | PASS on the current baseline: WPA2 5 GHz association, four-way handshake, DHCP, DNS, TLS and Safari page rendering (`logs/022344-run.log`) | test 2.4/5 GHz switching, reconnect after link loss, and sleep/wake |
| Bluetooth UART firmware | PASS on official main | `htbtfw20.tlv` and `htnv20.bin` accepted |
| Bluetooth HCI reset | PASS on official main | HCI 0x0c03 completed; iOS command flow followed |
| Scan / pair / connect | PASS for scan, pairing and connection; PARTIAL for audio/reconnect | the user confirmed a known Bluetooth headset connects on the current RAM boot; next verify A2DP/HFP routing and reconnect after disabling/enabling Bluetooth |
| ALS / proximity / accelerometer | PARTIAL | the SLPI boots and its SNS client answers QMI (see below); no sensor data or HID service yet |

## User space and daily use

| Check | Status | Evidence / next action |
|---|---|---|
| Darwin launchd and core daemons | PASS in historical RAM runs | launchd starts process 1 and brings up cfprefsd, logd, runningboardd, backboardd, powerd, wifid and other selected system daemons |
| SpringBoard home screen | PASS in historical RAM runs; current fixed build awaits retest | SpringBoard reaches the home screen and renders through the Adreno OpenGL ES path |
| SpringBoard settings and status bar | PARTIAL | Reduce Transparency, Reduce Motion and battery/status-bar hooks exist; battery charging state still needs the current-build RAM retest |
| Settings and application launch | PARTIAL | URL openings, Settings panes and application launch navigation work in scripted runs; broad application compatibility is unverified |
| Wi-Fi Settings UI | PASS for radio, join recovery and discovery | The final regression shows active Wi-Fi, Safari traffic and two complete associations. Entering credentials through the Settings keyboard remains unvalidated because the test uses the local Git-ignored join file |
| Bluetooth Settings/pairing UI | PASS for discovery, pairing, connection and audible A2DP | The footer renders `HakiOS Mi 10S`, devices are discovered, a headset connects and 3 Mbaud playback is clean. HFP, AVRCP controls and deliberate reconnect remain open |
| Chinese UI and fonts | PASS | The automated Settings gate switched the primary language to 简体中文; the entire Language & Region page rendered in Chinese without tofu glyphs (`logs/thyme-zh-hans-final.png`) |
| Rootless package manager (Sileo) | PARTIAL; offline install/remove and Sileo render PASS | The Procursus prefix grafted, dpkg installed/queried/removed tree 2.0.1 without missing file-list warnings, and Sileo rendered. Online repository install remains unverified; see below |
| Notifications, audio routing and background apps | PARTIAL | Safari reaches and renders HTTPS content, Weather/Calendar widgets render, Apple services resolve and establish TLS. Notification delivery and sustained background behavior still need an interactive soak |
| Lock/unlock and passcode | PARTIAL | display sleep/wake through the lock button is verified; passcode entry, secure storage and reboot persistence are not validated |
| User data persistence | TODO | current root and private data are RAM-backed; the persistent-storage policy and guarded write path are not accepted |

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

- online repository installation through Sileo; a later run reached 5 GHz
  association, accepted CCMP keys and advanced Sileo, but the loader console
  did not retain the apt command output, so this is not counted as PASS;
- Sileo on-line repository refresh and a longer interactive session;
- dependency repair after installing a package that needs additional packages.

Evidence:

| File | SHA-256 |
|---|---|
| `logs/060438-run.log` | `bdca7a132549bec6dbd899adb081c4701a8d300296bba0d95503be4e6184619c` |
| `logs/jbmeta-sileo.png` | `d9cbdd35279824497bb778fd4c885b4429fba7ffdc38e14207d5943eb0bf76f9` |

## Display, storage, and installation

| Check | Status | Evidence / next action |
|---|---|---|
| Native 1080 x 2340 display | PASS in historical runs | loader validates THYME GOP size and SpringBoard renders at the panel resolution |
| Brightness control | PARTIAL | iOS hook and DSI DCS `0x51` completion are verified; human comparison and automatic brightness remain missing |
| Display sleep and wake | TODO | exercise the power path under the current board-gating fix |
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
| Rear camera stream path | PASS | stage 7 of `hakios.thyme_umi_camera.c`: camcc PLL0 (Lucid, 1200 MHz) feeds ife_lite 400 / ife_lite_csid 400 / cphy_rx 400 / csi3phytimer 300 MHz; CSIPHY 3 is C-PHY, 3 trios, lane enable `0x2a`, settle 19, rate row 0 (1.707 Gbit/s a trio); CSID receives from PHY 3, vc 0, RAW10 `0x2b`; the SMMU mapper was extended past one 2 MB L2 entry for the 2.47 MB frame. The DT's `cam-sensor@1` names `csiphy-sd-index = 3` — the first attempt listened on CSIPHY 1 and saw 0 packets. `cam_mode=N` selects the module file's `resN` (default 3) |
| Front camera capture | TODO | not exercised |
| Rear camera sensor/power probe | PASS | `cam=2` reached the S5KHMX sensor: ID `0x1ad0`, revision `0xa000`, module selector `0x1301`; all three supplies released with error 0 (`logs/101242-run.log`) |
| Camera sensor mode programming | PASS | `cam=3` applied 259 init and 179 mode writes; dimensions `3008x2256`, line/frame lengths `0x3ea0/0x0945`, stream register `0x0100=0`, then powered down cleanly (`logs/114440-run.log`) |
| Camera ISP clock/reset probe | PASS | `cam=4` read IFE `0x10000001`, bus `0x20000000` and CSID `0x20000001`; completed CSID480 and IFE-lite48x resets with DMA disabled and all supplies released (`logs/122824-run.log`, SHA-256 `f5aa54ee6186f2420cb26fcaa7eea930add008daf994abf2a86de419d1784397`) |
| Camera CSID/IFE test pattern | PASS | `cam=6` wrote a whole 320x240 RAW/Bayer frame through the THYME DMA path; `xl camera` exported `logs/cam-tpg-0.png` (`logs/104221-run.log`) |
| Camera userspace / AVFoundation | PASS with the real sensors | `cam=5 cam_real=1` served the S5KHMX's own frames to `/dev/hakios-camera`: 3174 frames of 1280×720 (a centre crop of mode 5) at 15 a second, 3173 delivered to mediaserverd and Camera.app (`logs/131304-run.log`); an earlier session at 5 a second delivered 1166 of 1167 (`logs/122512-run.log`). The front camera is the device's second stream: the app's switch sends `HAKIOS_CAMERA_SENSOR`, the kernel selects the front descriptor (its `camera-s5k3t2.tab` modes) and the UMI driver's next session, and the front's 2592×1940 frames reach the same capture graph (`logs/213334-run.log`). The program's exposure/gain requests reach the sensor through its group hold. The preview was frozen on a session's first frame: the IFE-lite takes one buffer address per frame, so with a single address it wrote once and every read returned the same frame — often the black first one after stream-on. The DMA buffer now holds two frames, each read hands the WM the other bank and copies the one its frame has finished in (`logs/001158-run.log`, `logs/preview-135.png`). The AE's group hold is bit 0 of 0x0104 on the S5KHMX (the module's 0x0100 registers the values but the sensor keeps the old ones; 60 -> 1000 lines now moves the frame 29 -> 41 in the dark, `logs/000647-run.log`); the S5K3T2's front takes the exposure only before 0x0100: mode 1 with a pre-stream 2000-line/64x exposure is a real picture (`logs/front-mode1-0.png`), while the app's session stayed black at the maximum and the AE's changes are now applied between a stop and a start (30 ms standby, the register read back). `cam=5 cam_fake=1` still provides the synthetic chart for QEMU (`logs/110735-run.log`) |
| Camera in the daily boot | PASS | `devices/thyme/ram.args` now carries `cam=5 cam_real=1 cam_keep_mb=24 torch=3`, so every daily boot creates the loader's `/arm-io/isp` node, MobileGestalt reports the cameras, the Camera icon is on the first page (diagnostic boots with cam=2/3/6/7 have no icon — that is why it was missing), and the torch device `/dev/hakios-torch` exists for the flash. Opening it starts the real session and shows the sensor's picture (`logs/201806-run.log`, `logs/cam5-icon.png`). The camera diagnostic scripts strip those arguments before appending their own `cam=` |
| Camera colour tuning | PASS | `camtuning.py` now reads the phone's Parameter Parser V2.0.0 tuning file (its 0x48-byte symbols, 8-byte fields, cc12 matrices and padded lens-shading grids); the scene carries `/redwood/fw/camera-s5khm2sd.isp` made from `com.qti.tuned.thyme_semco_s5khmx_wide.bin` (25,428 bytes). The media library loads it: 3 colour matrices' lights and 3 lens shadings (`logs/131304-run.log`). That file's AWB reference lights are zeroed, so the auto white balance runs grey world without the reference-light projection (gains around R 1.2 B 1.7 under warm indoor light; `logs/211126-run.log`). The front S5K3T2's own tuning (`/vendor/lib/camera/com.qti.tuned.thyme_sunny_s5k3t2_front.bin`, 5.3 MB, pulled with `su`) is in the image as `/redwood/fw/camera-s5k3t2.isp`: 1 colour matrix's light and 3 lens shadings (corners about 3.3x), loaded for the front stream (`logs/215111-run.log`) |
| Camera still capture | PASS | Camera.app's shutter on the real stream: two photographs, each `StillImage: 4032x3024 made of every pixel in 175/205 ms` and given to the graph (`logs/133226-run.log`). The still is scaled from the 1280×720 preview stream; a full-resolution still path remains |
| Front camera (S5K3T2) | PASS (hardware stream and Camera.app) | The stream's silence was the CSID's PHY select: `RX_CFG0`'s phy number is a 3-bit field at bits 22:20, the front's CSIPHY is 4, and the code masked it to two bits while sweeping only 0–3. With `phy & 7` and phy 4, `cam=7 cam_sensor=front` receives **734 packets, the whole 1,152,000-byte 1280×720 mode-2 frame**, rx IRQ `0x10000ff`, no ECC/CRC, IFE done (`logs/205858-run.log`). The iOS path (`cam=5 cam_real=1 cam_sensor=front`) runs the module's mode 1 (2592×1940, stride 3248, 6.3 MB) through CSIPHY 4 (D-PHY, lanes `0xd5`, settle 19), CSID-lite 1 and IFE-lite 1, and Camera.app displays it (`logs/213334-run.log`, `logs/preview-165.png`); `HAKIOS_CAMERA_SENSOR` now selects the front descriptor and switches the UMI sensor between sessions, and the app's rear↔front switch was exercised both ways in that run. Power/ID/tables: `cam=2`/`cam=3` power the module (PM8009 LDOF7/LDOF4, the 2.85 V rail, the PM8150L gates) on the CCI controller at `0xac50000`, master 0 = **GPIO105/106**, read **ID `0x3142` at slave `0x20`** and apply the init (281) and res2 (66) tables (`logs/153959-run.log`, `logs/160909-run.log`). The front power-up took all three PM8150L gates but the release skipped PPID `0x4c2`, leaving the 2.85 V rail on for the next session (EBUSY 16); the release restores it now, and an already-on gate is taken over. Tables come from `umi_sunny_s5k3t2.bin` via the generalized `umi-camsensor-v2.py` |
| Camera autofocus | IN PROGRESS | The wide camera's lens motor (DW9800 at `0x0c`) is now powered and initialised: the VAF rail `ldoc9` at 2800 mV through the UMI RPMh path (`UMI RPMH: camera ldoc9 2800 mV enable=1 error=0`) and the module's motor sequence (`lens motor initialised (DW9800 at 0x0c)`, `logs/010655-run.log`). The table path's `vaf()` uses a different RPMh client whose RSC is not mapped on this path and hung the kernel on the first rear session. The capture stack's focus requests (`HAKIOS_CAMERA_FOCUS`) now reach `uvcm_position()`; the contrast sweep on a distant scene is being verified. The front camera's lens is fixed |
| Torch/flash | PASS | Hardware pulse: PM8150L channel 97 ramped up and cleaned up (`logs/081934-run.log`). End-to-end userspace and stock Control Center: AVFlashlight/mediaserverd requested 1000, 250 and 850 through `/dev/hakios-torch`; every request reached the kernel and the safety timer returned it to dark (`logs/090116-run.log`, `logs/091200-run.log`). The daily args now carry `torch=3`, so the device is there on every boot |
| Cellular modem / SIM | FAIL | MPSS/telephony stack is not implemented and no SIM registration has been verified |
| Mobile data / calls / SMS | FAIL | depends on the cellular stack |
| ALS / auto-brightness | PARTIAL | the SLPI ALS path is part of the sensor bring-up; brightness remains manual until the HID sensor service works |
| Accelerometer / orientation | PARTIAL | the SLPI boots and the accelerometer SUID lookup is accepted; no samples reach backboardd yet |
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

## Later product work

- [x] Integrate the phone's extracted UFSDxe/DiskIoDxe/PartitionDxe into the
  THYME UEFI, expose BlockIo/PartitionInfo, and pass the read-only survey.
- [ ] After that survey, make a separate accepted design for persistent storage.
- [ ] Rear camera.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [x] Rootless Procursus prefix, dpkg metadata and offline install/remove verification.
- [ ] Online repository installation and Sileo long-run stability.
- [ ] Performance and thermal soak under daily use.
- [ ] Battery-cycle integrity and no unexplained percentage jumps.
