# Official-main AW8697 haptic bring-up

Date: 2026-11-02

## Build

The device-owned AW8697 driver builds as `CONFIG_HAKIOS_THYME_HAPTIC`; the
Redwood haptic driver is disabled. THYME configuration uses
`CONFIG_UMI_HAPTIC=5`, which emits `umi_haptic=5`. `haptic=1..5` remains an
alias inside this THYME-only source so it cannot select Redwood's GPIO/I2C
path.

## Stock comparison

The live Android device tree and Xiaomi SM8250 driver were used as the
authority:

- AW8697 is at I2C `0xa8c000` bus address `0x5a`, reset GPIO 130, IRQ GPIO 129.
- Xiaomi's device tree identifies a 1700 Hz LRA:
  `vib_f0_pre = <0x6a4>`, `vib_f0_coeff = <0x104>`, and
  `vib_f0_cali_percen = <0x07>`.
- An earlier comment carried over Redwood's 175.5 Hz actuator value. RAM
  playback was unaffected because its waveform is precomputed, but microphone
  analysis must use 1700 Hz for THYME.
- Android's working driver leaves `SYSST=0x30` at rest. Bit 5 is not, by
  itself, a fatal under-voltage indication.
- The stock RAM sequence selects RAM with the boost bypassed, enters active,
  clears `SYSINT`, unmasks UVLO, and then enables the boost.
- The stock IRQ handler logs UVLO, over-voltage, over-current,
  over-temperature and completion, but ignores `BSTERRI`.
- `BSTDBG4` uses `vib_bst_vol_ram = 0x10`, giving register value `0x60`.
- The latest stock f0 calibration wrote `trim_lra = 0x0c`.

References:

- https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/6b34d24765327d746074c8e53b5ad0d24a51acc7/drivers/input/misc/aw8697_haptic/aw8697.c
- https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/6b34d24765327d746074c8e53b5ad0d24a51acc7/drivers/input/misc/aw8697_haptic/aw8697_reg.h

## Verified on the phone

- GPIO 60/61 I2C, reset GPIO 130 and interrupt GPIO 129 were mapped.
- The chip answered at address `0x5a` with ID `0x97` (AW8697).
- The stock register configuration was written and read back.
- The VBAT measurement returned 4312 mV.
- `aw8697_haptic.bin` was loaded into RAM and verified byte-for-byte.
- Active mode completed with the stock boost sequence.
- Waveform 1 played at the stock-derived gain `0x1e` and returned to standby
  in 38 ms.
- Seven waveform-11 bursts at gain `0x40` each completed in about 761 ms with
  no fatal UVLO, over-voltage, over-current or over-temperature fault.
- Both microphone captures completed with zero read errors while the motor
  service ran.
- The microphone could not detect the LRA at 1700 Hz above its noise floor.
  That result is recorded as inconclusive in
  `official-haptic-mic-analysis.md`; it is not physical vibration evidence.

```text
haptic: the chip at 0x5a identifies as 0x97 (AW8697)
haptic: UMI AW8697 RAM firmware: 4324 bytes from 0x800
haptic: UMI AW8697 waveforms loaded and verified
haptic: UMI AW8697 waveform 1 is played at diagnostic gain 0x1e
haptic: 0x3a <- 0x60, reads 0x60
haptic: before UMI playback: AW8697 flags 0x80 carry BSTERRI before GO; stock Android ignores this boost latch
haptic: UMI waveform returned to standby after 38 ms
```

The full captured traces are ignored in Git under `logs/official-haptic-*`.

## Continuous-mode BEMF/f0 diagnostic

`haptic_f0_probe=1` runs the same non-persistent continuous-mode measurement
as Xiaomi's driver after configuration and waveform loading. It temporarily
clears `trim_lra`, drives the LRA at the stock continuous level, reads:

- `F_LRA_F0_H/L` (`0x68/0x69`), converted with the stock 260 coefficient;
- `F_LRA_CONT_H/L` (`0x6a/0x6b`);
- BEMF voltage (`0x70/0x71`) and positive/negative BEMF peaks (`0x6d/0x6f`);
- playing state, `SYSST` and `SYSINT`;

then restores every touched configuration register and the saved trim value.
The measurement registers are read one byte at a time, as the vendor driver
does, because this chip does not reliably auto-increment them.

Two RAM runs produced repeatable BEMF peaks:

```text
F0=0x444 -> 3522 Hz (advisory), positive=47, negative=200, SYSINT=0x01
F0=0x446 -> 3515 Hz (advisory), positive=48, negative=200, SYSINT=0x01
```

Android's stock driver reports `f0=1764`, `positive=45`, `negative=202` at
boot. Re-reading stock's `sysfs cali` repeatedly returned
`1282051 / 769230 / 3427 / 1700 / 3581 Hz`, proving the F0 result is unstable
outside HakiOS too. The probe therefore passes on completion + `DONEI` +
a non-zero BEMF peak response, and reports F0 only as advisory.

See `official-haptic-bemf-validation.md` for the complete evidence.

## Remaining checks

- Confirm user-perceptible vibration and strength.
- Run `umi_haptic=5` with SpringBoard and verify short taps and long vibrations
  from the iOS Actuator HAL.
- Compare stock gain `0x1e`, the iOS ceiling `0x40`, and the lowest usable
  level; keep the ceiling conservative if the stronger setting is harsh.
