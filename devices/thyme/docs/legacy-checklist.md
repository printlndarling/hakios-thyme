# Xiaomi Mi 10S (thyme) daily-use checklist

Status values:

- **PASS**: observed on the physical THYME in the current RAM-only boot path.
- **PARTIAL**: the driver or firmware path starts, but user-visible behavior is incomplete or unverified.
- **FAIL**: hardware is present, but the required path does not work yet.
- **NOT TESTED**: no trustworthy physical result is recorded.
- **OUT OF MVP**: useful, but not required for the first "barely daily usable" gate.

The minimum daily-use gate is display, touch, working buttons, stable GPU,
charging without false state, Wi-Fi, Bluetooth, speaker audio, haptics, sleep /
wake, and safe battery protection. Camera, cellular, NFC, fingerprint and
always-on sensors can follow after that gate.

## Safety and test discipline

- [x] Use `fastboot boot` only; never flash, erase, change slots, or write Android/data.
- [x] Address the phone as ADB `5290780a` and fail closed on any other serial.
- [x] Keep source, builds and logs on the WSL ext4 filesystem.
- [x] Return to Android for charging if XNU charging is not yet trustworthy.
- [x] Keep an external charge timer active for every long RAM session.
- [x] Confirm the target's USB instance before recovery if fastboot loses its serial.
- [ ] Add one script that performs serial checks, boot, staging, timed recovery and log collection.
- [ ] Add machine-readable pass/fail assertions for display, touch, GPU and battery logs.

## P0: boot and interaction

| Check | Status | Evidence / next test |
|---|---|---|
| THYME DTB accepted | PASS | Hardware boots through `xnuloader`. |
| XNU reaches userspace | PASS | SpringBoard home screen reaches the panel. |
| 7 CPUs and cpufreq | PASS | Seven cores start; cpufreq path is active. |
| 1080 x 2340 display | PASS | Screenshots decode as 1080 x 2340 BGRA. |
| Hardware GPU rendering | PASS | FD650 Mesa path renders SpringBoard. |
| 20-minute UI stability | PASS | No return-to-loader, backboardd SIGSEGV or softpipe fallback in the 20-minute stable run. |
| FTS touch polling | PASS | Idle polls remain `errors=0`; a physical touch raised the event count from 7 to 239. |
| Physical tap | PASS | A physical tap opened Preferences; backboardd reported contact presence touching/none. |
| Physical drag | PASS | Continuous FTS MOVE events reached backboardd and SpringBoard. |
| Edge/up-swipe gesture | PARTIAL | SpringBoard's system-gesture recognizer consumed a swipe; the final home/app-switcher result still needs a dedicated check. |
| Power button | PASS | Short presses produced `usage:0x30` down/up events without restarting SpringBoard. |
| Volume buttons | PASS | Volume-up/down produced the expected HID `0xE9` / `0xEA` down/up events. |
| No random flash/text overlay | PASS | Stable payload uses `loader_return_after=0`, no `-v` and no `fb_scan=1`. |

## P0: power, battery and charging

| Check | Status | Evidence / next test |
|---|---|---|
| Battery percentage/voltage/temperature | PASS | Loader and SpringBoard report real values. |
| Signed current measurement | PASS | Negative current is visible when the charger is inactive. |
| Qualcomm `TakeAction` charging | PARTIAL | Action 7 -> 3 produced `+1009 mA`, later `+93..125 mA`. |
| Charger firmware errors | PASS | More than 100 calls completed without a reported error in the first run. |
| Loader USB endurance during charging | FAIL | A later periodic-action session lost the USB endpoint after about five minutes. |
| One-shot TakeAction mitigation | PARTIAL | Calls stopped at 2 (`action 7`, then `action 3`); signed current remained positive (`+1127 mA` initially, `+95 mA` at poll 53). The 30-minute endpoint is still running. |
| `battery_stop` reads real state | PASS | Guard reports `97%`, battery present, charger attached, charging yes. |
| Low-battery automatic return | NOT TESTED | Controlled discharge test after charging path is stable. |
| Android charge recovery | PASS | Timed return starts Android and `status: 2`. |
| PC USB net current at high SOC | PARTIAL | Host port is only 500 mA; net battery current can be small near full. |
| Sleep / display off | PASS | Power short press changed backboardd to `displayState: off`, disabled the digitizer and entered WakeOnTap mode. |
| Wake from power button/touch | PARTIAL | A second power press returned `displayState: on`; touch-to-wake still needs a dedicated test. |
| Thermal guard | NOT TESTED | Sustained GPU/charge load while logging SoC and battery temperature. |

