# Xiaomi Mi 10S (`thyme`) — HakiOS Hardware & Daily-Driver Checklist

Status legend:
- `✅` Passed — verified on physical Xiaomi Mi 10S (`SM8250-AC`)
- `🚧` Partial — hardware/driver partially working or sub-features remaining
- `❌` Todo — not implemented or not yet verified
- `⛔` Blocked — external prerequisite or compatibility boundary prevents end-to-end verification

> Historical bring-up logs, SHA-256 checksums, and per-run records are archived in [`devices/thyme/docs/HISTORY-LOG.md`](docs/HISTORY-LOG.md).

---

## 1. Hardware & Peripheral Inventory

| Component | Silicon / Device Tree | Priority | Status | Current Notes |
|---|---|---|---|---|
| CPU Clusters (7/8 cores) + DVFS | `SM8250-AC` Silver / Gold / Prime (EPSS) | P1 | ✅ | 7 cores active (`bootcpu=7 cpus=7`), 200 ms runtime EPSS scaling (up to 1.80 / 2.42 / 3.19 GHz) + `thermal=1` cap |
| CPU0 (8th Core) | Silver core 0 | P1 | ❌ | Reserved by `xnuloader` for UEFI/EL3 under RAM boot |
| GPU (OpenGL ES) + DVFS | Adreno 650 (`0x06050002`) + GMU HFI | P0 | ✅ | Mesa/Freedreno HW rendering + 250 ms GMU DVFS (`305–587 MHz`) |
| GPU (System Metal) | `hakios-metal` + `HakiosMetalCA` | P0 | 🚧 | Compiles & initializes in probes (`metal_apps=1` `backboardd` fix); system `MTLDevice` stability pending |
| Display & 90 Hz Refresh | 6.67" 1080×2340 OLED (`j2s_mp_42_02_0a_dsc_cmd`) | P0 | ✅ | `89.08 Hz` TE-synced (`INTF_1`, 0 missed scans), DCS brightness & lock blanking |
| Touchscreen | FocalTech `FTS521` | P0 | ✅ | Multi-touch, edge/home gestures, coordinate latency logging |
| Physical Buttons | Power, Vol+ / Vol- (`PM8150 PON` / `GPIO6`) | P0 | ✅ | Press/release, screenshot chord, reboot/wake |
| Stereo Speakers & Earpiece | Dual `CS35L41` (`0x40` bottom + `0x42` top RCV) | P0 | ✅ | Stereo L/R, ADSP `0x1004`, VoLTE, 15 s keepalive & 5×500 ms retry recovery |
| Microphones | Dual onboard mics (`mic=4`) | P0 | ✅ | Dual-mic capture, VoLTE uplink, VoiceMemos 0-short-read capture |
| Linear Haptics | `AW8697` LRA | P1 | ✅ | CoreHaptics transient & continuous waveforms |
| Rear Main Camera + AF | 108 MP `S5KHMX` + `AK7377` VCM | P1 | ✅ | `1080p` ~51 fps preview, adaptive AF, instant AWB, 5×5 ISP stills |
| Front Camera | 20 MP `S5K3T2` | P1 | ✅ | `720p @ 42.7 fps` config + lens switching |
| Camera Flash / Torch | `PM8150L` WLED | P1 | ✅ | Torch & shutter flash (`torch=3`) |
| Camera OIS | `qcom,ois@1` (`umi_ois` @ CCI `0x1c`) | P1 | 🚧 | CCI `0x1c` HW readback verified (`ok=1`, `0xf0`); active servo control pending |
| Aux Cameras (Ultra-wide / Macro / Depth) | `OV13B10` / `GC02M1` | P2 | 🚧 | Stock UMI module tables can be parsed. The real UMI capture backend and Camera expose rear main and front only. |
| Cellular Modem (4G / VoLTE) | External `SDX55M` (`PCIe2 / MHI / pcie2_mhi=2`) | P0 | ✅ | `AppleBaseband` + `CommCenter`, 4G LTE dual-stack + VoLTE (`10099`) |
| 5G NR (SA/NSA) & Dual-SIM | `SDX55M` (Android camps `NR_SA` n41) | P1 | ❌ | Userland `Mav17` is 4G-only; needs `Mav20` bridge + 2nd SIM slot |
| GNSS / Location | `SDX55M QMI LOC` + `umi_gnssd` | P0 | 🚧 | Wi-Fi CoreLocation (`BaiduMap` blue dot) & SV stream working; outdoor 3D fix pending |
| Wi-Fi | `QCA6390` (`PCIe0 / ath11k`) | P0 | ✅ | 2.4/5 GHz WPA2, DHCP/TLS, `CN` locale, 65 s lock/wake, auto-rejoin |
| Bluetooth | `QCA6390` (`3 Mbaud H4`, `BlueTool`) | P0 | ✅ | Verified headphone connection, A2DP playback, and cross-phone pairing |
| SLPI Core Sensors | `LSM6DSO` (accel/gyro), `AK0991x` (mag), `BMP285` (baro) | P1 | ✅ | FastRPC live stream + `mag_cal` 50 Hz Compass HID + pedometer + orient |
| Front & Rear Ambient Light | `TCS3701` (under-OLED) & `TSL2540` (rear) | P1 | ✅ | Both streaming over SLPI QRTR -> `backboardd` HID |
| Proximity (Ultrasonic) | ADSP `Xiaomi Proximity` | P1 | ❌ | Uses ADSP ultrasonic (not SLPI IR); unintegrated |
| Hall Sensor | `hall_key` (`TLMM SOUTH GPIO14`) | P2 | ❌ | RAZ/XPU-guarded at `0x0f50e000`; polling disabled |
| NFC Controller | `qcom,nq-nci` (`0x28` QUP1 `gpio4/5`) | P1 | 🚧 | `/dev/hakios-nfc` NCI 2.0 `CORE_RESET`/`CORE_INIT` HW handshake working; CoreNFC daemon pending |
| IR Blaster | `ir-spi@0` (`gpio116`) | P2 | ✅ | `/dev/hakios-ir` 38 kHz NEC TX ready |
| Under-Display Fingerprint (FOD) | `goodix,fingerprint` | P1 | ❌ | GPIOs surveyed (`10/23/24`); SPI/BiometricKit HAL pending |
| USB Charging & Fuel Gauge | `PM8150B` + gauge | P0 | ✅ | Live mV/mA/`%`, USB charging, `battery_stop` guard |
| 33W Wired Fast Charge | `bq25970@0x66` (`QUP15`) + `fsa4480@0x42` | P1 | 🚧 | Live 10 s I2C ADC & USB-C audio switch telemetry; active QC/PD pump control pending |
| 30W Wireless / 10W Reverse Charge | `idt,p9415@0x3b` / `ln8282@0x55` (`QUP16`) | P1 | ❌ | QUP16 bus probed idle-high; active WLC power/pad bring-up pending |
| USB Data Gadget (PC Connect) | `dwc3@a600000` (`snps,dwc3` `id=0x33313130`) | P0 | ❌ | Read-only HW MMIO telemetry verified; XNU gadget, `usbmux`, `lockdownd` (i4Tools/iTunes) pending |
| USB OTG Host Mode | `dwc3@a600000` / `a800000` | P1 | ❌ | External USB drives / HID / USB-C audio host unsupported |
| UFS Internal Storage | `ufshc@1d84000` (`hakios.ufs.c`) | P0 | 🚧 | Read-only partition & `modem_a` VFAT LFN reader working; guarded writable RW persistence pending |
| Real-Time Clock (RTC) | `qcom,pm8150_rtc` (`SPMI PPID 0x60`) | P1 | ✅ | SPMI RTC tick + `epoch=` Unix sync (`kern.hakios.umi_rtc_unix_time`) |

