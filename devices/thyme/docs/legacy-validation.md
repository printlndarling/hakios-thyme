# Legacy THYME validation

These results were recorded on the Xiaomi Mi 10S before migration to official
main. They define expected hardware behaviour for the new port; they are not
results from the official-main kernel.

## Stable UI

- A stable payload with `loader_return_after=0`, no `-v`, and no `fb_scan=1`
  ran for more than 30 minutes.
- No loader return, `hakios: signal 11`, backboardd SIGSEGV, or softpipe
  fallback occurred.
- FTS polling remained `errors=0`.
- Physical touch increased the event count from 7 to 239 and produced complete
  DOWN/MOVE/UP records.
- A tap opened Preferences.
- SpringBoard's system-gesture recognizer consumed a swipe.

## Keys and power

- Volume up/down produced the expected HID usages `0xE9` and `0xEA`.
- The power key produced HID usage `0x30`.
- Short power press changed backboardd to `displayState: off`.
- A second press returned `displayState: on` without restarting SpringBoard.
- Intermittent `PON=0` SPMI reads were observed; they recovered, but remain a
  stability issue.

## Charging

The loader accepted exactly two charger actions:

```text
battery: TakeAction(7) accepted
battery: TakeAction(3) accepted
battery-now: flags=1f SOC=84% V=3766 mV I=1127 mA T=30 C action=3 polls=7 failures=0 take_action=1 live=0 calls=2 failures=0
```

The signed current stayed positive during a long session, for example `+95`,
`+471`, `+112`, and `+89 mA`. The charge guard reported the real battery state:

```text
battguard: charge 82% (battery present, charger attached, charging yes); stop at or below 25%
```

The battery percentage nevertheless drifted from 84% to 80% under a long
screen-on load while connected to a 500 mA PC USB port. Positive battery current
alone is therefore not enough for unattended use on that host port; the external
charge guard remains mandatory until a higher-power and thermal test passes.
