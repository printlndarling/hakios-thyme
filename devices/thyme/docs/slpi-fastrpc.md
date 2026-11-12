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
| `fastrpc_attach0=1` | the guest-OS attach (PD 0) before the listener, as `attachguestos` does |
| `fastrpc_static_open=1` | open the `adsp_listener` module for `init2`/`next2` (after the register) |
| `fastrpc_open_wait=N` | how long that module open waits, in ms (default 20000) |
| `fastrpc_open_async=1` | send that open without waiting, so the listener can serve its file request |
| `fastrpc_open_after=1` | send it after the listener is waiting |
| `fastrpc_open_pd=N` | the PD of that open (unset: `fastrpc_pd`) |
| `fastrpc_attach_mid=1` | guest attach, register, sensors attach, then the listener thread (the daemon order) |
| `fastrpc_phone=1` | the phone daemon's order: sensors attach (`fastrpc_pd=0`: the guest-OS attach of the plain `sscrpcd`), `init2`, `next2` waiting on a thread id of its own, then open and register the default listener |
| `fastrpc_close=1` | close the default listener's control handle after registering (mainline does; the phone's daemon does not) |

The handle `remotectl_open` returns is not the listener's own handle:
`adsp_listener_init2` on it answers -1, on the fixed 3 it answers 0
(`fastrpc_lhandle=1` tests the other; the default uses 3). Qualcomm's own
`adsp_listener_stub.c` (Android 11 and current) shows why the fixed 3 is
not the real listener: the static interface's handle is **opened by name**
with `remote_handle_open("adsp_listener")`, i.e. `remotectl_open` of that
name, and its methods are `init2` = 3 and `next2` = 4. The
`fastrpc_static_open=1` boot argument opens it, after the default
listener's register (the order the daemon uses); its module load can take
longer than the two seconds the other opens need.

The guest-OS attach matters: `adsp_listener_init2` in PD 0 answers 0 only
after `INIT_ATTACH` (PD 0) has been sent; without an attach the same call
answers `0x80000434` (tested in `fastrpc_bare` mode). The listener PD's
readiness varies between boots: `init2` sometimes answers within two
seconds and sometimes not, so the register and init2 waits are now 5 s and
10 s. An outstanding `next2` then holds the DSP's FastRPC thread: an
`INIT_ATTACH_SNS` (PD 2) sent afterwards is not answered, even with another
client id and thread id (`fastrpc_attach_client`, `fastrpc_attach_tid`),
which is the order the phone itself uses with two separate daemons and
processes.

The listener's input buffer matters too: the daemon reserves 1 MB for the
`next2` inputs, while a 256-byte buffer gets `0x80000434` or silence; the
kernel now passes 2 KB (the output encoding is capped at 1 KB so the 4 KB
SMEM packet still holds both). A 2 KB run still shows no nested call.

The middle order is the first one where the sensor process does **not**
watchdog: guest-OS attach, register the default listener in PD 0, attach the
sensors PD, then init2 and `next2`. The sensor process's first file request
is therefore accepted by the registered listener; what is missing is the
fetch: `adsp_listener_next2` on the fixed handle 3 never returns it, so the
request stays in the listener PD. That is the same module the 60 s open
cannot load, and the firmware's `pages[0].phy_addr != 0` assertion next to
`fastrpc_kprocess`/`adsp_default_listener` points at the remote heap the
kernel has never provided.

The SM8250 SLPI libraries that do live on the AP side are in the
linux-msm/hexagon-dsp-binaries tree (`sm8250/Thundercomm/RB5/...`): the
user-PD shell `fastrpc_shell_2`, `libsns_direct_channel_skel.so` and the
sysmon skels. There is **no** listener skel among them, so `adsp_listener`
is internal to the DSP firmware and its open must not need an AP file; the
reason it hangs is still open (the FastRPC remote heap and the DSP's
session handling are the candidates). They are copied into
`fw-sensors/adsp/` so the file service can serve them if the sensor
process asks. The sensor process
reads its registry once, when its PD starts, so two orderings are available
as boot arguments: `fastrpc_attach_late=1` registers the listener and starts
`next2` before the first attach, and `fastrpc_restart=1` releases the sensors
PD (`INIT_RELEASE`, method 1 on handle 1) and attaches it again, which starts
the process with the listener already waiting.

