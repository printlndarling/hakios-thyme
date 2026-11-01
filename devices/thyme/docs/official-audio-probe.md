# Official-main audio probe

Date: 2026-11-01

## Build

```text
kernel.ver=2.1.13-hakios-darwin22.5-sm8250-g56c2867c-dirty
payload sha256=ce318aa182a85b0aadedfc4edc6c468979ad3ca1bcf4161177de033e73d44706
```

The device-owned audio stack builds as `CONFIG_HAKIOS_THYME_AUDIO`:

```text
hakios.thyme_adsp.c
hakios.thyme_speaker.c
hakios.thyme_sound.c
hakios.thyme_mic.c
hakios.thyme_guard.c
```

The official Redwood ADSP, SPEAKER, SOUND and MIC drivers are disabled.

## DSP and routing

The audio DSP started and linked, and its audio services came up:

```text
adsp: THE AUDIO DSP IS UP AND LINKED, its audio services up (APR)
sound: UMI I2S configuration: done
sound: UMI tertiary MI2S clock: 1.536 MHz, on: done
sound: UMI port 0x1004, started: done
sound: THE AUDIO CLOCK AND FRAME SYNC RUN at the pins
sound: the stream, opened for writing: 16-bit linear PCM, no processing: done
sound: session 1 routed to the device path: done
sound: the session, run: done
```

## Amplifiers

Both CS35L41 amplifiers were found and their protection firmware was loaded
with readback verification.

```text
umi-audio: amp 0x40 OTP map 0x8: 99 trim fields, applied
umi-audio: amp 0x40: protection firmware ... verified in DSP memory
umi-audio: amp 0x40 HALO firmware 0x400a4 version 0x300, 1 algorithms
umi-audio: amp 0x40 HALO mailbox cmd 1: ack=1 state=1 (expected 1)
umi-audio: amp 0x42 OTP map 0x8: 99 trim fields, applied
umi-audio: amp 0x42: protection firmware ... verified in DSP memory
umi-audio: amp 0x42 HALO mailbox cmd 1: ack=1 state=1 (expected 1)
sound: WHAT IOS PLAYS GOES TO THE SPEAKERS from here on
```

The iOS HAL opened `/dev/hakios-audio` and the stream was routed to the CS35L41
speaker path.

## Self-test correction

The THYME `sound=3` path originally left the amplifiers at a fixed `-40 dB`
while playing a `-20 dBFS` tone, and ignored `sound_secs`. That made an
acoustic loopback test nearly impossible. The self-test now applies the
requested `sound_below` attenuation and plays for the requested duration, up
to the three-second test limit.

Example:

```text
adsp=1 sound=3 sound_secs=3 sound_below=96 mic=3 mic_secs=8
rec_sound=8 rec_sound_mic=1 mic_tap=1 umi_haptic=5
```

This is intended to record the speaker tone and the motor through the
microphone. Hardware verification is pending.

## Remaining physical checks

- Play a known system sound or music and confirm audible stereo output.
- Record a spoken sample and verify microphone level and playback.
- Observe amplifier temperature and battery voltage under sustained audio.

The run ended through the 240-second watchdog and returned to Android.
