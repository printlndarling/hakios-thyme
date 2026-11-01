# Official-main touch and keys probe

Date: 2026-11-01

## Purpose

Bring the device-owned ST FTS521 touch controller and PM8150 key driver onto the
official common kernel without modifying or replacing the official Redwood
drivers.

## Integration

`CONFIG_HAKIOS_THYME_INPUT` builds four files from the device repository:

```text
hakios.thyme_touch.c
hakios.thyme_touch_boost.c
hakios.thyme_rpmh.c
hakios.thyme_keys.c
```

The official stubs leave `hakios_goodix_init()` and `hakios_buttons_init()` to
this device driver when `HAKIOS_THYME_INPUT` is enabled. The device driver
accepts the official `touchscreen=1` / `buttons=1` arguments and the legacy
loader's `umi_touch=1` / `umi_keys=1` aliases during migration.

## Touch result

The controller initialized, the boost and `ldoc1` 1.8 V rails came up, and
backboardd attached the controller as the main display digitizer.

```text
UMI boost: VIN1 and direct output-high verified
UMI RPMH: ldoc1 addr=0x42e00 slave=4 aux-version=0x100 priority=0:0
UMI FTS: ST FTS521 3639 initialized scan=0x1
backboardd: [TouchEvents] *** acquired main display digitizer
```

A physical drag and several taps raised the event counter from 7 to 472:

```text
UMI FTS: event 13 ...   # DOWN
backboardd: contact 1 presence: touching
UMI FTS: event 23 ...   # MOVE
UMI FTS: event 33 ...   # UP
backboardd: contact 1 presence: none
UMI FTS: poll=18125 events=472 errors=0 irq=1 active=0
```

The event stream also drove SpringBoard's system gesture handling.

## Keys result

The PM8150 arbiter initialized and both APIDs were found:

```text
UMI keys: arbiter=0x50010000 channels=512 PON-APID=13 GPIO6-APID=25
```

One transient `PON=1 GPIO6=0` read failure recovered. No physical key event was
captured during this run, so the hardware path remains to be verified in the
next dedicated button/power test.

## Stability

The probe ran for 180 seconds and returned to Android through
`loader_return_after`. No SIGSEGV, touch initialization failure, or SpringBoard
restart was observed. Without the GPU driver the UI remained on the generic
framebuffer/software path.