`next2` must have exactly one call outstanding: when it is waiting for a
request the DSP holds the GLINK intent, and a short timeout sent call after
call until the channel had no intents left ("the channel did not take the
call").

## What the phone's own daemon does

The phone's listener is `sscrpcd`, started twice by init:

| init service | argv | domain |
|---|---|---|
| `vendor.sensors` (`/vendor/etc/init/vendor.sensors.sscrpcd.rc`) | `sscrpcd sensorspd` | the sensors PD (2) |
| `vendor.lowi` (`/init.target.rc`) | `sscrpcd` | the root PD (0) |

It dlopens `libssc_default_listener.so` (or `libadsp_default_listener.so` when `/sys/kernel/slpi`
does not exist) and calls `adsp_default_listener_start(argc, argv)`. That wrapper builds a URI
from `argv[1]`, and `remote_handle_open_domain()` in the FastRPC userspace
(`fastrpc_apps_user.c`) reads it:

| URI token | Meaning |
|---|---|
| `createstaticpd:sensorspd` | select the sensors static PD (2) as this session's attach mode |
| `attachguestos` | select the root PD (0) |
| `geteventfd` | return the listener thread's exit eventfd |

`createstaticpd:<name>` only *sets the attach mode* (`dsppd`); it creates nothing. `sensorspd`
maps to `FASTRPC_INIT_ATTACH_SENSORS`, which is the `FASTRPC_IOCTL_INIT_ATTACH_SNS` ioctl. The
`sensorspd` name is an AP-side domain name only — the SLPI firmware contains no such string.
`INIT_CREATE_STATIC` is used only for the audio (3 MB remote heap) and OIS static PDs, so loading
`adsp_listener` that way was the wrong mechanism.

The wrapper then, in order:

1. `remote_handle_open("adsp_default_listener")` — the first open of the domain attaches the
   sensors PD *and starts the listener*: `listener_android_domain_init()` calls
   `adsp_listener_init2()` on the fixed handle 3 (`sc 0x03000000`) and starts the `next2` thread;
2. `remote_handle_invoke(default_listener_handle, 0, NULL)` — register;
3. `remote_handle_open("...geteventfd")` — take the listener thread's eventfd and poll it.

The control handle is never closed, and the listener thread uses thread ids of its own while the
daemon's main thread opens and registers — the two must not share one DSP thread id. The
`next2` loop starts with a 0-byte input buffer and grows it from `inBufsLenReq`.

The phone's kernel is a custom 4.19 with the downstream adsprpc UAPI (`/dev/sdsprpc-smd`). Its
`fastrpc_rpmsg_send`/`_response` tracepoints exist under `/sys/kernel/tracing/events/fastrpc` and
would show `cid`, `ctx`, `handle` and `sc`, but tracefs is not writable from the shell domain, so
they cannot be captured without turning SELinux permissive.

## The file service behind the tunnel

`apps_std` methods 1, 2, 3, 4, 9, 19, 26, 27, 28 and 31 are decoded and
served in the kernel from `fw/sensors/` in the root image. The DSP's
`libapps_std_skel.so` numbers its whole interface, decoded from the SLIM
string table in the skel's generated source; the ones not served yet are the
next candidates when a request names them:

| # | method | # | method | # | method |
|---|---|---|---|---|---|
| 0 | fopen | 13 | ferror | 26 | opendir |
| 1 | freopen | 14 | clearerr | 27 | closedir |
| 2 | fflush | 15 | print_string | 28 | readdir |
| 3 | fclose | 16 | getenv | 29 | mkdir |
| 4 | fread | 17 | (unused) | 30 | rmdir |
| 5 | fwrite | 18 | unsetenv | 31 | stat |
| 6 | fgetpos | 19 | fopen_with_env | 32 | ftrunc |
| 7 | fsetpos | 20 | fgets | 33 | frename |
| 8 | ftell | 21 | get_search_paths_with_env | 34 | fopen_fd |
| 9 | fseek | 22 | fileExists | 35 | fclose_fd |
| 10 | flen | 23 | fsync | 36 | fopen_with_env_fd |
| 11 | rewind | 24 | fremove | | |
| 12 | feof | 25 | fdopen_decrypt | | | The input and
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
   DSP's first nested call to exercise it. The `adsp_listener` module open
   hangs (20 s and 60 s waits), which is consistent with the DSP's mod_table
   asking the AP for the module through apps_std while no listener is
   serving yet; `fastrpc_open_async` sends that open and then starts the
   listener, so the request can be served while the open is in flight. The
   library has to be in `fw/sensors/adsp/` (the phone's
   `/vendor/dsp/adsp/libadsp_listener_skel.so` and its siblings).
   If the registry parser asks to write its merged output
   (`/mnt/vendor/persist/sensors/registry/registry`), `apps_std` method 5
   (`fwrite`) and the write mode of `fopen` still have to be added.
5. **Registry** — once the sensor process reads the registry, repeat the
   `accel`/`registry` SUID lookup, configure the accelerometer, and expose the
   samples to `backboardd` through a HID sensor service.

## The next probe: the remote heap

The DSP's fastrpc kernel itself asserts `pages[0].phy_addr != 0` for a
static PD (the string sits next to `fastrpc_kprocess` and
`adsp_default_listener` in the firmware), which is exactly what
`fastrpc_init_create_static` passes: a physically contiguous remote heap,
assigned to the DSP's VMIDs (`qcom,adsp-remoteheap-vmid = <0x16 0x25>`,
i.e. 22 and 37) with an SCM `assign_mem` call before the DSP may use it.
The kernel already makes the SCM calls for PAS and can allocate such a
buffer, so the next implementation is:

