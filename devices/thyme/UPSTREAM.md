# Upstream policy

## Remotes

| Remote | Repository | Role |
|---|---|---|
| `origin` | `printlndarling/hakios-thyme` | this integrated fork; all THYME work is pushed here |
| `upstream` | `oosexgang/hakios-xnu-kernel` | official common development |

The official revision imported into this fork is:

```text
a48d2b29dd58da5ef322052a7f4f73d7eb8eebc4
SpringBoard: boot-args ax_rt and ax_rm, Settings' Reduce Transparency and Reduce Motion as it starts
```

## Updating from official

```sh
git fetch origin
git fetch upstream
git switch main
git pull --ff-only origin main
git merge upstream/main
```

Resolve conflicts in favour of the official common code unless the change is a
documented THYME hardware fix. Device files live under `devices/thyme/`; common
driver changes should remain minimal and isolated in commits so they can be
reported or upstreamed.

Before pushing a merge:

```sh
tools/hakios-device.py check thyme
make thyme_defconfig
make config
make kernel -j8
```

Do not merge a new official revision without a successful THYME RAM boot.

## Inspected candidate

On 2026-11-02, `upstream/main` advanced to
`96ffbc5c8f8d0f6814dd5d55948e8b6dfacf7208`
(`HakiosMetalCA: pipelines CoreAnimation makes at run time get their variants`).
The candidate contains 12 commits after `a48d2b2`, all centered on the optional
CoreAnimation-through-Metal path:

- `HakiosMetalCA.m` and its CoreAnimation/Metal window-server integration;
- Mesa AIR/IR3 compiler changes and two additional Freedreno patches;
- a Metal probe job and `METAL_PLUGIN=ca` wiring in `userland/build.sh`;
- SpringBoard `anim_max`, animation profiling and Metal-cache diagnostics;
- generated boot-argument catalogue updates.

They contain no THYME device-driver changes, but they add roughly 18,000 lines
and touch `userland/src/inject_sb.c`, where this fork now also carries the
battery-status aggregator fix. A dry merge reports conflicts only in
`configs/symbols.tsv` and `docs/boot-args.md`; the code merges automatically.

The candidate is not imported yet. The fork policy requires a successful
THYME RAM boot before accepting a new official revision, and more specifically
the validation battery now needs:

1. the board-gating and charging-status fixes to pass on hardware;
2. both the validated OpenGL ES path and `METAL_PLUGIN=ca` to boot;
3. a sustained SpringBoard interaction test with no regression in touch,
   charging, Wi-Fi, Bluetooth, audio or thermal behavior;
4. conflict resolution that retains the THYME battery hooks and generated
   argument catalogue rather than taking either side wholesale.

Do not carry this optional rendering stack into a hardware-diagnostic payload
before the current THYME regression is closed.
