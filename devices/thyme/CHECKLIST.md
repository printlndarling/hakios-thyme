# THYME daily-driver checklist

Status meanings: `PASS` is observed on the physical Mi 10S in the named build;
`PARTIAL` is implemented but incomplete; `FAIL` is a reproduced failure;
`TODO` has not been tested.

## The battery guard, and the charging bit that defeated it (2026-11-11)

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
`0874fe424c86e20e0ddfd18991a910b6ae4f347aeef9052453399e30eb387abd`).

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
| Speaker playback | PASS for acoustic output: MI2S, both CS35L41 amps and protection DSP run, and a 1 kHz tone raises the microphone's 1 kHz component by 25.78 dB at the matching time | confirm independent left/right output and stereo balance |
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
| SpringBoard home screen | PASS in historical RAM runs; current fixed build awaits retest | SpringBoard reaches the home screen and renders through the Adreno OpenGL ES path |
| SpringBoard settings and status bar | PARTIAL | Reduce Transparency, Reduce Motion and battery/status-bar hooks exist; battery charging state still needs the current-build RAM retest |
| Settings and application launch | PARTIAL | URL openings, Settings panes and application launch navigation work in scripted runs; broad application compatibility is unverified |
| Wi-Fi Settings UI | PASS for radio, join recovery and discovery | The final regression shows active Wi-Fi, Safari traffic and two complete associations. Entering credentials through the Settings keyboard remains unvalidated because the test uses the local Git-ignored join file |
| Bluetooth Settings/pairing UI | PASS for discovery, pairing, connection and audible A2DP | The footer renders `HakiOS Mi 10S`, devices are discovered, a headset connects and 3 Mbaud playback is clean. HFP, AVRCP controls and deliberate reconnect remain open |
| Chinese UI and fonts | PASS | The automated Settings gate switched the primary language to 简体中文; the entire Language & Region page rendered in Chinese without tofu glyphs (`logs/thyme-zh-hans-final.png`) |
| Rootless package manager (Sileo) | PARTIAL; offline and online repository installs PASS | The Procursus prefix grafted, dpkg installed/queried/removed tree 2.0.1 offline, and Sileo rendered. On 2026-11-08 apt fetched the Procursus index (`InRelease` 8,310 B, `Packages` 334 kB) and installed tree 2.0.1 from https://apt.procurs.us over the phone's own Wi-Fi, queried, listed and removed it, with Sileo launching (`logs/012326-run.log`). A longer interactive Sileo session and dependency repair remain | A re-run on 2026-11-11 (kernel `35d4e53f`, scene `root-sileo` `44281532`) stopped after the scene's first command, `sound_test springboard`: no `prep.sh`, `apt-get`, Sileo or `dpkg-query` line followed (`logs/111316-run.log`, SHA-256 `9fb720bf6d4e41dbd71e0ac5f28a595c39fcc13069453a7ee079689b9092d040`), so the scene image/script needs its first step fixed before the online path can be re-verified on the current kernel; the 2026-11-08 apt evidence above is unchanged. On 2026-11-11 the scene was re-run correctly on kernel `35d4e53f` with the JB system image `b7e22d95` (`system-jb-meta.apfs`) and the rebuilt scene `3d3b5b11` (`logs/115231-run.log`, SHA-256 `43f9bd40c2270a0b217ae98bff829213babac83d0b7149a7f40d9c29b7fff00b`): init grafted `/var/jb from /jb` (439 directories, 6432 files and links), `apt-get update` fetched `InRelease [8,310 B]` and `Packages [334 kB]` from https://apt.procurs.us, and Sileo launched (`org.coolstar.SileoStore`); the UI taps still installed nothing (`dpkg-query: no packages found matching tree`), the known queue-sheet problem. The scene needs the JB system image: with the daily `system.apfs` (`4f0f3f8b`) `/var/jb` is absent and `prep.sh` exits 127 (`logs/113529-run.log`).
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

Evidence:

| File | SHA-256 |
|---|---|
| `logs/060438-run.log` | `bdca7a132549bec6dbd899adb081c4701a8d300296bba0d95503be4e6184619c` |
| `logs/jbmeta-sileo.png` | `d9cbdd35279824497bb778fd4c885b4429fba7ffdc38e14207d5943eb0bf76f9` |

## Display, storage, and installation

