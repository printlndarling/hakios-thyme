# SLPI sensors: the FastRPC/registry layer

Status: 2026-11-05. This note records why the SLPI sensor process stalls and
what the next implementation has to provide. It is a working design, not a
claim that any of it is done.

## What is already verified

- The SLPI loads its signed firmware, exits reset and opens GLINK `IPCRTR`
  (see `devices/thyme/CHECKLIST.md`, `logs/131207-run.log`).
- QRTR shows node 9 and the SNS client (service 400). The SNS client QMI path
  works with the SSC array TLV; SUID lookups for `accel` and `registry` are
  accepted with QMI result 0 (`logs/140238-run.log`).
- No SUID indication arrives and the sensor process gives up after about
  164 s with the user-PD watchdog
  `dog_virtual_user.c` / "stalled initialization".
- The Android sensor HAL (`sensors.ssc.so`, pulled read-only) shows the
  registry is a direct-channel sensor whose discovery waits on FastRPC:
  `direct_channel::init_fastRPC_symbols`, `sensor_factory::is_registry_up`,
  `libsns_fastRPC_util.so`.
- The SLPI opens the GLINK channel `fastrpcglink-apps-dsp`. It is now opened
  both ways from our side too (ours 4, the DSP's 2) and its intents are
  offered; the DSP sends no FastRPC message until the AP registers a listener
  (`logs/152916-run.log`).
- The apps SMMU is on with 87 match registers and 74 contexts; unmatched
  streams **fault**, and no match register covers the SLPI stream `0x541`
  (`logs/160408-run.log`). The AP has to add an SMR/S2CR for it: bypass for a
  first bring-up, or a translated context built with the Wi-Fi page-table
  code.
- The bypass is programmed and verified: match register 6 gets
  `smr 0x80000541`, `s2cr 0x20000` (bypass), old `0/0` saved and restored when
  the DSP stops (`logs/163238-run.log`). A physically contiguous argument
  buffer helper (`hakios.thyme_slpi_dma.cpp`) is in the driver; the next step
  is the first `remotectl_open` call on the FastRPC channel.

## The AP-side FastRPC contract

The open reference implementation is the `fastrpc/` tree of the sensh project
(`hexagonrpcd`, `libhexagonrpc`). It uses Linux's `/dev/fastrpc-adsp` ioctl
interface; the actual wire protocol is in Linux `drivers/misc/fastrpc.c`.

The pieces that matter here:

1. **Transport**: the `fastrpcglink-apps-dsp` GLINK channel (dt:
   `qcom,msm_fastrpc_rpmsg`, intents 0x64 x 0x40). The AP sends a
   `struct fastrpc_msg` header; the arguments live in a buffer whose DMA
   address and size are in that header.
2. **Message**: `client_id`, `tid`, `ctx`, `handle`, `sc`, `addr` (DMA
   address), `size`. The DSP answers on the same channel with the same header
   shape; the response carries the return value in the args buffer.
3. **Memory**: a remote heap and per-call argument buffers, mapped through the
   apps SMMU. The DTB has `qcom,msm_fastrpc` with the `adsprpc-smd` context
   banks (`iommus = <0x47 0x1803 0x00>` and siblings) and an address pool of
   0x80000000 + 0x78000000; the vendor driver's remote heap comes from a
   memory region outside this property. Finding/validating that region is one
   of the first tasks.
4. **Listener**: the AP opens `remotectl`/`adsp_default_listener` handles on
   the DSP and registers a reverse listener
   (`adsp_default_listener_register`). The DSP then calls the AP's
   `apps_std` methods for file access. `hexagonrpcd` serves:
   - `/etc/qcom/sns_reg.conf` <- `/vendor/etc/sensors/sns_reg_config`
   - `/etc/qcom/sensors.d` <- `/vendor/etc/sensors/config`
   - `/var/lib/qcom/sensors` <- `/mnt/vendor/persist/sensors/registry`
   - `/usr/lib/qcom/adsp` <- `/vendor/dsp/adsp`
   - `/usr/lib/qcom/adsp/avs` <- `/vendor/etc/acdbdata`
5. The sensor registry JSON on the phone is at
   `/mnt/vendor/persist/sensors/registry/registry/sns_reg_config`; the
   per-sensor data are the files beside it, and the config JSONs are under
   `/vendor/etc/sensors/config/`.

## The first two calls, exactly

The reference is `libhexagonrpc`'s `vfastrpc2` and Linux's `fastrpc_get_args`.
For both calls `fastrpc_msg.client_id` is 0 (a kernel call); `tid` is ours;
`ctx` is `ctxid | pd` (`pd = 2` for the sensors PD, so `0x102` for
`ctxid = 1 << 8`); `addr` is the buffer's physical address; `size` is the
payload rounded to a page.

1. **Sensors PD attach** (`FASTRPC_IOCTL_INIT_ATTACH_SNS`): handle
   `FASTRPC_INIT_HANDLE = 1`, `sc = 0x10000` (one input buffer). The single
   input buffer is a `uint32_t client_id` (nonzero; `1`). The DSP maps that
   client to the sensors PD.
2. **`remotectl_open`**: handle `REMOTECTL_HANDLE = 0`,
   `sc = 0x20200` (2 in buffers, 2 out buffers), from the definition
   `in_nums=0, in_bufs=1, out_nums=2, out_bufs=1`. The four buffers, in order:
   - scalar input buffer, 8 bytes: `[0]` = name length, `[1]` = error buffer
     size (256);
   - name input buffer, `strlen("adsp_default_listener") + 1`;
   - output scalar buffer, 8 bytes: the DSP returns the handle and `dlret`;
   - error output buffer, 256 bytes.
   The buffer's metadata is `nscalars = 4`; each scalar has a
   `fastrpc_remote_buf` (16 bytes), `fastrpc_invoke_buf` (8) and
   `fastrpc_phy_page` (16), then `fdlist` (8 x 16) and `crc` (4 x 64) and a
   u32; the whole metadata is aligned to 128, then the four buffers are laid
   out inline, each aligned to 128. The `rpra[i].buf.pv` is the kernel virtual
   address, `pages[i].addr` the physical address of the same page, and
   `list[i].num = 1`, `pgidx = i`. The DSP writes the two output words into
   the output scalar buffer.

The next call after a successful open is
`adsp_default_listener_register` (handle from the open, `sc = 0x100000`:
`in_nums=0, in_bufs=0, out_nums=0, out_bufs=0`), then the reverse `apps_std`
calls arrive from the DSP.

## Implementation plan

1. **Channel and message probe** — add `fastrpcglink-apps-dsp` to the SLPI
   GLINK channel table, open it both ways, and log every FastRPC header and
   payload. This proves the channel and shows what the DSP sends before the
   AP registers a listener.
2. **AP buffer + SMMU** — allocate a small contiguous argument buffer, map it
   in the `adsprpc-smd` context bank, and place its IOVA in `fastrpc_msg.addr`.
   Reuse the apps-SMMU code from the Wi-Fi path where possible; the FastRPC
   context bank is a different SID and needs its own mapping.
3. **Remote control** — send `remote_handle_open("remotectl")`, then
   `adsp_default_listener_register`, and keep the listener context. The AP
   side of the listener is the `apps_std` method table.
4. **File service** — implement the small subset of `apps_std` the sensor
   registry uses (`open`, `read`, `close`, `stat`, maybe `opendir/readdir` for
   `sensors.d`) over the reverse calls, serving the phone's registry files
   from `fw-sensors/` or the root image.
5. **Registry** — once the sensor process reads the registry, repeat the
   `accel`/`registry` SUID lookup, configure the accelerometer, and expose the
   samples to `backboardd` through a HID sensor service.

## Risks and open questions

- The remote heap region is not named in the decompiled `qcom,msm_fastrpc`
  node; the vendor driver may allocate it from the SLPI/ADSP reserved memory.
  This has to be resolved before the DSP will trust AP buffers.
- The SMMU context programming is security-sensitive; a wrong mapping can
  stall or crash the SLPI or the AP.
- The `apps_std` wire method IDs and argument layouts come from the DSP-side
  `libapps_std_skel.so`; they may differ from the ADSP reference on this
  firmware. The open `hexagonrpcd` definitions are the starting point.
- The sensor process is a user PD with a watchdog; a partial listener that
  hangs a call may reset it. Every handler must answer or fail promptly.

## Evidence

| File | What |
|---|---|
| `logs/131207-run.log` | SLPI firmware, TrustZone, GLINK, QRTR services |
| `logs/140238-run.log` | SNS client QMI result 0, silent SUID lookups, watchdog |
| `logs/141201-run.log` | Legacy AP SNS_REG service probe: no group requests |
| `logs/152916-run.log` | `fastrpcglink-apps-dsp` open both ways; no DSP FastRPC message before listener registration |
| `devices/thyme/drivers/sensor/hakios.thyme_slpi.c` | the loader, GLINK/QRTR and SNS client |
| `devices/thyme/CHECKLIST.md` | the hardware checklist and current blockers |