---

## 2. SoC Low-Level Capabilities

| Capability | Priority | Status | Current Notes |
|---|---|---|---|
| All 8 CPU Cores Online | P1 | 🚧 | 7 cores active (`bootcpu=7 cpus=7`); CPU0 reserved by UEFI loader |
| CPU Idle / Deep Sleep | P0 | 🚧 | ARM64 `WFI`/`WFE` + display blanking working; PSCI `CPU_SUSPEND` power collapse pending |
| GPU Dynamic Frequency Scaling | P1 | ✅ | GMU HFI `305 / 400 / 441.6 / 525 / 587 MHz` runtime scaling verified |
| VPU Hardware Video Decode | P0 | 🚧 | `venus=1` prepares firmware, `venus=2` cycles GCC/MVS0C power, `venus=3` verifies the reversible MVS0C XO clock path, and `venus=4` also verifies the AXI0 gate. PAS execution and HFI decode pending. |
| VPU Hardware Video Encode | P0 | ❌ | Depends on `pil-venus` HFI encode pipeline |
| ISP Pipeline (`Spectra 480` vs SW) | P1 | 🚧 | SW 5×5 Malvar-He-Cutler + sharpen + AE/AF; HW IFE/IPE/HDR pending |
| Hexagon DSPs (`ADSP`, `SLPI`, `CDSP`) | P1 | ✅ | `ADSP` and `SLPI` operational; `cdsp=2` verified TZ startup, fresh SMP2P ready and clean shutdown |
| NPU / AI Acceleration (`CDSP`) | P2 | 🚧 | `cdsp=7` verifies a capability RPC and isolated DMA, `cdsp=8` verifies guest attach/release, and `cdsp=10` verifies stock compute-process creation and release. `cdsp=11` verifies opening, invoking and closing the built-in current-process module in that address space. `cdsp=12` verifies listener initialization and registration in the user process. An uncommitted `cdsp=13` experiment served module file reads and timed out during module open. External module execution, HVX and Core ML remain open. |
| Hardware Crypto & PRNG | P0 | 🚧 | `qrng@793000` TRNG -> `mc_prng` (`✅`); `GCC_CE1` read (`qcedev@1de0000` crypto pending) |
| IOMMU / SMMU DMA Isolation | P0 | ✅ | Active SMMU contexts zero-fault across Wi-Fi, SDX55M, ADSP, GPU, SLPI |
| Hardware Watchdog | P1 | ✅ | `wdt=1` APSS WDT (`0x17c10000`, 16 s/32 s) 4 s keepalive & panic-safe quiesce |

