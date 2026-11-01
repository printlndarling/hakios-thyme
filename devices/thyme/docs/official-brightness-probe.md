# Official-main brightness probe

Date: 2026-11-01

## Panel facts

The selected THYME panel node is:

```text
xiaomi 42 02 0a cmd mode dsc dsi panel
qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs"
qcom,mdss-brightness-max-level = <0xfff>
```

The panel therefore uses DCS `0x51` with a 12-bit level range (`0..0xfff`).

## Kernel path

`CONFIG_HAKIOS_THYME_DISPLAY` adds `hakios.thyme_display.c`. It:

- verifies the GCC display bus clock and DISPCC power/AHB clocks;
- maps DSI0 at `0x0ae94000`;
- checks that DSI is enabled in command mode;
- sends DCS `0x51` through the command DMA path;
- exposes `kern.hakios.brightness` in permille (`0..1000`).

The first hardware run logged:

```text
display: THYME brightness ready: DSI_CTRL=0x1f5, 600 permille -> level 2463 (skipped)
```

The initial write happened before the DSI command path became ready and was
correctly rejected without disturbing the display. Later writes from iOS
succeeded:

```text
display: THYME backlight 500 permille -> DCS 0x51 level 2056 (ok)
```

This is the command-DMA completion acknowledgement, not just a userspace log.

## Userspace path

`libhakios.dylib` now hooks:

- `BKDisplayBrightnessController` availability, initialization, getter and
  setter;
- `BrightnessSystemInternal` property reads and writes for
  `DisplayBrightness`.

The hooks write the requested brightness to `kern.hakios.brightness`. An iOS
boot produced:

```text
hakios: THYME brightness hooks installed (initial 0.60)
hakios: brightness (BKDisplayBrightnessController): 0.500 -> 500 permille
display: THYME backlight 500 permille -> DCS 0x51 level 2056 (ok)
```

The iOS backlight transaction completed normally:

```text
[Backlight] Beginning explicit brightness transaction ...
[Backlight] Ending transaction ...; 0 remain
```

## Root image update

Replacing the injected library on a compressed APFS file cannot be done safely
with an in-place byte write. The existing `root.apfs` was therefore extracted,
its `/lib/libhakios.dylib` replaced, and the volume rebuilt with the repository's
`mkapfs.py`.

Validation:

```text
check-image.py: apfsck -cw: clean
check-image.py: volume 0: 599 entries read back, every one as in the source tree
```

## Remaining check

The DSI command is acknowledged, and the iOS brightness path is active. A human
visual check should still confirm that the panel visibly changes at distinct
levels such as 20%, 50% and 100%, then the item can move from `PARTIAL` to
`PASS`.
