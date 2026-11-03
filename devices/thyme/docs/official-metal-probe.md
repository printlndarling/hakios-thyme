# THYME Metal driver probe

Date: 2026-11-04

The experimental `hakios-metal` core was cross-built and run on THYME as the
GPU's only client. It is not the current UI renderer; the default UI continues
to use the validated Mesa/FD650 OpenGL ES path.

## Build

- `hakios-metal` source: `oosexgang/hakios-metal` at `9d52b51`.
- Clang compatibility patch:
  `devices/thyme/patches/hakios-metal-constexpr-clang.patch`.
  It replaces `__builtin_signbit` in the constant-expression `fui()` with
  Clang's `__builtin_bit_cast`, preserving the existing finite/NaN handling.
- Probe bisect patch:
  `devices/thyme/patches/hakios-metal-probe-bisect.patch`. It runs the render
  option matrix before the normal render tests, so one cold GPU can at least
  identify whether the first submission completes.
- `libhakios-metal.a` and `hakios-metal-probe` were built with `MESA=1` and
  `HAKIOS=/home/darling/hakios-thyme`.
- Probe image:
  `out/thyme-boot/root-metalprobe.apfs`, SHA-256
  `34412634cda92723ae0b0b63697a0475fc1402a9c3b66e6d991e48424675e8bb`.
- Run arguments included `gpu=1 gpu_recover=1`.

## Hardware result

PASS in `logs/031626-run.log`:

- `hakios_metal_open()` returned GPU 0, chip id `0x206050002`, 587 MHz;
- a 500,000-byte `CP_MEMCPY` was exact and left surrounding memory untouched;
- fills through 999,996 bytes passed;
- 40 command buffers completed in order;
- a 16 MB copy reached 732 MB/s from commit to fence;
- AIR compute kernels and on-device AIR compilation passed, including
  `fill_id_air`, chain/loop kernels, clz, bytes, uniform, pack, ballot,
  argument-buffer and several texture compute paths.

FAIL in the same run:

- a 4096-word `fill_id_air` dispatch did not write its target;
- private-memory dispatch was incomplete;
- the first render submission did not finish within five seconds;
- after that first render timeout the GPU stopped answering later submissions,
  so all render, depth, MSAA and indexed-draw checks that follow fail with the
  same timeout. `gpu_recover=1` allowed the run to continue instead of hanging
  the kernel.

The render timeout is the current blocker for the Metal path. It is not a
CoreAnimation or plugin-only failure: the probe exercises the command stream
directly through `hakios-metal`.

## Cold-GPU bisect

The probe was then rebuilt into a minimal launchd image without SpringBoard, so
`hakios-metal` was the GPU's only client. It ran
`hakios-metal-probe --bisect --recover` with `gpu=1 gpu_recover=1`.

The whole default render stream timed out after painting only 6 of 2016
interior pixels. All 30 one-group-removed variants then failed immediately
because the GPU never answered again. `FAULTS` remained 0.

`gpu_recover=1` is currently not implemented by the THYME GPU driver, so it
does not reset the core after the first timeout. A valid bisect therefore needs
one cold boot per render option, or a working GPU reset path. This is the next
requirement for the Metal bring-up.

Evidence: `logs/033837-run.log`, SHA-256
`51692c55af66800272effbcc682214d421c5797a75594ce76ee2c0b8834a34b1`.

## Consequence

- Do not enable `METAL_PLUGIN=ca` or the window server on Metal yet.
- Keep `gpu=1` with Mesa/FD650 as the working default.
- The next task is to bisect the first render command stream on hardware,
  starting with the render sysmem-pass register sequence and its fences.

Evidence: `logs/031626-run.log`, SHA-256
`e59e24f648d1ab56c18786f92819f405f4968da17cc66bdb8fa27ed1933eb83b`.
