# Metal on thyme: the Adreno 650, not the driver's Adreno 7c+ Gen 3

The port's Metal driver (`hakios-metal/`) was written for the Adreno 7c+ Gen 3
(chip id `0x06030500`, an A635-family part, "a6xx gen4" to Mesa).  thyme's SoC
is the Snapdragon 870, whose GPU is the **Adreno 650** (chip id `0x06050002`,
"a6xx gen3" to Mesa).  The driver refuses any other chip on purpose, and the
window server says so in the boot log (`metal=1`):

    HakiosMetal[backboardd 18]: no device: the GPU's chip ID is 0x6050002: the
    tables of this driver are the Adreno's of chip ID 0x6030500 (a6xx.hpp),
    another's would hang it

CoreAnimation then draws through OpenGL ES, as it always has; the Maps screens
of that run are the GL path's.  The plug-in also shows its device to the window
server alone unless the boot argument `metal_all=1` is given:
`HakiosMetal[Maps 202]: Maps is not shown the device: the window server alone
is (metal_all=1: every process)`.

## The A650 as Mesa has it

From Mesa 25.0.7's generated `freedreno_devices.h`, the entry the chip id
`0x06050002` resolves to (`GPUId(650)` → `{650, 0x60500ff}` → `__info13`,
`a6xx_base + a6xx_gen3`):

```c
.chip=6, .gmem_align_w=16, .gmem_align_h=4, .tile_align_w=96, .tile_align_h=16,
.tile_max_w=1024, .tile_max_h=1008, .num_vsc_pipes=32, .cs_shared_mem_size=32768,
.num_sp_cores=3, .wave_granularity=2, .fibers_per_sp=4096, .threadsize_base=64,
.max_waves=16, .highest_bank_bit=16, .ubwc_swizzle=6, .macrotile_mode=1, .num_ccu=3,
.a6xx={.magic={.PC_POWER_CNTL=2, .TPL1_DBG_ECO_CNTL=0x01008000, .GRAS_DBG_ECO_CNTL=0,
        .SP_CHICKEN_BITS=0x1400, .UCHE_CLIENT_PF=4, .PC_MODE_CNTL=31,
        .SP_DBG_ECO_CNTL=0x01000000, .RB_DBG_ECO_CNTL=0x04100000,
        .RB_DBG_ECO_CNTL_blit=0x04100000, .HLSQ_DBG_ECO_CNTL=0, .RB_UNKNOWN_8E01=0,
        .VPC_DBG_ECO_CNTL=0x02000000, .UCHE_UNKNOWN_0E12=1,},
       .has_cp_reg_write=True, .has_8bpp_ubwc=False, .has_gmem_fast_clear=True,
       .has_hw_multiview=True, .has_fs_tex_prefetch=True, .has_sampler_minmax=True,
       .supports_double_threadsize=True, .sysmem_per_ccu_depth_cache_size=65536,
       .sysmem_per_ccu_color_cache_size=65536, .gmem_ccu_color_cache_fraction=2,
       .prim_alloc_threshold=7, .vs_max_inputs_count=32, .max_sets=5,
       .line_width_min=1.0, .line_width_max=1.0, .reg_size_vec4=64,
       .instr_cache_size=127, .supports_multiview_mask=True,
       .has_z24uint_s8uint=True, .tess_use_shared=True, .storage_16bit=True,
       .has_tex_filter_cubic=True, .has_separate_chroma_filter=True,
       .has_sample_locations=True, .has_dp2acc=True, .has_lrz_dir_tracking=True,
       .enable_lrz_fast_clear=True, .lrz_track_quirk=True, .has_lrz_feedback=True,
       .has_per_view_viewport=True, .has_scalar_alu=True, .has_early_preamble=True,
       .prede_nop_quirk=True,}
```

## What the driver has today, and the delta

`hakios-metal/include/hakios-metal/a6xx.hpp` carries the 642L's values as
`inline constexpr` constants.  The ones that differ (642L → A650):

| constant | 642L | A650 |
|---|---|---|
| `kChipId` | 0x06030500 | 0x06050002 |
| gen | a6xx gen4 | a6xx gen3 |
| `kNumCcu` / `kNumSpCores` | 2 / 2 | 3 / 3 |
| `kTileAlignW` / `kTileAlignH` | 32 / 16 | 96 / 16 |
| `kHighestBankBit` | 15 | 16 |
| `kGmemCcuColorCacheFraction` | 4 | 2 |
| `kRegSizeVec4` (a6xx.hpp's vector register size) | 96 | 64 |
| `kMaxSets` | 8 | 5 |
| `kHasLpac` | true | false |
| `kHas8bppUbwc` | true | false |
| `kHasEarlyPreamble` | false | true |
| `kMagicPcPowerCntl` | 1 | 2 |
| `kMagicTpl1DbgEcoCntl` | 0x05008000 | 0x01008000 |
| `kMagicUcheClientPf` | 0x84 | 4 |
| `kMagicSpDbgEcoCntl` | 0x06 | 0x01000000 |
| GMEM size | 512 KiB (`kGmemBytes`) | 1 MiB (Qualcomm's Adreno 650; Mesa takes it from the kernel, not this table) |

(`fibers_per_sp` 4096, `cs_shared_mem_size` 32 KiB, `wave_granularity` 2,
`instr_cache_size` 127, `has_gmem_fast_clear` true, the sysmem cache sizes and
most magic registers are the same.)

## Porting steps

1. Turn the chip constants into a small table (two rows: the 642L and the
   A650), selected at `hakios_metal_open()` time by the chip id the kernel
   reports (`gpu->chipId()`), instead of the `inline constexpr`s.  The
   compiler's `id.chip_id` (`src/compiler.cpp`, `src/texture.cpp`) must be the
   actual chip's, so Mesa's ir3 takes the gen3 route; `hakios_metal_chip_known`
   accepts both ids.
2. Audit the paths that branch on the constants above: the GMEM size and the
   per-CCU colour cache fraction (tile grid), the vector register size and
   `max_sets` (constant uploads), LPAC (its commands and register writes must
   not be emitted), 8bpp UBWC (format restrictions), and early preamble
   (gen4 leaves `SP_FS_CTRL_REG0`'s preamble off; the A650 resets it on, so the
   driver must clear it as it does for the 642L, or implement the preamble).
3. The fake GPU and its tests (`fake_gpu.hpp`'s `kChipId`, `kParamChipId`)
   need the A650 row too, so the host tests cover both.
4. Build and run as the 642L was: `make ios MESA=1` in `hakios-metal/`, the
   image with `METAL_PLUGIN=ca`, a `default.metallib` from
   `tools/merge_metallib.py`, and the boot arguments `metal=1 ca_metallib=...
   metal_all=1` (the last for Maps and the other apps; the window server alone
   otherwise).
5. The kernel side is shared: thyme's `hakios.thyme_gpu.cpp` serves the same
   `HakiosGPU` user client the GL path uses, and `MiniIOMobileFramebuffer`
   publishes the plug-in for any `metal=N`.
