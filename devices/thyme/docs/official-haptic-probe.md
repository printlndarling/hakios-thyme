# Official-main haptic probe

Date: 2026-11-01

## Build

The device-owned AW8697 driver builds as `CONFIG_HAKIOS_THYME_HAPTIC`; the
Redwood haptic driver is disabled.

## What passed

- GPIO 60/61 I2C, reset GPIO 130 and interrupt GPIO 129 were mapped.
- The chip answered at address `0x5a` with ID `0x97` (AW8697).
- The stock register configuration was written and read back.
- The VBAT measurement returned about 4384 mV.
- `aw8697_haptic.bin` was loaded into RAM and verified byte-for-byte.
- `/dev/hakios-haptic` was created for the iOS Actuator HAL.

```text
haptic: the chip at 0x5a identifies as 0x97 (AW8697)
haptic: UMI AW8697 RAM firmware: 4324 bytes from 0x800
haptic: UMI AW8697 waveforms loaded and verified
haptic: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR
```

## Remaining failure

Before a waveform can start, active-mode SYSST still reports the UVLO bit:

```text
haptic: active AW8697 SYSST=0x30 still reports UVLO; no waveform will start
```

The next hardware work is to establish which AW8697 supply is actually absent
or below threshold, compare the PMIC/boost state with Android, and correct the
activation sequence. This is not a waveform or I2C-path problem.