1. allocate an 8 MB physically contiguous heap below 4 GB;
2. assign it to VMIDs 22 and 37 with SCM (the descriptor goes through a
   shared buffer, as `qcom_scm_assign_mem` does in mainline);
3. send `FASTRPC_RMID_INIT_CREATE_STATIC` (method 8, `sc = 0x08030000`,
   handle 1) with `{client_id, namelen, pageslen = 1}`, the module name
   (`adsp_listener`), and `{heap address, heap size}`;
4. if the DSP accepts it, init2/next2 on the handle it makes available.

The 60 s hang of the module open and the assertion are consistent: the
module PD cannot be created without a heap.

The first implementation shows that the kernel's physically-contiguous
allocator cannot produce a large buffer at the point step 10 runs: 8 MB
down to 32 KB all fail, and only **16 KB** succeeds (the 4 KB call packet
works because a page-sized request takes the ordinary page path, not
`kmem_alloc_contig`).

The SCM memory-protect assignment (service `0x0c`, command `0x16`) **hung
the kernel**: the qcom SMC64 ABI carries at most six arguments in registers
(`SCM_SMC_N_REG_ARGS` is 4 in the downstream driver), and a seven-argument
call has to be packed into a buffer and passed by address; sending it
directly traps in TrustZone and the core never comes back.

The create path therefore also falls back to a 64 KB item in the shared
memory (item 625), which both sides already read and write for GLINK, and
skips the SCM assignment for it (`fastrpc_heap_smem`, `fastrpc_assign=0`).

On the phone, `INIT_CREATE_STATIC` with the name `adsp_listener` and the
64 KB shared-memory heap answers **`0x80000416`** (an AEE "unsupported"
code): this DSP firmware does not load the listener that way, and the
`adsp_listener` open still hangs afterwards. The remaining routes are the
module file the DSP's own `mod_table` asks the AP for — the AP-side
`apps_std_imp.c` looks in `/vendor/lib/rfsa/adsp` and `/vendor/dsp/adsp` —
and `FASTRPC_RMID_INIT_CREATE` (method 6) with the `fastrpc_shell_2` that
the hexagon-dsp-binaries tree carries for SM8250.

