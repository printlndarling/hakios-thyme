# SLPI sensors: the FastRPC/registry layer

Status: 2026-11-05. The reverse tunnel now works end to end as far as the DSP:
the AP sends calls, the DSP answers, the listener is registered and served,
and the DSP's own `apps_std` file requests are waited for. What is not yet
verified is the DSP's sensor process asking for a file; that is what the
`next2` loop below is watching for.

## What the AP sends now, and what the DSP answers

The argument packet is a 4 KB item of **shared memory** (item 621, in the
partition for processor 0 and the sensor DSP), not an ordinary page: the
DSP's FastRPC stream cannot read the kernel's pages through the apps SMMU,
and a packet there wedges the phone at step 10. SMEM it reads anyway (GLINK
lives in it). `hakios.thyme_slpi.c` lays the packet out exactly as Linux's
`fastrpc_get_args` does and copies it in from a staging buffer.

Verified on the phone (`logs/185355-run.log`, `logs/190234-run.log`):

| Call | Client | Handle | `sc` | Answer |
|---|---|---|---|---|
| `INIT_ATTACH_SNS` | 0 (kernel) | 1 | `0x10000` | ctx `0x102`, retval 0 |
| `remotectl_open("adsp_default_listener")` | 1 | 0 | `0x20200` | handle 837051488, dlret 0 |
| `adsp_default_listener_register` | 1 | that handle | `0x100000` | retval 0 |
| `adsp_listener_init2` | 1 | 3 | `0x03000000` | retval 0 |
| `adsp_listener_next2` | 1 | 3 | `0x040202` | blocks, or "nothing" (`0x80000434`) |

There are two listener interfaces in the firmware, and Qualcomm's
`listener_android.c` uses the modern one first:

| Interface | Opened by | `init2` | `next2` |
|---|---|---|---|
| `adsp_listener` (static) | the fixed handle 3 | method 3, `0x03000000` | method 4, `0x040202` |
| `adsp_listener1` (modern) | `remotectl_open("adsp_listener1")` | method 5, `0x05000000` | method 6, `0x060202` |

The argument layout of `next2` is the same in both (`prevCtx`, `prevResult`,
`prevbufs`/`prevbufsLen` in; `ctx`, `handle`, `sc`, `bufsLenReq`, `bufs` out),
and `get_in_bufs2` (method 5 static, 7 modern) fetches input buffers larger
than the one passed to `next2`. The boot argument `fastrpc_listener1=1`
selects the modern interface.

The session `client_id` is 1 for every call after the attach; with 0 the DSP
accepts the call but never delivers the listener's requests. The context is
`ctxid << 8 | pd`; `ctxid` numbers the call and `pd` selects the processor
domain, so the next2 loop must not vary the low byte (an earlier version did,
and rotated its calls through PD 0, 1 and 2). The boot argument `fastrpc_pd`
sets the PD for the whole listener chain (2, the sensors PD: `remotectl_open`
is rejected with `0x80000434` in PD 0, and the fixed listener handle only
answers in PD 2).

The bring-up's boot arguments:

| Argument | Meaning |
|---|---|
| `fastrpc_call=N` | run the chain; 9 starts the listener thread, 11 adds the name probe |
| `fastrpc_pd=2` | the processor domain for the listener's calls (2, the sensors PD) |
| `fastrpc_lhandle=1` | use the handle `remotectl_open` returned for `init2`/`next2` (default: the fixed 3) |
| `fastrpc_attach_late=1` | open and register before the sensors PD's first attach |
| `fastrpc_restart=1` | release the sensors PD after registering, attach it again, then start `next2` |
| `fastrpc_restart=2` | release the PD right after the first attach, register, and attach once more |
| `fastrpc_delay=N` | wait N seconds between the first attach and the listener (let the first registry read fail and the process be restarted) |

The handle `remotectl_open` returns is not the listener's own handle:
`adsp_listener_init2` on it answers -1, on the fixed 3 it answers 0
(`fastrpc_lhandle=1` tests the other; the default uses 3). The sensor process
reads its registry once, when its PD starts, so two orderings are available
as boot arguments: `fastrpc_attach_late=1` registers the listener and starts
`next2` before the first attach, and `fastrpc_restart=1` releases the sensors
PD (`INIT_RELEASE`, method 1 on handle 1) and attaches it again, which starts
the process with the listener already waiting.

`next2` must have exactly one call outstanding: when it is waiting for a
request the DSP holds the GLINK intent, and a short timeout sent call after
call until the channel had no intents left ("the channel did not take the
call").

## The file service behind the tunnel

`apps_std` methods 1, 2, 3, 4, 9, 19, 26, 27, 28 and 31 are decoded and
served in the kernel from `fw/sensors/` in the root image. The input and
output blobs are `[u32 size][padding][data]` sequences with 8-byte alignment,
as sensh's `iobuffer.c` encodes them. The paths the DSP asks for map to:

| DSP | root image |
|---|---|
| `/vendor/etc/sensors/sns_reg_config` | `/fw/sensors/sns_reg_config` |
| `/vendor/etc/sensors/config/...` | `/fw/sensors/config/...` |
| `/sys/devices/soc0/...` | `/fw/sensors/soc0/...` |
| `/mnt/vendor/persist/sensors/...` | `/fw/sensors/persist/...` |
| `/usr/lib/qcom/adsp/...` | `/fw/sensors/adsp/...` |

`fw-sensors/` comes from the phone's Android: `sns_reg_config` (553 bytes),
the 49 `config/*.json` files, and the six `/sys/devices/soc0` values
(`THYME`, subtype `Unknown`, soc_id 356, revision 2.1) that the registry
parser reads to choose the platform files. `userland/build.sh` copies them
into the scene image; the files are vendor data and stay out of git like the
other `fw-*` trees.

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
`fastrpc_msg.client_id` is 0 for the kernel-level attach and 1 (the session)
for every user invoke after it; `tid` is ours; `ctx` is `ctxid << 8 | pd`
(the listener chain's `pd` is the `fastrpc_pd` boot argument, default 0);
`addr` is the shared-memory packet's physical address; `size` is the payload
rounded to a page.

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

## Where it stands

1. **Channel and message probe** — done; the channel opens both ways and the
   DSP answers the AP's calls.
2. **AP buffer + SMMU** — done with shared memory instead of an SMMU mapping:
   an ordinary page cannot be read by the DSP and wedges the phone, an SMEM
   item works. The SMMU bypass (SMR 6, stream 0x541) stays harmless for now;
   a translated context would let ordinary pages be used later.
3. **Remote control** — done: open, register and the listener's init2 all
   answer 0; `next2` is polled with one call outstanding.
4. **File service** — implemented in `hakios.thyme_slpi.c`; waiting for the
   DSP's first nested call to exercise it. If the registry parser asks to
   write its merged output (`/mnt/vendor/persist/sensors/registry/registry`),
   `apps_std` method 5 (`fwrite`) and the write mode of `fopen` still have to
   be added.
5. **Registry** — once the sensor process reads the registry, repeat the
   `accel`/`registry` SUID lookup, configure the accelerometer, and expose the
   samples to `backboardd` through a HID sensor service.

## Risks and open questions

- The DSP's sensor process has not started a nested call in 150 s of a boot
  with the listener registered and `next2` outstanding; it neither watchdogged
  (which it did without a listener) nor asked. The listener may be registered
  in the wrong PD for the DSP's file daemon — `fastrpc_pd` now selects it —
  or the process may be blocked in a request made before registration.
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
