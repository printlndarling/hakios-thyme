# THYME microphone path

Date: 2026-11-02

## Build and evidence

Port-mapping correction run:

```text
kernel sha256=8ce8492840d073d8eaf9808e5683738aca3133ec9926fe03092f6d4960f7f135
mic=3 payload sha256=df0971c13ae0d72ae3333efc81a12225a17cecdd118d6dd3ea4a1b582d79cf7c
log=logs/official-mic3-analog-port2-tail.log
```

Single-channel default run:

```text
kernel sha256=7d0bcf1d343560a78399b595197d25337503505be7e260c65323c35f4ab80ee2
mic=3 payload sha256=b7b3d742ea1114408d3513599b23649dcd1a02cf55ca9b9ceaf783746b163b75
log=logs/official-mic3-default-tail.log
```

Dual-channel run:

```text
kernel sha256=a66228d13b23dc69b1995d7b185415eb21cc405706501e593d6950a88a07eba7
mic=3 payload sha256=37c482d18767c9ac88c35a65df6b3f42aa0005daf5248fd885b92f496f73ee23
log=logs/official-mic3-dual-default-tail.log
```

Loader RAM image used for all runs:

```text
sha256=f46783dc8df5973bc260f1853770a26466062a05c65253355b090584519afcf1
```

The code was built from `a420d234` plus the THYME audio and RPMh changes.

## Verified transport

- The official THYME loader boots and identifies as `product: xnuloader`,
  `version: 0.5`.
- GPIO32 is on Kona's SOUTH tile:
  `0x0f000000 + 0x500000 + 0x1000 * 32 = 0x0f520000`.
- GPIO32 must use GPIO mux 0. The DTB label `func2` does not drive the reset
  line on this board; mux 0 produces a real low/high pulse and full readback.
- PM8150 S4 at 1800 mV and PM8150A BOB at 3296 mV are voted through the
  existing RPMh client.
- THYME's TX SoundWire pins are LPASS GPIO0, GPIO1 and GPIO2. Their slew
  controls write as `0x15` and read back.
- THYME has no `qcom,swrm-hctl-reg`, so the Android HCTL workaround is skipped.
- The SoundWire master reports `COMP_STATUS=0x1`.
- WCD938x TX enumerates as device 1, ID `0xd01170223`, status 1.
- Codec ID reads return:

```text
DIGITAL_CHIP_ID0 = 0x00
DIGITAL_CHIP_ID1 = 0x00
DIGITAL_CHIP_ID2 = 0x0d
DIGITAL_CHIP_ID3 = 0x01
SCP DEVICE_ID_0  = 0x23
SCP DEVICE_ID_1  = 0x02
```

- The VA macro reports Bolero 1.2 (`00/0f`).
- VA core/NPL (`0x30b/0x310`) and TX core/NPL (`0x30c/0x30d`) clocks come up,
  and all modified registers are restored in reverse order.
- The audio DSP and TrustZone shut down cleanly.

## Capture works

SoundWire routing follows the THYME DTS:

- ADC1 (`0x12`): codec TX port 0/channel 1 -> master port 2/channel 1.
- ADC4 (`0x15`): codec TX port 1/channel 2 -> master port 3/channel 2.
- The TX macro uses DEC1/ADC0 for ADC1 and DEC0/ADC3 for hardware ADC4.
- `TX_CDC_DMA_TX_3`, ADM and ASM carry two active channels (mask 3).

The earlier code used master port 1 for ADC1 and therefore returned exact
silence. The corrected single-channel path produced nonzero samples, and the
completed dual path produced:

```text
TX SoundWire master DP_PORT_CTRL (port 2): channel 1 ... reads back
TX SoundWire master DP_PORT_CTRL (port 3): channel 2 ... reads back
mic: second 1: 48000 frames (50 periods, 0 empty), level -16.8 dBFS (rms 4854), peak 28524, mean 479; reads 50, errors 0
mic: second 2: 48000 frames (50 periods, 0 empty), level -12.5 dBFS (rms 7831), peak 27990, mean 0; reads 100, errors 0
mic: second 3: 48000 frames (50 periods, 0 empty), level -12.3 dBFS (rms 7930), peak 28409, mean -23; reads 150, errors 0
```

This is a hardware PASS for dual-channel microphone capture and cleanup. It is
not yet a complete user-facing PASS because `/dev/hakios-mic` has not recorded
through Darwin with this path.

## Remaining checks

- Correlate a known speaker tone or spoken sample with captured RMS.
- Exercise `mic=4` and `/dev/hakios-mic` from Darwin with the corrected path.
- Verify mono downmix and recording continuity when Darwin consumes the ring.
- Only mark the complete microphone item PASS after a recovered recording
  contains intelligible audio.

The diagnostic direct-DMIC path (`mic_path=0`, DMIC1/DEC2, LPI GPIO6/7) still
returns silence and zero data-line transitions. It is not the default.

No flash, erase, partition, slot or Android-data modification was used for
these probes.