## The remote heap, and what it did not fix

The vendor's remotectl has a second method, `grow_heap(phyAddr, nSize)`: method
2, one 8-byte input buffer, no output (`sc = 0x02010000` on the wire). It is
**refused before any attach** (`0x80000474`) and **accepted after one**
(retval 0). The shared-memory partition fits a 124 KB page-aligned heap, which
is what the driver offers (`fastrpc_grow_kb=1024`, `fastrpc_grow_after=1` for
the sensors PD's own session, `fastrpc_phone=6` for the root PD's first).

None of the placements wakes the sensor process: every call still answers 0,
`next2` still times out, and the process still meets its user-PD watchdog.
The missing piece is in the DSP's own start of `sensor_process`, not in the
remote heap, the listener or the file service.

## Reading the DSP's own code

The firmware can be disassembled: `readelf -l fw-slpi/slpi.mdt` gives each
segment's load address (b15 is the segment at `0x8A000000`), and
`tools/hexagon-wrap.py fw-slpi/slpi.b15 /tmp/slpi15.elf 0x8A000000` followed by
`llvm-objdump -d --arch-name=hexagon` (LLVM lists hexagon among its targets).
A string at file offset N is then at `0x8A000000 + N`.

That is how the watchdog string was decoded. `TMR_CLNT_1:0x88` is not a file
error: b15's `timer_client_user_init.c` opens the QDI device `/dev/timers`
under the name `TMR_CLNT_%u`, and the string it would log is
`timer_client_user_init: qdi_open failed`. The sensor process dies opening its
timer client, before it ever asks the AP for anything. The remaining question
is what makes that `qdi_open` fail in the PD the AP starts (a PD attribute or
a resource the Android kernel supplies at PD creation), and the disassembly is
now the way to answer it.

## The service-location registration (servreg)

Android's kernel registers a **service-location client** for the sensors PD
before it attaches: downstream `adsprpc.c` sets
`SENSORS_PDR_SLPI_SERVICE_LOCATION_CLIENT_NAME` (`"sensors_pdr_sdsprpc"`) for
the service `"tms/servreg"` whenever `FASTRPC_IOC_INIT_ATTACH_SNS` runs. The
SLPI advertises that notifier as QRTR service **66 (0x42)** at node 9 port 3
(`qrtr: a service: 66 (0x42), version 1, instance 90`); the locator (service
64) is not advertised. The downstream attach message's one input buffer is the
caller's **tgid** (`ra[0].buf.pv = &tgid`), not a client id; our driver sends
the client id there.

The QDI failures fit a PD spawned without the sensors configuration: the
watchdog, island manager, power manager and timer QDI opens all fail
(`qdi_open("/dev/timers")`, `dog_qdi_init failed`) — a generic user PD has no
QDI devices. The fix is the registration the phone's kernel does: QMI message
`0x20` (`SERVREG_REGISTER_LISTENER_REQ`) with TLV `0x01` = enable and TLV
`0x02` = the service path, answered with a result and the PD state (mainline
`pdr_interface.c` / `qcom_pdr_msg.c`; state values `0x1fffffff` UP,
`0x0fffffff` DOWN, `0x2fffffff` EARLY DOWN, `0x7fffffff` UNINIT).

The driver now implements it: boot argument `servreg=1` sends a QRTR
`NEW_LOOKUP` for service 66 on port `0x101`, then the register-listener
request for `msm/slpi/sensor_pd` (the path in the firmware's servreg table,
next to `tms/servreg`), and prints the answer. It runs in step 10 before the
FastRPC chain, so the same boot can attach afterwards and the DSP's last words
show whether the QDI devices came with the PD. The image must carry
`fw/slpi.mdt` and `slpi.b00..b20` (`SLPI=1 userland/build.sh`); the default
wirelessfull scene does not, and step 2 stops without them.

