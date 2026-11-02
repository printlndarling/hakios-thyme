# THYME AW8697 BEMF validation

Date: 2026-11-02

## Test build

```text
repo HEAD before this evidence commit: bccf8b3
kernel after f0/BEMF fixes:          d0d3e3315af6c78038e30557a80d2954a5773600a9cecde043761c711fdfe1af
payload SHA-256:                     4346b99e057a2851826d864d3585d9d81770ded7ff4e8afc45d90e1f74344f44
cmdline:
  haptic=3 haptic_f0_probe=1
  loader_return_after=45
```

The test is RAM-only: `fastboot boot xnu-usb.img`, `stage`, and `oem run`.
Nothing was flashed, erased or written to a partition.

## Method

The probe follows Xiaomi's AW8697 continuous-mode sequence:

1. Save every register the diagnostic changes.
2. Clear `trim_lra`.
3. Select continuous mode, active state, f0 detection, the 1 kHz feedback
   low-pass and the register-provided LRA oscillator.
4. Use the stock 1700 Hz preset (`0x08d6`), drive level `0x26` and trace
   parameters `0x93/0x01/0x1f`.
5. Wait for the complete 0xfe-period trace, then poll `GLB_STATE`.
6. Read measurement registers one byte at a time; this chip does not reliably
   auto-increment `0x68/0x69`, `0x6a/0x6b` or `0x70/0x71`.
7. Require standby, `SYSINT.DONEI`, and a non-zero positive or negative BEMF
   peak.
8. Restore every touched register, including `trim_lra`.

## HakiOS results

Two runs:

```text
haptic: AW8697 f0/BEMF probe complete:
  F0=0x444 -> 3522 Hz (advisory), CONT=0000, BEMF=0000,
  positive=47, negative=200, SYSST=0x30, SYSINT=0x01
haptic: AW8697 f0/BEMF probe PASS

haptic: AW8697 f0/BEMF probe complete:
  F0=0x446 -> 3515 Hz (advisory), CONT=0000, BEMF=0000,
  positive=48, negative=200, SYSST=0x30, SYSINT=0x01
haptic: AW8697 f0/BEMF probe PASS
```

Earlier runs included valid and invalid F0 values while the BEMF peaks stayed
near `+45/-202`.

## Stock Android cross-check

The stock driver's boot calibration logged:

```text
aw8697_haptic_read_f0 f0=1764
aw8697_haptic_read_beme max_pos_beme=45
aw8697_haptic_read_beme max_neg_beme=202
aw8697_haptic_f0_calibration final trim_lra=0x0f
```

Five consecutive reads of stock's `sysfs cali` returned:

```text
1282051 Hz
769230 Hz
3427 Hz
1700 Hz
3581 Hz
```

This proves F0 is not a reliable repeated hardware check on this UMI. HakiOS
therefore reports it as advisory and uses the repeatable BEMF peak response
as the hardware result.

## Conclusion

- PASS: the AW8697 receives continuous drive, produces a repeatable back-EMF
  response matching Android, completes the trace (`DONEI`) and returns to
  standby.
- ADVISORY: single-shot F0; both HakiOS and stock Android return unstable
  values on repeated measurements.
- STILL PENDING: user-perceptible vibration, iOS taps/long vibrations through
  the Actuator HAL, and long-term service stability.

Raw logs are ignored in Git:

```text
logs/official-haptic-f0-bemf-final-tail.log
logs/official-haptic-f0-bemf-final2-tail.log
logs/official-haptic-f0-trace-tail.log
```
