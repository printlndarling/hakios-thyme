# THYME speaker-to-microphone acoustic loopback

Date: 2026-11-02

## Result

PASS for speaker acoustic output. The known 1 kHz self-test tone was present in
the microphone recording only while the CS35L41 outputs were playing.

```text
kernel sha256=fdf4509b58134906366d3f0c2b1e460136198003dd5f7ce0dac0d2ae2eddef14
payload sha256=709e7c5067fbd34f54a8dbc9662d24f80d765e64280c41321ba3aae11adb30e1
recording sha256=6e053b482a4d3ec366dfc8f06f674f9874e85cdd6252ba6f02a3dc4d1d73d353
log sha256=083b40cbee9a4bc598419ef4d753810dda9cee4428881360e6d258bc8a7f0129
```

Local evidence is retained as:

```text
logs/official-speaker-mic-loop6-tail.log
logs/official-speaker-mic-loop6.rec
logs/official-speaker-mic-loop6-analysis.txt
```

## Run

The image used:

```text
adsp=1 adsp_umi=1 sound=3 sound_secs=3 sound_below=96
mic=3 mic_secs=40 rec_sound=40 rec_sound_mic=1
```

This is a RAM boot through `fastboot boot`, `stage` and `oem run`. No
partition, slot, Android or user-data write was made.

The speaker path initialized both CS35L41 protection DSPs, applied a fallback
speaker resistance of 8392 ohms, started both outputs at -40 dB, ramped to
-12 dB and played 3 seconds of 1 kHz PCM at -20 dBFS:

```text
sound: UMI SPEAKER TONE NOW: 3 s, 1 kHz at -20 dBFS, 12.000 dB under unity
sound: 159 periods written, 159 given back (0 with an error), 0 underflows
```

The concurrent microphone path continuously returned 1920-byte dual-channel
periods:

```text
mic: second 29: 48000 frames (100 periods, 0 empty), ... reads 2900, errors 0
[sound: UMI SPEAKER TONE NOW]
mic: second 30: 48000 frames (100 periods, 0 empty), ... reads 3000, errors 0
mic: second 31: 48000 frames (100 periods, 0 empty), ... reads 3100, errors 0
mic: second 32: 48000 frames (100 periods, 0 empty), ... reads 3200, errors 0
[sound: LEVEL DONE]
mic: second 33: 48000 frames (100 periods, 0 empty), ... reads 3300, errors 0
...
mic: readback summary: 4000 done, 0 errors, ready 0x80, sizes 1920 ... 1920
```

## Frequency analysis

The loader left the microphone PCM recording in memory (`RWSND001`); it was
downloaded with `oem sound` and analyzed offline. A one-second Goertzel
measurement at 1 kHz gives:

```text
baseline median (seconds 25-28 and 33-36): -18.24 dBFS
tone median (seconds 29-32):                +7.54 dBFS
increase:                                   25.78 dB
```

The rise starts with `UMI SPEAKER TONE NOW` and falls immediately when the
tone ends. This verifies the complete physical chain: AFE/MI2S, CS35L41
protection and output stages, speaker acoustics, WCD938x microphone capture,
SoundWire, ADM/ASM and the shared SMMU path.

## Fixes found by the test

- One-second microphone statistics are no longer reset by every 20 ms read.
- An asynchronous microphone read whose APR transmit failed is retried for at
  most 100 ms instead of being silently lost.
- A non-Darwin run now ends with a readback count, error count, ready mask and
  all eight last period sizes.

Independent left/right speaker output and stereo balance remain to be tested.