The 7.9 MB of firmware does not fit the loader's 64 MiB payload area next to
the daily scene: kernel 16.2 MB + `root-slpifull.apfs` 51.2 MB made a 67.4 MB
payload, the loader's stage failed and left its USB endpoint unresponsive
(`logs/` run of 09:23). A test scene built without the audio firmware fits:
`cd userland && REST=1 BLUETOOTH=1 GL=1 SLPI=1 RC_JOBS=rc.jobs.wirelessfull bash build.sh`
and copy `root.apfs` to `out/thyme-boot/root-slpitest.apfs` (the audio
firmware is only needed when the kernel's `adsp=1` path runs before the root
is mounted).

The first registration answered (`logs/093621-run.log`): the notifier replied
QMI 0x20 with result 0, error 0 and `state 0x1fffffff (UP)`, so the DSP's
servreg considers `msm/slpi/sensor_pd` up. The sensor process's last words
changed with it: the timer client's error is now **0x89**, where every boot
without the registration reported 0x88 (the crash item is written by this
boot's process, so it is new). The FastRPC chain was skipped in that run
(`fastrpc_smmu` was unset, and the chain runs only after the SMMU bypass), so
the next measurement runs the attach after the registration.

With the registration in place the full chain runs: SMMU bypass,
`INIT_ATTACH_SNS`, `remotectl_open`, register and `init2` all answer 0 and
`next2` waits (`logs/094147-run.log`, `logs/094812-run.log`). The crash item
still reads `TMR_CLNT_1:0x89`; its whole 256 bytes hold only the watchdog
line (`USER-PD DOG detects stalled initialization`), so the item adds no
error detail. Whether 0x88/0x89 is an error code or the QDI handle the timer
client received (the TMS proxy may open one more handle in the PD, shifting
the number by one) is the open question. The driver can dump any SMEM item
as text (`slpi_dump_item=N`); item 473 (12 KB) turns out to be binary state,
not the DSP's console, so the DSP's own log is not in these items
(`logs/100002-run.log`).

A control boot with neither the attach nor the registration
(`logs/095517-run.log`) settles who starts the sensor process: item 424 is
not in the table at step 1, and by step 10 it holds `TMR_CLNT_1:0x89` again.
The sensor process therefore starts on its own when the DSP boots and fails
its QDI timer client before the AP has done anything. The AP-side tunnel and
the servreg registration are not what breaks the PD; the PD's own start at
DSP boot is.

The next step is inside the DSP's timer client. `llvm-objdump` over `slpi.b15`
(in a wrapped ELF at `0x8A000000`, 474k instructions) does not show a
reference to the message strings: no `immext(#0x8a14)`, no `#0x2f7a` and no
literal 0x8A142F7A or 0x142F7A anywhere in the segment. The code addresses
its data position-independently (through the GP register, or a loader-patched
table), so following the call needs a Hexagon-aware tool (Ghidra's Hexagon
module) or the QuRT `timer_client_user_init.c` source, not a flat objdump.

## The APSS service locator (QRTR service 64)

The DSP's servreg has a client of Android's `pd-mapper`: the firmware's
strings say it (`Attempting APSS Service Locator Server connection`,
`SERVREG_QDI: user-pd servreg_qdi_init failed`, `Servreg ack timedout`), and
Android's QRTR table has the AP's "Service registry locator service" at node 1
(`qrtr-lookup` on the running phone; service 64, version 1 instance 1). The
protocol is the open-source `andersson/pd-mapper`: QRTR service 64, QMI
`servreg_loc.qmi` (get_domain_list 0x21, the service name in TLV 1, the
result in TLV 2, the domains in TLV 0x12 as a count and
name/instance/valid/data entries; pfr 0x24), and the maps come from the
firmware's JSONs on the phone (`/vendor/firmware_mnt/image/slpir.jsn`,
`slpius.jsn`, `adspr.jsn`, `adspua.jsn`, `cdspr.jsn`: `tms/servreg` in
`msm/slpi/root_pd`, `msm/slpi/sensor_pd` (instance 90), `msm/adsp/root_pd`,
`msm/adsp/audio_pd` (74), `msm/cdsp/root_pd` (76)).

