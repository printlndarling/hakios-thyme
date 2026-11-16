# Bringing the A650 into hakios-metal: the exact sites to change

`docs/metal-a650.md` records why the driver refuses thyme's GPU (its tables
are the Adreno 7c+ Gen 3's, `0x06030500`) and what the A650's values are
(Mesa's `__info13` row, `0x06050002`, a6xx gen3).  This is the audit of where
those values live in the code and the order to change them, so the port is a
mechanical edit rather than a rewrite.  It was written with the phone off the
bus (charging), from the sources and the driver's own host tests.

## What uses the constants today

`grep -rn 'kChipId\|kNumCcu\|...' hakios-metal` gives 27 sites in six files;
the magic registers add 32 more, almost all in one table.

* `include/hakios-metal/a6xx.hpp`
  * lines ~30-37: `kChipId`, `kHasLpac`, `kNumSpCores` (top of file).
  * lines ~1393-1415: the `__info14` block -- `kMagicPcPowerCntl`,
    `kMagicTpl1DbgEcoCntl`, `kMagicUcheClientPf`, `kMagicSpDbgEcoCntl`,
    `kMagicRbDbgEcoCntl`, `kMagicHlsqDbgEcoCntl`, `kMagicVpcDbgEcoCntl`,
    `kMagicUcheUnknown0E12`, `kHasEarlyPreamble`, `kNumCcu`, `kGmemBytes`,
    `kGmemCcuColorCacheFraction`, `kTileAlignW/H`.
  * lines ~1647: `rbCcuCntlSysmem()` -- a `constexpr` whose default argument
    is `kNumCcu * kSysmemPerCcuDepthCacheSize`.
  * lines ~1650-1675: the static register list (the magic values above go
    into `REG_A6XX_*_DBG_ECO_CNTL` entries).
  * lines ~4090: `kGmemCcuColorOffset` (`constexpr` from `kGmemBytes`,
    `kNumCcu`, `kGmemCcuColorCacheFraction`) and `rbCcuCntlGmem()`, followed
    by `static_assert(rbCcuCntlGmem() == 0x3c400000)` -- the 642L's number.
* `src/render.cpp` ~2213-2231: the tile binning uses `kTileAlignW/H`
  (`kTileMaxW/H` are A650-identical, 1024x1008).
* `src/compiler.cpp:59` and `src/texture.cpp:466`: the compiler's
  `id.chip_id = a6xx::kChipId` -- this is what makes ir3 take the gen4
  route; the A650 must hand it `0x06050002`.
* `src/c_api.cpp:217,239`: `hakios_metal_chip_known()` accepts only
  `a6xx::kChipId`, and the refusal message names it.
* `include/hakios-metal/fake_gpu.hpp:190,193,298` and `src/fake_gpu.cpp:2440-
  2441`: the simulator's `kChipId`, `kGmemBytes`, `kParamChipId`,
  `kParamGmemSize`; `tests/a6xx_test.cpp:287,389-393` pin values.

So the values are not scattered through logic: they enter through about a
dozen named constants, two `constexpr` helpers, one static register list and
the compiler id.  That is the whole surface.

## Design

1. **One row per chip.**  Add `struct a6xx_chip` (chip id, gen, num_ccu,
   num_sp_cores, tile_align_w/h, gmem_bytes, gmem_ccu_color_cache_fraction,
   reg_size_vec4, max_sets, has_lpac, has_8bpp_ubwc, has_early_preamble,
   highest_bank_bit, the eight magic registers) with two `constexpr` rows:
   `kA642L` (today's values) and `kA650` (Mesa's `__info13`).  Put it at the
   top of `a6xx.hpp`, where the values are now.
2. **Select once.**  `hakios_metal_open()` already reads `gpu->chipId()`;
   store the chosen row in the driver's device object and expose it as
   `const a6xx_chip &a6xx::chip()`.  `hakios_metal_chip_known()` accepts
   `0x06030500`, `0x06050002` and 0 (the fake), and the refusal message names
   both.
3. **Retire the constexprs that read the constants.**  `rbCcuCntlSysmem`,
   `rbCcuCntlGmem` and `kGmemCcuColorOffset` take the row (or become functions
   over `chip()`); the `static_assert` becomes a host test over both rows
   (`rbCcuCntlGmem(kA642L) == 0x3c400000` today, and the A650's number
   computed and pinned once verified on the device).
4. **The static register list** is built from the row.  The DBG_ECO values
   differ per chip (the A650's `kMagicTpl1DbgEcoCntl`, `kMagicUcheClientPf`,
   `kMagicSpDbgEcoCntl`, `kMagicPcPowerCntl` all differ); building the list in
   a function that copies the row's values is the smallest change.  The
   preemption preamble that adds `kMagicPcPowerCntl` twice
   (`a6xx.hpp:1543`) reads the same row.
5. **The compiler.**  `src/compiler.cpp` and `src/texture.cpp` must use
   `chip().chip_id`, not the constant.  Ir3 then takes the gen3 route for the
   A650.  Nothing else in the compiler is chip-specific in this tree (the
   `earlyPreamble` flags in the tests are per-compilation options).
6. **The fake GPU and its tests.**  `fake_gpu.hpp` gets the A650 row
   (`kChipId650`, `kGmemBytes650 = 1 MiB`) and a boot-arg/env switch (the
   simulator already answers `kParamChipId`/`kParamGmemSize`); the host tests
   check both rows and that the driver's selection follows `kParamChipId`.
7. **The audit items `docs/metal-a650.md` lists** that the grep does *not*
   find in `src/` -- `kHasLpac`, `kHasEarlyPreamble`, `kHas8bppUbwc`,
   `kMaxSets`, `kRegSizeVec4`, `kHighestBankBit` have definitions but no
   users outside tests.  Either they are consumed under other names in the
   register emitters, or the 642L path never needed them.  Before the A650
   run, re-grep for their register-level spellings (`SP_FS_CTRL_REG0`'s
   preamble bit, `HLSQ`'s bindless vertices, `RB_BLIT`'s UBWC formats,
   `SP_*`'s constant upload size), and if they are truly unused, say so here
   rather than inventing a use.

## Order of work

1. The `a6xx_chip` table and `chip()` selection; chip-known accepts both;
   host tests build and pass for both rows (no device).
2. The register list and the two CCU helpers per row; the static_assert
   becomes two tests.
3. The compiler id from the selected row; a host test compiles one fixture
   twice (642L, A650) and checks the id reaches ir3.
4. The fake GPU's A650 mode; the existing tests run against it.
5. Device run: `METAL_PLUGIN=ca` image, `default.metallib` from
   `tools/merge_metallib.py`, boot args `metal=1 ca_metallib=<file>
   metal_all=1`.  Acceptance: the log no longer says "no device: the GPU's
   chip ID is 0x6050002"; then the first screenshots of Maps and a
   `HakiosGPU` submission count above zero through the Metal path, which is
   the line between "compiled" and "worked" this project keeps.

## Risks

* Gen3 vs gen4 register defaults: the A650's preamble is reset on (the driver
  must clear `SP_FS_CTRL_REG0`'s preamble as it does for the 642L) -- check
  the register emitter before trusting `kHasEarlyPreamble = false`.
* GMEM is 1 MiB on the A650 (512 KiB in the driver's table); the binning
  room calculation must use the row's value or the last tile row overruns.
* `reg_size_vec4 = 64` and `max_sets = 5` change constant uploads; if the
  driver hard-codes 96 or 8 anywhere under another name, the shader upload
  will go wrong only on hardware, so grep the register emitters for the raw
  numbers as well as the names.
* The A650 is a6xx gen3: ir3 will emit different instructions; a shader that
  compiled under gen4 rules may need the gen3 workarounds Mesa carries
  (`lrz_track_quirk`, `tess_use_shared`, `storage_16bit`), which this
  driver's compiler takes from `id.chip_id` only if it maps it the way Mesa
  does -- verify against Mesa 25.0.7's `ir3` chip switch.
