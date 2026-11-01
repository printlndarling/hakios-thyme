# Official-main AW8697 haptic bring-up

Date: 2026-11-01

## Build

The device-owned AW8697 driver builds as `CONFIG_HAKIOS_THYME_HAPTIC`; the
Redwood haptic driver is disabled. THYME configuration now uses
`CONFIG_UMI_HAPTIC=5`, which emits `umi_haptic=5`. `haptic=1..5` remains an
alias inside this THYME-only source so it cannot select Redwood's GPIO/I2C path.

## Stock comparison

The live Android device tree and Xiaomi SM8250 driver were used as the
authority:

- AW8697 is at I2C `0xa8c000` bus address `0x5a`, reset GPIO 130, IRQ GPIO 129.
- Android's working driver leaves `SYSST=0x30` at rest. Bit 5 is therefore not,
  by itself, a fatal under-voltage indication.
- The stock RAM sequence selects RAM with the boost bypassed, enters active,
  clears `SYSINT`, unmasks UVLO, and then enables the boost.
- The stock IRQ handler logs UVLO, over-voltage, over-current,
  over-temperature and completion, but ignores `BSTERRI`.
- `BSTDBG4` uses `vib_bst_vol_ram = 0x10`, giving register value `0x60`.
- The latest stock f0 calibration wrote `trim_lra = 0x0c`.

Reference:

- https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/6b34d24765327d746074c8e53b5ad0d24a51acc7/drivers/input/misc/aw8697_haptic/aw8697.c

## Verified on the phone

- GPIO 60/61 I2C, reset GPIO 130 and interrupt GPIO 129 were mapped.
- The chip answered at address `0x5a` with ID `0x97` (AW8697).
- The stock register configuration was written and read back.
- The VBAT measurement returned 4312 mV.
- `aw8697_haptic.bin` was loaded into RAM and verified byte-for-byte.
- Active mode completed with the stock boost sequence.
- Waveform 1 played at the stock-derived diagnostic gain `0x1e` and returned
  to standby in 38 ms.
- No fatal UVLO, over-voltage, over-current or over-temperature fault occurred.
- The kernel returned to the loader, which was then rebooted to Android.

```text
haptic: the chip at 0x5a identifies as 0x97 (AW8697)
haptic: UMI AW8697 RAM firmware: 4324 bytes from 0x800
haptic: UMI AW8697 waveforms loaded and verified
haptic: UMI AW8697 waveform 1 is played at diagnostic gain 0x1e
haptic: 0x3a <- 0x60, reads 0x60
haptic: before UMI playback: AW8697 flags 0x80 carry BSTERRI before GO; stock Android ignores this boost latch
haptic: UMI waveform returned to standby after 38 ms
```

The full captured trace is ignored in Git at
`logs/official-haptic-aw8697-probe.log`.

## Remaining checks

- Confirm that the 38 ms waveform is physically perceptible.
- Run `umi_haptic=5` with SpringBoard and verify short taps and long vibrations
  from the iOS Actuator HAL.
- Compare `haptic_gain=0x1e` (stock-derived) with the current iOS ceiling
  `0x40`; keep the ceiling conservative if the stronger setting is harsh.