The driver serves it (`slpi=… servreg_loc_advertise/servreg_loc_input`). In the
boot the DSP's node 9 port 13 asked for `tms/pdr_enabled` and
`tms/pddump_disabled`. The phone's own map, pulled with su
(`/vendor/firmware_mnt/image/sdx55m/modemr.jsn`), lists `tms/pdr_enabled` for
`mdm/modem/root_pd` instance 165 and nothing for `tms/pddump_disabled`; the
locator answered 0 domains for both at first (`logs/200444-run.log`) and,
made to answer as the phone does, 1 domain for `tms/pdr_enabled`
(`logs/162943-run.log`) — and the sensor process's crash item is unchanged:
`TMR_CLNT_1:0x89 ... USER-PD DOG detects stalled initialization`, with the
SNS service 400 acking the SUID request (QMI result 0) and sending no SUID
event. The locator is therefore not the missing piece by itself — but where the TMS
lives is. The phone's map puts `tms/pdr_enabled` on the modem's domain, and
with the modem down the sensor process's timer client stalled every boot
(`TMR_CLNT_1:0x89 ... USER-PD DOG detects stalled initialization`). Answered
with `msm/slpi/root_pd` **first** and the modem's domain second, the timer
client comes up: `logs/193010-run.log` has no `TMR_CLNT` item at all and ends
with `THE SENSOR DSP STAYS ON while the system runs`. The failure then moves
one step on: the sensor PD's own `servreg_monitor.c:537` times out waiting
for the AP to acknowledge `msm/slpi/sensor_pd`
(`Servreg ack timedout:msm/slpi/sensor_pd:State:0xfffffff`), and the SNS SUID
request is still answered with a QMI result and no event. Serving the DSP's
listener registration on the AP's notifier (service 66) with the domain's
state is the next piece.

The ack is in, and it is the mainline one: when the notifier sends
`SERVREG_STATE_UPDATED_IND` (0x22, TLV 0x01 state, 0x02 path, 0x03
transaction id), the driver answers `SERVREG_SET_ACK_REQ` (0x23) with TLV 0x01
the indication's path and TLV 0x02 its transaction id
(`drivers/soc/qcom/pdr_interface.c`'s `pdr_send_indack_msg`, message ids from
`pdr_internal.h`). The DSP acknowledges every one with result 0/error 0, and
the "Servreg ack timedout" is gone: in `logs/200251-run.log` the notifier
reports down/up (txn 2-5) as the sensor PD restarts and the DSP's last words
change to `EX:kernel:0x0:sns_sig_remote_:0x2061:PC=0xb01c1f2c`
(`logs/200251-run.log`). The listener registration also answers the
protection-domain-failure request (PFR, 0x24: TLV 0x01 service, 0x02 reason)
now, so the failure's own words are in the log: the sensor process dies in
`irq115:0x1072 ... sns_err_handler.c:180:Crash`, and the crash PC is a
`memcmp` whose first pointer is NULL in the SNS signal path (the remote
processor-state signal, `sns_sig_remote_proc_state`). The registry files
from the phone (`/mnt/vendor/persist/sensors/registry`, 189 files) now travel
in the scene, but the reverse tunnel never opens them: the process still
reads its registry before the listener is ready. `fastrpc_restart=1`, the
ordering that restarts the PD with the listener waiting, crashes the fresh
process at once in `frpck_1_2:0x5b` (`logs/201811-run.log`), so the next
question is what the SNS signal path compares when it receives the remote
processor-state interrupt.

A control boot answers where that crash comes from: with the servreg
registration and the FastRPC listener but no SNS request at all (`sns=0`), the
whole run has **no protection-domain failure** — no `TMR_CLNT`, no `irq115`,
no item 424, and the DSP stays up to the end (`logs/204756-run.log`). The SNS
client is therefore what destabilizes the sensor process. Its first request
sweep sent the invalid client bytes 1, 2 and 0xff, each answered result
1/error 1, before settling on the byte the phone's sensh sends (0); the sweep
is gone and only 0 is sent now. The run that follows measures whether the
process stays up and whether the SUID event finally arrives.

