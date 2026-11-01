# THYME haptic microphone cross-check

Date: 2026-11-02

## Purpose

The AW8697 controller can report `GO`, enter its waveform-playing state and
return to standby even when the actuator or its supply path is faulty.
Therefore those controller states alone are not physical vibration evidence.
This test captured the WCD938x microphone while the iOS haptic service played
seven waveform-11 bursts.

## Configuration

```text
mic=3 mic_secs=18
mic_tap=1 mic_tap_count=7
mic_tap_start_ms=5000 mic_tap_period_ms=1800 mic_tap_on_ms=800
haptic=5 rec_sound=18 rec_sound_mic=1
```

Xiaomi's device tree identifies the LRA as a 1700 Hz part:

```text
vib_f0_pre = <0x6a4>          # 1700 Hz
vib_f0_coeff = <0x104>        # 260
vib_f0_cali_percen = <0x07>
```

Earlier source comments incorrectly carried over the Redwood actuator's
175.5 Hz value. That did not affect waveform playback because RAM waveforms
are precomputed, but it invalidated an earlier interpretation of the recorded
spectrum.

## Captured result

The microphone path delivered all 18 seconds without a short read:

```text
mic: readback summary: 1800 done, 0 errors, ready 0, sizes 1920 ... 1920
```

All seven controller pulses entered waveform 11 for about 756-761 ms and
returned to standby. The microphone's broadband RMS during and between those
pulses was effectively unchanged:

```text
active RMS by pulse (dBFS): -12.39 -12.34 -12.42 -12.36 -12.41 -12.38 -12.32
quiet  RMS by pulse (dBFS): -12.29 -12.34 -12.35 -12.34 -12.36 -12.35 -12.33
```

Averaging seven 600 ms waveform-aligned captures did not reveal the actuator's
1700 Hz fundamental:

```text
1700 Hz active power: -65.52 dB
1700 Hz quiet  power: -65.99 dB
```

The dominant small peaks were unrelated room/fixture noise, primarily around
246, 313, 3973 and 4000 Hz. There was no repeatable pulse-locked component at
the motor's actual resonance.

## Conclusion

This microphone cross-check is **INCONCLUSIVE**, not a pass. The LRA is
mechanically isolated from the microphone well enough that its 1700 Hz motion
is below the recorded noise floor. The result must not be used to mark
haptics as working.

The next physical check uses the AW8697's own continuous-mode BEMF/f0
measurement from Xiaomi's driver. A plausible 1700 Hz result requires
back-EMF from a moving coil and is materially stronger evidence than controller
state transitions alone.

## Evidence

The full raw captures and kernel traces are ignored in Git:

```text
logs/official-haptic-repeat-tail.log
logs/official-haptic-repeat.rec
logs/official-haptic-repeat-average.wav
logs/official-haptic-mic-tail.log
logs/official-haptic-mic.rec
```