| Check | Status | Evidence / next action |
|---|---|---|
| Native 1080 x 2340 display | PASS in historical runs | loader validates THYME GOP size and SpringBoard renders at the panel resolution. The phone's dtbo (`dtbo_a`, the `xiaomi 42 02 0a cmd mode dsc dsi panel` nodes) has exactly two timings, **60 and 120 Hz** (no 90), and its timing-switch commands (F0 page 0, B4, C1 `00 50`/`00 4b`, F0 page 3, BB, 2F, 26, marked `dsi_lp_mode`) are now what the driver sends; the panel still counts 119 DSI command-DMA transitions a second with them (`logs/174108-run.log`, `logs/173158-run.log`), so the low-power send those commands are marked for is the next thing to try. The sequence was then tried on the **60 Hz path at boot** (the regression's `fb_hz=60`): the phone vanished from USB during that boot and needed a physical reset, so the change is reverted (`2538eff`) until the 60 Hz switch can be watched on a console — the 90 Hz path of the same code had booted and run the jiggle scene (`logs/174108-run.log`) |
| Brightness control | PARTIAL | iOS hook and DSI DCS `0x51` completion are verified; human comparison and automatic brightness remain missing |
| Display sleep and wake | PASS (re-verified on the current baseline) | the `rc.jobs.buttons` session's side-button presses: `08:53:53 Set Blanked: YES` (backlight factor 0, fade 0.185 s), `08:54:01 Set Blanked: NO` (`NotificationDeviceWake`, `changeSource: LockButton`, backlight factor 1), `08:54:12 YES` again and `08:54:32 NO`. Screenshots: `logs/buttons-085056/03-script-lock.png` (7 KB, black), `04-script-lock.png` (3.6 MB, the lock screen at 8:54), `05-script-lock.png` (7 KB, black). Logs: `logs/085255-run.log`, `logs/buttons-085056.watch.log`. **The lock screen's cover sheet**: after the one-shot 10 s auto-unlock a re-locked sheet could not be dismissed by touch, and `com.hakios.unlock` answered "not locked" while it still drew (`logs/batui6-435.png`); `inject_sb.c`'s `unlock()` now dismisses it directly through `SBCoverSheetSlidingViewController`, the minimal unlock scene logs "the cover sheet was dismissed directly" (`logs/unlock-live.log`), and the wake-and-open scene reached Settings > Battery (`logs/batteryui-200.png`; see the battery section) |
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
| Camera exposure and switching | PASS | The frame header used to name the exposure just asked for while the bank held the frame before it: the capture stack's AE waited for the header, measured the wrong light, and rang between dark and bright (gains `0x2a`–`0x200`). The header now names the exposure of the pixels the read copies, and the AE's step is 0.4 in log space (0.7 against the one-frame feedback delay rang for fifteen frames); a session settles from 8x to its room's gain in five steps (`logs/161455-run.log`: `ae: frame 2 ... 8.00x -> 0xc0` … `frame 10 ... -> 0x81`). The reads follow the sensor's frame time now: mode 3's HTS x VTS / PCLK is 92 ms at the mode's 2373-line frame (its longest exposure, 2333 lines, nearly fills it), and a 40 ms wait copied the DMA bank while the WM was still writing it — the preview tore across, a line of the next frame's picture, whenever the phone moved, while reads faster than the sensor duplicated frames (the earlier fifteen and twenty-five counts were reads, not frames). The capture stack's AE now also asks for the frame length: the sensor takes a shorter frame (0x0340, 1000 lines = 39 ms at the shortest) when the light fits in it, and the grab waits whatever the sensor is running (10.0 frames a second at the mode's frame, `logs/172555-run.log`; 17.4 in the room, 1548 frames in 88.9 s, `logs/175138-run.log`; 25.6 at the shortest frame). The line time is the sensor's HTS / PCLK, so the header's exposure and lux are right too (the ctab now says 10.85 frames a second, not 30; the front 11.06). The AE's measured mean is smoothed (0.12 a frame), a change beyond 15 % between asks holds the exposure until the scene settles (a violent shake is not a light to follow), asks are at least 10 frames apart (12 on the front, whose exposure now applies mid-stream through SMIA's hold, so an ask keeps the frames it used to lose), and one step is capped at a fifth either way; a 5.8-minute front preview in `logs/202824-run.log` ran 6939 frames at 20.0 a second with no panic. A 30-minute daily session with that build stayed up (three Wi-Fi joins, no mid-run fault; `backboardd`'s SIGSEGV is the known teardown artefact, at the return's second — `logs/205333-run.log`). A vibrator-driven shake scene ran the rear and front previews at 17.5 and 12.9 frames a second; neither CoreHaptics nor the ringer's system sound reached the kernel's vibration counter (0 vibrations played — the system sound went through the audio path as a tap), so whether the gate holds under a hand's shake is still the phone owner's test. What the front's log does show is the spacing: five asks over its 62-second session, 41 frames (about 2.4 s) apart, converging 8.5x → 5.6x instead of asking every frame (`logs/213228-run.log`, `logs/213857-run.log`). A switch asks for the stream's sensor before its first frame, so the kernel starts no session on the sensor it was last on; the timing is in the front camera's row. `cam_frame_min` and `cam_front_frame_min` set the shortest frame's lines (defaults 1000 and 1600): a higher floor is more light a frame — less gain's noise — and fewer frames a second, a lower one the reverse; either is clamped to the mode's own frame. The grab's wait now takes the longer of the frame length the read was asked under and the one the previous read ran at: a bank whose frame was exposed at the old, longer length is not copied on the new, shorter one's clock. When the AE shortened the frame while the phone was moved through changing light, that wait copied the bank mid-write — the tear the phone showed as a flicker (`logs/232943-run.log` is the run that found it; the fix is in `umi_camera_stream_grab()`). A 30-minute preview soak on the camera-focus build (`logs/003837-run.log`, kernel `9488e228`, root `df8e41f6`) ran the rear session for **20,927 frames** (11.6 a second on the dark room's 1960-line frames), every frame given to the capture stack, with the UI answering the screenshots at 5, 13, 22 and 29 minutes (`logs/camsoak30-300.png`, `-800`, `-1340`, `-1740`); no camera error, no memory pressure or jetsam, thermal nominal/moderate (59–71 °C) through the run and one heavy reading (76.3 °C) only in the return's teardown, the battery on its charger at 91–92 % and 35–36 °C. The only fault is the known `backboardd` SIGSEGV at the return. The UI path works too: with the scene waiting 300 s for SpringBoard (the demo job starts at launchd, minutes before its window server), the Camera icon tap opened Camera.app, which ran the rear S5KHMX session (481 done / 479 given, mode 3 3008x2256) and the flip tap switched to the front S5K3T2 (1,331 done / 1,328 given, mode 1 2592x1940) -- `logs/202610-run.log`, SHA-256 `c2973f8b71fb784873a1ecf929feb207b6d728ff93fc84620b6dbae48493a01a`, scene `864738bc`. The timed screenshots missed the preview window (the app opened at kernel t≈435, the captures ended at ≈400); the kernel's session lines above are the evidence. |
| Camera in the daily boot | PASS | `devices/thyme/ram.args` now carries `cam=5 cam_real=1 cam_keep_mb=48 torch=3`, so every daily boot creates the loader's `/arm-io/isp` node, MobileGestalt reports the cameras, the Camera icon is on the first page (diagnostic boots with cam=2/3/6/7 have no icon — that is why it was missing), and the torch device `/dev/hakios-torch` exists for the flash. Opening it starts the real session and shows the sensor's picture (`logs/201806-run.log`, `logs/cam5-icon.png`). The camera diagnostic scripts strip those arguments before appending their own `cam=` |
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
| Camera still capture | PASS | Camera.app's shutter on the real stream: the photograph is made of the wide's 3008×2256 mode whole (the session hands up the whole sensor frame, not a centre crop), the mean of eight raw frames summed in the sensor's 10-bit values (eight since the preview's frame is shorter and carries less light; ~0.3–0.5 s of light in the room), rendered to the graph's 4032×3024 StillImage buffer. The four frames are exposed alike now that each frame's header names the exposure its pixels were made with, and the lens is at the scene's focus (the fridge at ~2 m settles near 1120). The same buffer goes whole to the host through `HAKIOS_CAMERA_KEEP` (`camera: a picture from iOS for the host: 4032x3024, format 2`) and `xl camera` writes it out (`logs/1655iosrear.png`). The label crop beside the Android reference is legible — the QR code, the 中国能效标识 title and the print below it (`logs/162831-run.log`, `logs/label-compare-af3.jpg`); the same crop was a noisy blur before the autofocus and AE fixes (`logs/cmp-ios-label-4f.png`, `logs/cmp-android-label.png`). The frame sums behind a photograph were **not cleared between shots**: the second shot added onto the first one's total and went out at twice the light, its whites clipping channel by channel into pink — the preview, which never averages, measured R/G 0.934 and a still rendered from a live frame 0.935, while the averaged second shot measured 1.136 (`logs/preview-lit2.png`, `logs/live-rear.png`, `logs/pv-rear.png`). With the clear the second shot measures R/G 0.938, B/G 0.955, matching the preview and Android's own 0.939/0.966 (`logs/fix-rear.png`) |
| Front camera (S5K3T2) | PASS (hardware stream and Camera.app) | The stream's silence was the CSID's PHY select: `RX_CFG0`'s phy number is a 3-bit field at bits 22:20, the front's CSIPHY is 4, and the code masked it to two bits while sweeping only 0–3. With `phy & 7` and phy 4, `cam=7 cam_sensor=front` receives **734 packets, the whole 1,152,000-byte 1280×720 mode-2 frame**, rx IRQ `0x10000ff`, no ECC/CRC, IFE done (`logs/205858-run.log`). The iOS path (`cam=5 cam_real=1 cam_sensor=front`) runs the module's mode 1 (2592×1940, stride 3248, 6.3 MB) through CSIPHY 4 (D-PHY, lanes `0xd5`, settle 19), CSID-lite 1 and IFE-lite 1, and Camera.app displays it (`logs/213334-run.log`, `logs/preview-165.png`); `HAKIOS_CAMERA_SENSOR` now selects the front descriptor and switches the UMI sensor between sessions, and the app's rear↔front switch was exercised both ways in that run. Power/ID/tables: `cam=2`/`cam=3` power the module (PM8009 LDOF7/LDOF4, the 2.85 V rail, the PM8150L gates) on the CCI controller at `0xac50000`, master 0 = **GPIO105/106**, read **ID `0x3142` at slave `0x20`** and apply the init (281) and res2 (66) tables (`logs/153959-run.log`, `logs/160909-run.log`). The front power-up took all three PM8150L gates but the release skipped PPID `0x4c2`, leaving the 2.85 V rail on for the next session (EBUSY 16); the release restores it now, and an already-on gate is taken over. Tables come from `umi_sunny_s5k3t2.bin` via the generalized `umi-camsensor-v2.py`. The front's lit-session check: a 150 s run with the app forced to the front applied 2024 lines at gain `0x200`, the AE ramped `0x200 -> 0x372 -> 0x800`, and the screenshots at 190–250 s show a real scene — the ceiling/wall corner and a wall panel — heavy chroma noise at the 64x gain ceiling but no black frame (`logs/083341-run.log`, `logs/front-lit-210.png`). The app's rear→front switch failed after the first session: `uprobe()` re-read `cam_sensor` from the boot argument and overwrote the switch (EINVAL before the front was powered), and the first session shrank the boot-time 2592×1940 DMA reserve to its own frame (ENOMEM for the front). Both are fixed: after the switch the front arms (`armed 2592x1940 stride 3248, 4 lane(s) ... phy 4`), reads `S5K3T2 ID 0x3142`, and every session start answers 0 (`logs/105556-run.log`, `logs/camcmp-220.png`). The handover itself: a stream's worker now asks the kernel for its own sensor before its first frame, so the kernel no longer starts a session on the sensor it was last on (the other camera's power-up and two frames) before the switch; the handover is one kernel power-up. Measured from the old session's stop to the new camera's first frame: 1.7–1.9 s to the front and 2.4 s to the rear, of which about 1.3 s is the capture graph's own reconfiguration before the kernel is asked and about 0.6 s the kernel's power-up (`logs/161455-run.log`). The session follows the mode's frame time now (90 ms for the front's mode 1): ten frames a second, one distinct frame per read — the fifteen before were duplicate reads of a slower frame (see the exposure row). The front's white point against the Android front camera's on the same walls measured 1.5 % short in red (R/G 0.987 in the brightest tenth against 1.002, `logs/1840iosfront.png`); the stream's white balance now carries a 1.5 % red calibration after the scene's estimate (`isp.wb_bias`, the camera table), and the same measurement reads 0.995 with the neutral mean at 1.002 (`logs/1900iosfront.png`). The module's colour matrix is the identity: both of its tuning file's colour-correction modules (`mod_cc12`, `mod_cc13`) carry the 3x3 identity in its symbol table (dumped entry by entry, `tools/camtuning.py`), so there is no real matrix to extract — the front's colours are the white balance's and the gamma's alone, less saturated than Android's; a chart-based calibration would be the only way to get a real one, so the stream applies a 1.18 saturation after the white balance (its pictures looked grey beside the Android camera's). Its frame floor is 1100 lines (48 ms, twenty frames a second) against the wide's 1000; the earlier 1600 (70 ms, fourteen) kept its high-gain noise down, but a preview that follows a moving hand read as a stream that had stopped, so the floor gives up about a stop of light a frame for the motion. The module has no lens motor and the frame header now says so (`HAKIOS_CAMERA_NO_FOCUS` from `umi_camera_ios_focus()`): before, the front's frames carried the rear motor's last position and the capture stack ran its contrast search on every front frame, sending the rear's AK7377 positions while the rear's supply was down — `lens position … not taken`, one per step (`logs/233807-run.log`) — and the search cost a pass over each frame. In a lit room on 2026-11-08 the floor's 1060-line exposure ran the front session's 390 frames (about fourteen a second, the hold reading back `0x424`) and its still measured R/G 1.020, B/G 0.990 in the centre against the Android front camera's 1.025/1.004 on the same room — there is no cool cast left to correct (`logs/110219-run.log`, `logs/camreturn-front.png`). |
| Camera autofocus | PASS | The lens motor is an **AKM AK7377**, not the DW9800 the earlier notes assumed: the module file names it `umi_ak7377_s5khmx`, and the AK73xx family takes the position in register `0x00` as a 16-bit value with the position shifted left four bits (`0x02` is control). The chip answers at CCI address `0x18` (the 8-bit form of the module file's 7-bit `0x0c`; `0x1c` is the OIS), and the DW9800's `0x03/0x04` writes never touched it. With the AK7377 write the lens moves: `cam_focus=0` leaves the fridge label a blur, `cam_focus=1023` renders its QR code and print (`logs/141344-run.log`, `logs/focus0-0.png`, `logs/focus1023-0.png`, `logs/cmp-ak1023.png`). The capture stack's requests are no longer masked to ten bits, and the app's search walks AF_NEAR 16 to AF_FAR 3072 in steps of 96 (fine 24). The search runs end to end: the frame header carried NO_FOCUS in the UMI path, so the app's search returned at every frame until `umi_camera_ios_focus()` reported the motor's position. It sweeps and settles at 1168 for the fridge scene, and the photograph's label crop rises from a 48 to a 63 gradient score with its QR code and print legible (`logs/144509-run.log`, `logs/ios-rearpic.png`, `logs/cmp-ios-label-af2.png`). The metric was not the problem: over the fridge scene the block-gradient measure runs 0.0043 out of focus to 0.0355 at the peak, and the `0.0` in the earlier log was its one-decimal format (now `%.4f`). At stream start the lens climbs about the module's position (343) in steps of 96; when neither way is sharper — the fridge's focus is near 1120 — it falls back to the whole travel and then the fine search, so the photograph is not left at the module's position (`logs/162831-run.log`, `logs/1655iosrear.png`, `logs/label-compare-af3.jpg`). The front camera's lens is fixed. A travel with no peak in it is no focus and no longer ends at a noise-picked position: the second session after a switch (rear→front→rear) swept all 32 steps with every value between 0.0068 and 0.0070, took 'the best' at 880 and left the rear soft until the next search (`logs/233807-run.log`); the sweep now tracks its least value and, when the best is within a fifth of it, goes back to the position the last search settled on (`af_home`, kept across the streams). A scene that turns much sharper than the one the lens settled on — the phone pointed at something with detail after a blank wall — now asks for the search again after three frames; the watch only reacted to a drop before, so a search parked on a blank surface never came back. Near-black frames are not measured at all: the measure divides the centre's gradient by its mean's square, and on an all-black frame (the phone face down, `logs/235943-run.log`) the noise read 0.36 and the search 'settled' on it. `camreturn` (`userland/rc.jobs.camreturn`) is the scene that exercises this rear→front→rear path; its lit verification ran on 2026-11-08 in a lit room (`logs/110219-run.log`): the first rear session settled at **1399 (sharpness 0.018)**, the front session ran 390 frames at the 1100-line floor, and the second rear session settled at **1471 (0.017)** — a real peak eleven moves away, where the old code swept all 32 steps on noise and parked on it. The dark half is verified on the phone: an all-black rear→front→rear run (`logs/002926-run.log`, the phone face down, every frame green 0) made no focus measurement at all — zero `focus: at` lines and no lens moves, where the run before the guard swept 66 positions and settled on 0.36 of pure noise — and the front's session logged no rear-motor writes (its `NO_FOCUS` frames keep the capture stack from searching a motor that is not there). The three streams (289 rear, 278 front, 474 rear frames) ran to their stops without a fault. |
| Torch/flash | PASS | Hardware pulse: PM8150L channel 97 ramped up and cleaned up (`logs/081934-run.log`). End-to-end userspace and stock Control Center: AVFlashlight/mediaserverd requested 1000, 250 and 850 through `/dev/hakios-torch`; every request reached the kernel and the safety timer returned it to dark (`logs/090116-run.log`, `logs/091200-run.log`). The daily args now carry `torch=3`, so the device is there on every boot |
| Cellular modem / SIM | FAIL | the reference phone's SMEM/GLINK stack cannot start here: thyme's SDX55M is a separate die on PCIe2 over MHI (`docs/cellular.md`). The PCIe2 link trains from XNU, the endpoint answers `17cb:0306` with MHI BAR0 `0x64300000`, and BHI reports PBL (`EXECENV 0`) waiting for `sdx55m/sbl1.mbn`; the trust registers read (serial `0x9721197d`, OEM PK hash nonzero, a fused part). The doorbell is refused in 0 ms with `0xef120700` for every image address -- byte-perfect, zeroed, physical below 4 GB, the DT pool IOVA with the BDF-to-SID table and a stage-1 context, and with the MHI windows set to the DT pool; the refusal is independent of the image's content and records no SMMU fault. The PON pulse leaves no visible trace (0x862/0x863 all zero after it). Next: the MHI context-base registers and an explicit MHICTRL reset before the doorbell. No SIM registration |
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
- [ ] Rear camera.
- [ ] Cellular modem, SIM, and network registration.
- [ ] NFC and fingerprint policy.
- [ ] Repeatable installer and recovery workflow for this third-party ROM.
- [x] Rootless Procursus prefix, dpkg metadata and offline install/remove verification.
- [ ] Online repository installation and Sileo long-run stability.
- [ ] Performance and thermal soak under daily use: a 20-minute soak on the daily scene with tonight's camera kernel changes stayed live (settings pages at 150-900 s, battery flat, no panics, no memory pressure; `logs/040632-run.log`); only backboardd meets SIGSEGV during the kernel's abrupt return. A longer interactive soak and a thermal log remain. A 20-minute daily-scene soak on the current baseline (`logs/185653-run.log`, kernel `26257831`, root `8f5bc2aa`) ran two Wi-Fi joins with the scene's Safari/Settings traffic and GL frames, reached thermal **heavy** (75–78 °C, cores at their rows) under the load and settled again, and had zero panics, zero memory-pressure or jetsam lines, and a clean audio-DSP shutdown at 1374 s; the display slept once the scene's jobs ended, so its screenshots are black — it is an idle-stability soak, while the interactive evidence remains the 30-minute camera soak (`logs/003837-run.log`). A 30-minute camera-preview soak on the camera-focus build ran 20,927 frames (11.6 a second) with the UI answering at 5, 13, 22 and 29 minutes, no memory pressure or panic, thermal nominal/moderate until the return's teardown (one heavy, 76.3 °C) and the battery charging at 91–92 % (`logs/003837-run.log`, `logs/camsoak30-*.png`). A 20-minute daily soak on the servreg-ack baseline (`logs/230103-run.log`, kernel `bcc297c7`, root `8f5bc2aa`) joined the Wi-Fi network, kept every thermal read nominal (44–59 °C, hottest cpu-1-5), showed no panic or jetsam, and shut the audio DSP down cleanly at 1374 s; the Bluetooth daemon's short exits (117 over the run) are the same count as the earlier soak's and are its usual idle behaviour. A 22-minute daily-scene soak on the current build (`logs/102532-run.log`, kernel `09cb2178`, root `8f5bc2aa`, SHA-256 `e222104aa335a6ced2bc12c5170bd3fdc75e1c864f941cd0cde90da53114c1c9`) held the Wi-Fi join for 763 s (DHCP 192.168.0.103, live TCP at the end), counted 171 LE reports (50 named) with zero H4 transport faults, stayed thermal-nominal at 54-56 C (the usual five slowdowns), kept the battery on its charger at 80-82 % and 38-39 C, and showed no panic and no jetsam -- the only `memorystatus` lines are the teardown's at 1314 s; the UI answered at 6, 14 and 18 minutes (`logs/soak-360.png`, `soak-840.png`, `soak-1080.png`; the 10-minute capture failed to save).
- [ ] Battery-cycle integrity and no unexplained percentage jumps.