The phone's own request was then read off the wire: strace of
`android.hardware.sensors@1.0-service` on the running Android (root, strace
from /system/bin) shows every SNS client request as the phone sends it —
QMI 0x20 to service 400, TLV 0x01 the SSC array around the protobuf, a
`sns_suid_req` of `{name, 1, 0|1}` (field 3 is 0 for the registry and 1 for
every sensor), the SUID placeholder 0xab (the same guess the driver made),
and **TLV 0x10 of one byte, value 1**. The driver's TLV 0x10 had the value in
the length's high byte, so it sent either length 1/value 0 or a malformed
0x0101; the DSP refused the latter (result 1/error 1) and accepted the former
but stayed silent. The request now matches the phone byte for byte (65 bytes
for `registry`) and the DSP answers result 0/error 0 — but no SUID event
follows and the sensor process still dies ~5 s later, on *any* request, valid
or refused. The client manager is what the first request starts, and what it
lacks (the registry the sensor process reads once at PD start through the
reverse tunnel, before the listener exists) is the next question; the
`fastrpc_early=1` order brings the listener up at the DSP's boot instead of
after the QRTR listen.

The same capture also holds the DSP's answers: the `sns_suid_event`s carry
real SUIDs — the accelerometer's is
`0x1fbb6afc01727ea6`/`0x9a418d19f8d7ba44` (high/low), the value the phone's
DSP reports to each HAL instance. With `sns_accel_direct=1` the driver sends
the accelerometer's `SNS_STD_SENSOR_CONFIG` (513) to that SUID without any
lookup: the DSP **accepts it** (result 0/error 0, `logs/221040-run.log`) and,
for the first time on this path, the sensor process does **not** crash — the
whole run has no `TMR_CLNT`, no `irq115`, no item 424. The SUID lookups
(message 512) are what start the service the registry cannot answer and take
the DSP down; a config to a known SUID does not. No `SNS_STD_SENSOR_EVENT`
(1025) follows the config yet, though, and the reverse tunnel still receives
no request, so the stream itself — likely the client manager's instance and
its registry group — is the next piece.

The registry's shape is known now: on the phone it is one JSON file per group
under `/mnt/vendor/persist/sensors/registry/registry/` (189 files —
`ak0991x_0`, `bmp285_0`, `<sensor>.<item>`, each
`{"<group>":{"owner":"sns_…","<item>":{"type":"grp","ver":"0","data":""}}}`),
and the AP does not *serve* the files: the sensors HAL carries the protobuf
messages `SNS_REGISTRY_READ_REQ`/`WRITE_REQ`/`READ_EVENT`/`WRITE_EVENT` and
`SNS_REGISTRY_TYPE_…` in `libsnsapi.so` (the DSP's strings say
`Sending registry request for group name:%s` and `Registry Event decoding
complete (%s)`), so the AP **uploads** the groups over the SNS client service
(QRTR 400) and the DSP keeps them. Populating the sns therefore means sending
those groups ourselves; the message IDs and protobuf field numbers are the
next thing to take from the phone's `libsnsapi.so`/`sensors.qti` or from the
DSP's IDL tables. The registry files themselves are already readable and can
travel in the scene.

The first of those is in: `llvm-objdump` of `libsnsapi.so`'s
`sns_registry_msgid_IsValid` (the generated enum checker) shows the four
message ids as `msgid - 0x200 < 3` plus `msgid == 0x300` — so
`sns_registry_read_req`/`read_event`/`write_req` are 0x200/0x201/0x202 and
`write_event` is 0x300. They travel the way the SUID request does: a QMI
`SNS_CLIENT_REQ` (message 0x20) to QRTR service 400, the protobuf's field 2
carrying that suite id and the registry protobuf in the payload, the target
SUID being the registry sensor's (our `sns_ask("registry")` already asks for
it). The protobuf layout is now read off the library outright. `llvm-objdump`
over the generated parsers in `libsnsapi.so` (ARM64, the exported
`_ZN…MergePartialFromCodedStreamE` symbols) shows each accepted tag as a 32-bit
compare against the full tag byte — field number in the top bits, wire type in
the low three:

