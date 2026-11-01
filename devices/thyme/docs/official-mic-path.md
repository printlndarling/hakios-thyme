# THYME microphone path

Date: 2026-11-02

## Build and evidence

Final kernel probe build:

```text
kernel sha256=a27d2dea225dfd54eb54292f3018908189310ec920d6d6ae58622423f332d3a0
loader sha256=f46783dc8df5973bc260f1853770a26466062a05c65253355b090584519afcf1
mic=2 stage=6 payload sha256=1942befc236f430bd3c71b904e6bffa45962c12d69bf6213022115084333c6e1
```

The tested kernel was built from `a420d234` with the THYME audio and RPMh
changes described below. The final physical evidence is in
`logs/official-mic2-final-tail.log`; the follow-up capture probes are in
`logs/official-mic3-*`.

## What now works

- The official THYME loader boots and its USB endpoint identifies as
  `product: xnuloader`, `version: 0.5`.
- GPIO32 is on the Kona SOUTH tile:
  `0x0f000000 + 0x500000 + 0x1000 * 32 = 0x0f520000`.
- GPIO32 must use GPIO mux 0. `func2` does not drive the reset line on this
  board, while mux 0 produces a real low/high reset pulse and full readback.
- The codec rails are voted through the existing RPMh client: PM8150 S4 at
  1800 mV and PM8150A BOB at 3296 mV. Both returned success.
- THYME's TX SoundWire pins are LPASS GPIO0, GPIO1 and GPIO2. The three slew
  controls write as `0x15` and read back.
- THYME has no `qcom,swrm-hctl-reg`; the HCTL workaround is skipped.
- The SoundWire master reports `COMP_STATUS=0x1` (link up).
- WCD938x TX enumerates as device 1, ID `0xd01170223`, status 1.
- SoundWire reads identify the codec:

```text
DIGITAL_CHIP_ID0 = 0x00
DIGITAL_CHIP_ID1 = 0x00
DIGITAL_CHIP_ID2 = 0x0d
DIGITAL_CHIP_ID3 = 0x01
SCP DEVICE_ID_0  = 0x23
SCP DEVICE_ID_1  = 0x02
```

- The VA macro CORE_ID reads `00/0f`, the three-macro Bolero 1.2 profile.
- VA core/NPL clocks `0x30b/0x310`, TX core/NPL clocks `0x30c/0x30d`, the
  SoundWire master, and all modified registers are restored in reverse order.
- The audio DSP and TrustZone shut down cleanly after the probe.

This is a PASS for loader, clock, pin, SoundWire, codec-enumeration and cleanup
behavior. It is not a PASS for microphone capture.

## Capture attempts

Both candidate THYME handset paths were exercised through `mic=3`:

1. Base `mixer_paths.xml`: `handset-mic -> dmic2 -> TX DMIC MUX2=DMIC1`,
   `TX DEC2 MUX=MSM_DMIC`, LPI GPIO6 clock / GPIO7 data, AFE channel mask 4.
2. `mixer_paths_overlay_static.xml`: WCD938x ADC1/AMIC path, `TX DEC1 MUX`,
   `TX SMIC MUX1=ADC0`, AFE channel mask 2.

In both cases the DSP accepts the stream and returns complete 20 ms periods
(48000 frames per second, no read errors), but every sample is zero:

```text
mic: second 1: 48000 frames (50 periods, 0 empty), level -99.9 dBFS (rms 0), peak 0, mean 0
```

The direct-DMIC follow-up also sampled the selected data line:

```text
mic: DMIC1 data line gpio7: 0 edges in 20000 samples (last 0)
mic: DMIC3 data line gpio9: 0 edges in 20000 samples (last 1)
```

Attempts with DMIC channels 0, 2 and 3 and with AFE masks 1 and 2 either gave
no read completion or the same zero data. LPI GPIO6 was confirmed as clock
output (DIR=2, OE=1), GPIO7 as input, and the VA DMIC clock divider as 4.

Android's runtime mixer state confirms the expected control names exist:

```text
TX DMIC MUX2        ZERO
TX DEC2 MUX         MSM_DMIC
TX_CDC_DMA_TX_3     One
TX_CDC_DMA_TX_3 SampleRate KHZ_48
```

`mixer_paths.xml` maps `handset-mic` to `dmic2`, while
`mixer_paths_overlay_static.xml` overrides it with the ADC1/DEC1 analog path.
The runtime selection of that overlay has not yet been proven.

## Next checks

- Determine which mixer overlay Android actually applies during a real
  recording, not only at boot.
- Check DMIC microphone power and clock arrival at the LPI pads, then verify
  whether the selected pair is the physical handset microphone.
- Re-audit the AFE channel mask and ADM/ASM route with a known nonzero source.
- Only move microphone capture to PASS after a spoken or injected sample shows
  nonzero RMS in both the HakiOS log and a recovered recording.

No flash, erase, partition, slot or Android-data modification was used for
these probes.