## P1: networking

| Check | Status | Evidence / next test |
|---|---|---|
| QCA6390 PCIe link | PASS | PCIe link and MHI path start. |
| SMMU/MHI firmware load | PASS | Firmware collection and MHI initialization reach the firmware stage. |
| THYME board file | PARTIAL | Source accepts firmware `0x1016046e` and maps `bd_j2s.elf`; hardware run pending. |
| WLFW QMI service | FAIL | Previous run timed out with QMI/WLFW request `-60`. |
| WLAN station creation | FAIL | No usable station yet. |
| Scan/join/WPA/DHCP | NOT TESTED | Depends on station creation. |
| AirDrop/AirPlay/iOS network services | OUT OF MVP | Test after station connectivity. |
| Bluetooth UART transport | NOT TESTED | Identify the THYME QCA6390 UART instance and pins. |
| BT firmware/NVM load | NOT TESTED | Load `htbtfw20.tlv` and `htnv20.bin`. |
| HCI reset | FAIL | bluetoothd currently reports no usable HCI controller. |
| Pair/connect/audio | OUT OF MVP | Depends on HCI reset and a valid local address. |

## P1: audio and haptics

| Check | Status | Evidence / next test |
|---|---|---|
| ADSP firmware load | PARTIAL | Firmware starts and links; some services remain unavailable. |
| Speaker amplifier probe | PARTIAL | Driver path starts; physical output unverified. |
| Speaker playback | NOT TESTED | Play a known tone and a media file at several volumes. |
| Microphone capture | NOT TESTED | Record and play back a short sample. |
| Headset detection | NOT TESTED | Insert/remove wired headset. |
| AW8697 detection | PASS | Controller and waveform are detected. |
| Haptic UVLO handling | FAIL | UVLO / enable path still needs a hardware fix. |
| Physical vibration | NOT TESTED | Trigger keyboard/haptic feedback and system vibration. |

## P2: storage, camera and sensors

| Check | Status | Evidence / next test |
|---|---|---|
| UFS read-only survey | NOT TESTED | Run the existing read-only UFS survey before any write path. |
| Persistent filesystem | PARTIAL | Root/system images mount; persistent user storage policy is not defined. |
| Camera probe | NOT TESTED | THYME sensor tables and firmware are collected. |
| Rear camera capture | OUT OF MVP | Probe and power rails must pass first. |
| Flash/torch | PARTIAL | Torch register path exists; physical test pending. |
| Ambient light sensor | MISSING | ALS timeout remains; manual brightness works. |
| Auto-brightness | OUT OF MVP | Requires ALS. |
| Accelerometer/orientation | FAIL | Backboardd reports missing accelerometer HID service. |
| Proximity sensor | NOT TESTED | Lock-screen pocket behavior needs a real sensor path. |

## P3: product-level gaps

- [ ] Cellular modem and iOS telephony path.
- [ ] NFC.
- [ ] Fingerprint.
- [ ] Face ID substitute / documented lock-screen policy.
- [ ] OTA or repeatable installer workflow.
- [ ] Userdata backup and recovery workflow for this third-party Android ROM.
- [ ] Performance and thermal soak: 30 minutes browsing/rendering, 30 minutes charging.
- [ ] Battery cycle integrity: no unexplained percentage jumps or negative-current charging state.

## Current immediate order

1. Finish the running 30-minute one-shot charging/thermal run without losing `xnuloader`.
2. Verify the final home/app-switcher result of the edge/up-swipe gesture.
3. Physically verify power and volume buttons.
4. Run display-off/wake and charging/thermal soak tests.
5. Capture a fresh Wi-Fi run with the THYME board file and diagnose the WLFW `-60` timeout.
6. Finish BT UART identity and HCI reset.
7. Verify speaker and microphone, then fix haptic UVLO.

## Prepared isolated payloads

`devices/thyme/build-test-payload.sh NAME "arguments"` builds each test on top of
the stable display, touch, key, charging and battery-safety command line.

| Payload | Extra arguments | Purpose |
|---|---|---|
| `run-payload-stable.bin` | none | Daily baseline. |
| `run-payload-bt.bin` | `umi_bt=1` | QCA6390 UART firmware, HCI reset and iOS H4 transport. |
| `run-payload-audio.bin` | `adsp=1 sound=4 mic=4` | Speaker and microphone through the audio DSP. |
| `run-payload-haptic.bin` | `umi_haptic=5` | AW8697 vibration service. |
| `run-payload-bt-haptic.bin` | `umi_bt=1 umi_haptic=5` | Combined test after both pass independently. |