- `sns_registry_data` (at `0xb1f5c`): only tag `0x1a` — field 3,
  length-delimited, repeated: a group's data is a list of
  `sns_registry_data_item`s.
- `sns_registry_data_item` (at `0xb0e44`): `0x0a` field 1 string (the item's
  name), `0x15` field 2 fixed32 (version), then the oneof `0x52` field 10
  `sns_registry_data` (a subgroup), `0x5a` field 11 string, `0x65` field 12
  float, `0x69` field 13 sfixed64 — the JSON files' `"type":"grp"` item is the
  subgroup, `"type":"int"`/`"flt"`/`"str"` the scalar fields.
- `sns_registry_read_req` (at `0xb29dc`): `0x0a` field 1 string.
- `sns_registry_write_req` (at `0xb403c`): `0x0a` field 1 string (the group's
  full name), `0x12` field 2 the `sns_registry_data`.
- `sns_registry_write_event` (at `0xb4b7c`): `0x08` field 1 varint
  (`SNS_REGISTRY_WRITE_STATUS_SUCCESS` 0, `ERROR_OTHER` 1).

So a write is: `SNS_REGISTRY_MSGID_SNS_REGISTRY_WRITE_REQ` (0x201) as the
suite message id, protobuf `{1: name, 2: {3: [{1: item, 2: version,
10/11/12/13: data}...]}}`, to the registry sensor's SUID over the SNS client
service. What is still open is that SUID: the client acks the request without a
SUID event (`logs/200444-run.log`), so the upload cannot be addressed until the
registry sensor's SUID is known — the same PD stall that keeps the sensor
process from starting a nested call.

## Risks and open questions

- The DSP's sensor process has not started a nested call in 150 s of a boot
  with the listener registered and `next2` outstanding; it neither watchdogged
  (which it did without a listener) nor asked. The listener may be registered
  in the wrong PD for the DSP's file daemon — `fastrpc_pd` now selects it —
  or the process may be blocked in a request made before registration.
  Hexagon disassembly of `slpi.b15` (the segment at 0x8A000000,
  `tools/hexagon-wrap.py`) names the modules behind the crash item: the
  timer client's own strings sit at 0x8A142F4C-0x8A143060 —
  `timer_client_user_init.c`, `TMR_CLNT_%u`, `/dev/timers`,
  `timer_client_user_init: qdi_open failed`, `CreateWorkerThread failed`,
  and beside them the user-timer client's `UTMR_CLNT_%u` and `/dev/utimers`;
  the crash text itself is at 0x8A139104 (`USER-PD DOG detects stalled
  initialization, triage with IMAGE OWNER`). The strings are reached through
  the firmware's own tables (no 32-bit pointer to either address exists), so
  the init call that blocks is the next thing to read in the disassembly.
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
| `logs/182550-run.log` | `fastrpc_phone=2`: `adsp_default_listener` opens (retval 0); the DSP's first nested calls arrive and `hw_platform` is served; no watchdog |
| `logs/183050-run.log` | the same with `sns=1`: the DSP's `frpc_dsp` thread excepts at `PC=0xb20c2dac` (`slpi.b20`, offset `0x6022c`) |
| `logs/230419-run.log` | `fastrpc_open_registry=1`: the DSP's rtld opens the registry module and asks the file service for `libsns_registry_skel.so` |
| `logs/234154-run.log` | with the scene-image fallback and the 16 KB packet / 8 KB out cap: the 4 KB skel read is served whole (4120-byte reply) and the loader rejects the file -- `wrong ELF class 2 expected 1` (the phone's copies are AArch64/ARM32, the DSP wants Hexagon ELF32) |
| `devices/thyme/drivers/sensor/hakios.thyme_slpi.c` | the loader, GLINK/QRTR and SNS client |
| `devices/thyme/CHECKLIST.md` | the hardware checklist and current blockers |