---

## 3. Existing Subsystem Daily-Driver Completeness

| Subsystem | Working (`✅`) | Remaining (`❌` / `🚧`) |
|---|---|---|
| Cellular | 4G LTE dual-stack, VoLTE, native `CommCenter` | 5G SA/NSA (`Mav20`), Dual-SIM DSDS, SMS/OTP, USSD |
| Voice Calls | Outgoing VoLTE (`10099`), earpiece/speaker, dual mic | Incoming call ring/wake, mid-call route switch, ANC tuning |
| Wi-Fi | 2.4/5 GHz WPA2, DHCP/TLS, `CN`, 65 s wake, auto-rejoin | WPA3-SAE, hidden SSID, Personal Hotspot, 8 h standby |
| Bluetooth | 3 Mbaud H4, headphones A2DP playback, phone pairing | HFP/SCO call verification, AVRCP keys, HID keyboards/mice |
| GNSS | Settings toggle, Wi-Fi CL (`BaiduMap`), QMI SV stream | Outdoor 3D GPS+BeiDou fix, A-GPS XTRA, navigation |
| Camera | 27 MP rear + front switch, ~51 fps, AF/AWB, flash | Auxiliary lens integration, OIS servo, 108 MP mode, video rec, slow-mo, HDR, QR |
| Display / Touch | 89.08 Hz OLED TE, DCS brightness, multi-touch | 60/90 Hz dynamic switch, auto-brightness curve, DT2W |
| Audio | Stereo L/R `CS35L41` + earpiece, dual mic, USB-C switch probe | USB-C headset routing, automatic BT HFP/A2DP switch |
| Battery / Charging | Fuel gauge, USB charge, `bq25970` telemetry, thermal cap | Active QC/PD/33W & 30W wireless control, off-mode charge UI |
| Sensors | Accel, gyro, `mag_cal` compass, front+rear ALS, baro, orient, pedo | Step walk-test, ADSP proximity call screen-off, raise-to-wake |

---

## 4. OS-Level Readiness & Boundaries

