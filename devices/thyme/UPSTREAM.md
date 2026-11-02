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

On 2026-11-02, `upstream/main` advanced from `a48d2b2` to `acc0e88`. The three
new commits change the Mesa AIR/IR3 tooling, `userland/build.sh`, and add the
Metal probe job. They contain no THYME device driver changes.

They are not imported yet: the fork policy requires a successful THYME RAM
boot before accepting a new official revision, and the current Mesa compositor
already provides the validated GPU path. Re-evaluate this candidate with a
Metal-focused RAM test rather than carrying it into a stability or device
diagnostic payload.