| Layer | Status | Scope & Notes |
|---|---|---|
| Security & Privacy | 🚧 | TCC permission prompts work; Keychain persistence, passcode/FOD BiometricKit pending |
| Install & Storage | 🚧 | Cross-platform RAM boot launchers and local image builds. UFS persistence, standalone boot and OTA remain open. |
| Background Services | 🚧 | `launchd`, `locationd`, `wifid`, `CommCenter`, `backboardd` live; deep-sleep alarm/notifications pending |
| Networking | 🚧 | IPv4/IPv6, DNS, HTTPS TLS work; VPN (`utun`), proxy UI, Personal Hotspot pending |
| System Interaction | 🚧 | zh-Hans UI, PingFang, keyboard configuration, Control Center, Notification Center and switcher available. Input, notifications and functional controls have separate acceptance in section 6. |
| App Ecosystem | ✅ | `Sileo` (11 CN repos), `TrollStore` (`ldid`), `TrollApps` (6 CN sources), `WebKit`, `BaiduMap` |
| Apple services | ⛔ | Apple ID, App Store, FairPlay and stock WeatherKit lack end-to-end thyme verification. See [upstream review](UPSTREAM.md#apple-services-compatibility-review). iCloud, Apple Pay, APNs, iMessage and FaceTime remain unsupported. |

---

## 5. Long-Term Stability & Soak Gates

| Gate | Target | Status |
|---|---|---|
| Continuous Uptime | 168 h zero panic/WDT reset | ❌ (longest: 32 min @ 60 Hz, 21 min @ 90 Hz) |
| Screen-Off Standby | 8 h drain measurement | ❌ (65 s blank wake verified) |
| Lock / Wake Cycles | 500 consecutive cycles | ❌ |
| Cold Boots | 50 consecutive RAM boots | ❌ |
| Multi-App / Video / Handoff | 2 h video, app switch, Wi-Fi↔LTE | ❌ |
| Thermal / Memory / fsck | 30 min load, RSS leak, power-loss fsck | ❌ |

---

## 6. Native App and Daily-Use Acceptance

A PASS in this section requires completing the operation through the native
app or control on a physical thyme and observing its real effect. Keep the
build ID, operation, observed outcome and available log or capture. A
hardware, driver or UI milestone belongs in the earlier sections.
**Manual** marks interaction or equipment still needed. The acceptance status
remains open until that test passes.

### Camera, Photos, and Display

| Feature | Status | Acceptance on thyme |
|---|---|---|
| Rear ultra-wide live output | ❌ Manual | Select ultra-wide in Camera. Confirm the physical lens by occluding it and comparing field of view with the main sensor. Capture a real still, open it in Photos, and confirm the saved image came from that lens. |
| Auxiliary camera switching | ❌ Manual | Identify each independently exposed imager and its supported modes. Repeat main/ultra-wide/front and supported auxiliary switches. Verify real frames resume, the active sensor matches the selection, focus/exposure recover, and Camera works after reopen. |
| Zoom gestures, readout and dial | 🚧 Manual | Pinch in and out, tap available presets and drag the zoom dial in both directions. Confirm the displayed ratio follows the gesture, live framing changes, supported lens boundaries behave consistently and the saved photo matches preview framing. |
| Camera modes and Photos | ❌ Manual | Capture photo and video, verify playback, orientation and synced audio in Photos, then check recent thumbnail, edit/delete, permission prompts, and return to Camera. Test Portrait, slow-motion and QR separately where supported. |
| Manual brightness range | 🚧 Manual | Start from the minimum nonzero slider setting in Settings and Control Center, sweep to maximum and back, and check slider state across both surfaces. Measure or visibly confirm the panel responds across the range, especially at the low end. Verify the minimum stays nonzero. |
| Low-brightness stability | ❌ Manual | Hold the minimum visible setting in a dark room, change content, lock/wake repeatedly, and check for black frames, flicker or jumps. Use a photometer or fixed-exposure recording where available to corroborate panel output. |
| Auto-brightness | 🚧 Manual | Enable in Settings and move between controlled dark and bright conditions. Observe ambient readings and actual panel changes with plausible latency and hysteresis. Repeat with the setting disabled and after lock/wake. |

### Cellular, Phone, and Messages

| Feature | Status | Acceptance on thyme |
|---|---|---|
| Dual-SIM recognition and management | ❌ Manual (two active SIMs) | Insert two distinct SIMs, confirm concurrent registration, validate each line in Settings, and exercise line enable/disable, labels and line state after restart. Check incoming calls to both lines while idle. |
| Default data and voice lines | ❌ Manual (two active SIMs and peer phone) | Change default data and voice lines independently. Confirm carrier/line on an outbound call and real cellular traffic on the selected data line with Wi-Fi off. Test switching back and incoming calls on the non-default line. |
| Cellular data switch | 🚧 Manual (active SIM) | Turn data off in Settings and Control Center. With Wi-Fi off, confirm fresh network requests stop and the cellular route is removed. Re-enable and verify address, DNS and HTTPS recovery while the voice line remains usable. |
| Airplane mode and recovery | ❌ Manual (active SIM and peer phone) | Toggle from Settings and Control Center. Verify cellular registration and call/data behavior, independent Wi-Fi/Bluetooth toggles, and automatic cellular recovery without reboot. Repeat across lock/wake. |
| Native cellular SMS in both directions | ❌ Manual (active SIM and peer phone) | Send plain Latin and Chinese text from Messages to a real peer and receive replies. Verify correct sender/line, timestamps, delivery, thread placement and reply. Exercise while locked and on each line when dual SIM is available. |
| SMS OTP, tone, and lock-screen notification | ❌ Manual (approved OTP sender and peer phone) | Receive a real test OTP over SMS and verify the Messages thread, keyboard suggestion, SMS alert tone, banner, notification-center item, lock-screen preview/privacy and opening the correct thread. Check audible alerts with silent mode disabled and the silent path separately. Store redacted evidence only. |
| Phone integration | 🚧 Manual (peer phone) | Verify incoming/declined/missed calls, lock-screen answering, recent calls, earpiece/speaker switch and microphone mute during a real call. Verify resumed media and Bluetooth HFP when headphones are available. |

### Audio, Input, Voice Memos and System Controls

| Feature | Status | Acceptance on thyme |
|---|---|---|
| System volume and volume HUD | 🚧 Manual | Press both side buttons and drag the Control Center slider during media playback. Confirm HUD and sliders match the audible level, including min/max and mute. Check ringtone/alert volume independently, plus route changes to earpiece and headphones. |
| System sounds | ❌ Manual | Toggle keyboard clicks and lock sound individually and verify audible output on real typing and locking. Capture a real photo in the applicable region/mode and check shutter behavior. Preview and use a stock ringtone on an incoming call and a selected SMS tone on real delivery. |
| Silent mode and Focus | ❌ Manual | Toggle the native supported mute path, confirm its UI state, then verify ring, SMS, keyboard, lock and shutter behavior under the device's region rules. Verify media-volume independence, haptic policy, Focus notification handling and restoring audible alerts. |
| Pinyin and keyboard switching | 🚧 Manual | Type real pinyin into Notes and Messages, select a non-first candidate, correct and commit mixed Chinese/Latin text. Switch among Pinyin, Latin and emoji using the globe key and verify the committed text survives app relaunch within the RAM session. |
| Native Voice Memos | 🚧 Manual | Record spoken phrases with leading/trailing quiet, stop and name the memo, reopen Voice Memos, then play the entire recording and seek near both ends. Confirm duration, full audible content, gain and save/reopen. Reboot persistence is tracked under Install & Storage. |
| Settings and Control Center | 🚧 Manual | Cross-check tile and Settings state after toggling Wi-Fi, Bluetooth, data, Airplane Mode, brightness, volume and Focus. Verify actual radio/panel/audio effects, flashlight switching and state after lock/wake. Test accessible labels and permitted lock-screen controls. |
| Notifications, alarms and timers | ❌ Manual | Fire a Clock timer and alarm and a real app notification while unlocked, locked and in Focus. Verify sound/haptics, dismissal, correct app launch from Notification Center, and localized text. Deep-sleep and reboot behavior have separate gates. |
| Native apps and peripheral integration | 🚧 Manual | Exercise Safari page load/download, Files open/share, Photos playback/share, Maps location permissions, Calculator, Notes, and app switching with real state. Test A2DP/HFP and reconnect with a Bluetooth device, OTG with a supported USB device, and wireless charging with a pad. Record each result independently. |
