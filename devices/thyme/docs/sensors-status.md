# Sensors on HakiOS: where the SLPI path stands

Everything below is measured on the phone (RAM boots; Android's own sensor stack
used as the reference on the same hardware and firmware).

## Working

- The SLPI boots from the phone's signed image, GLINK's IPCRTR channel opens, QRTR
  lists its services, and the FastRPC channel opens both ways.
- **servreg**: the AP registers as a listener for `msm/slpi/sensor_pd` and now
  **answers the notifier's state indications** with mainline's
  `SERVREG_SET_ACK_REQ` (0x23: the path and the indication's transaction id). The
  DSP accepts every ack (result 0), the old
  `Servreg ack timedout:msm/slpi/sensor_pd` is gone, and the notifier's
  protection-domain-failure reports (0x24) are read (service + reason) and
  answered.
- **The SNS client request is the phone's request, byte for byte**: QMI 0x20 to
  service 400, the SSC array TLV around the protobuf, `sns_suid_req {name, 1,
  0|1}`, the 0xab SUID placeholder for lookups, and TLV 0x10 = length 0x0001,
  value 0x01. (Read off `android.hardware.sensors@1.0-service` with a root
  strace; the driver's old TLV 0x10 put the value in the length's high byte.)
- **The DSP's real SUIDs** are in that capture: each `sns_suid_event` carries
  the sensor's 128-bit SUID (accelerometer high 0x1fbb6afc01727ea6, low
  0x9a418d19f8d7ba44; the gyroscope 0x97d3c48969f972a0/0xb9418a10491511c1; 32
  sensors in all).
- With `sns_accel_direct=1` / `sns_gyro_direct=1` the driver sends
  `SNS_STD_SENSOR_CONFIG` (513) to those SUIDs with **no lookup**: the DSP
  answers result 0/error 0 and — unlike every SUID-lookup boot — the sensor
  process does **not** crash (no `TMR_CLNT`, no `irq115`, no item 424,
  `logs/221040-run.log`, `logs/221918-run.log`).

The SNS client response carries TLV 0x10 (eight bytes) and TLV 0x11 (four):
the phone's HAL gets a nonzero 0x10 (0x1d, 0x1e, …) and 0x11 = 0, while every
answer here has 0x10 = 0 and 0x11 = 5. Changing the QRTR source port (0x100 to
0x1e42) changes neither, so 0x10 is the DSP's own client/instance number (0
being the first client of that boot) and not a transport artefact.

## Not working yet

- **No sensor data**: no `SNS_STD_SENSOR_EVENT` (1025) follows the accepted
  config. The stream is not created.
- **The SUID lookup (512) takes the DSP down**: its service reads the sensor
  attributes from the registry, which is empty here, and the sensor process
  crashes (`irq115`, then `EX:kernel:...sns_sig_remote_...`). A config to a
  known SUID does not.
- **The reverse FastRPC tunnel never receives a call — the listener's scalar
  was wrong** (found 9 November with `ioctlspy`, a small ptrace tool that reads
  the ioctl buffers strace cannot): sscrpcd's listener calls are INIT flags=3,
  INVOKE handle 3 sc `0x03000000` (init2), INVOKE handle 0 sc `0x20200` with
  `[22, 0xff]` and `"adsp_default_listener"`, INVOKE the returned handle sc 0
  (register), then INVOKE handle 3 sc **`0x04020200`** (next2) with a 16-byte
  input `{0, -1, 0, 0}`. The driver sent next2 sc `0x040202`, which decodes as
  method 0 with four inputs — an *open*, not the listener — so the DSP never
  queued a reverse call in any ordering tried (the default chain,
  `fastrpc_early=1`, the dual PD-0/PD-2 listeners, the late attach, the
  restart, the phone orders `fastrpc_phone=2/3/6`). Register is sc 0, not
  `0x300`.

  **The corrected call hangs the phone** (9 November evening): with next2 sc
  `0x04020200` and the phone's `{0,-1,0,0}` input (`52c4a77`, `c548ece`), every
  boot that runs the chain — stage 6, 8 or 9, kernel-only or with the scene —
  stops at the Apple logo and the loader's USB dies with it (the hung kernel
  takes the AP's USB service away; a power cycle is the only recovery). The
  daily scene without sensor arguments boots fine on the same kernel, so the
  crash is the first real method-4 listener call. The register's sc 0 was
  tried too (with its context kept) and also hung at stage 6, so it went back
  to `0x300` (`1e0e007`); the phone's sc 0 does not suit this session. Next:
  isolate with stage 5 (attach + open only) and stage 7, on a fresh phone,
  before trying next2 again. The earlier successful SNS runs' arguments
  (`slpi_step=10 slpi_keep=1`) are back in the test scripts.

  **Stage 5 hangs too** (21:21, a clean boot): with only INIT_ATTACH_SNS and
  the open, the phone still left USB right after the loader came up. The
  crash is therefore not the next2 call alone — the SLPI bring-up (or the
  attach) itself fails in the current state. The next run must be **stage 0
  (`sns=1 servreg=1`, no FastRPC chain)** to tell a DSP-state problem from an
  attach problem; if that hangs as well, the DSP firmware side needs a fresh
  look before any chain experiments.
- **The client manager's instance** (`sns_client_manager.c`, which creates the
  per-processor info and needs the registry group `sns_cm`) evidently never
  comes up: the stream cannot start. Its strings name the missing group and the
  heap it would allocate.

## The next pieces, in order

1. **Verify the corrected listener**: with next2 now `0x04020200` and register
   sc 0 (`52c4a77`), the DSP should answer the listener instead of treating it
   as an open. The first boot after the phone is reachable again runs the
   kernel-only sensor scene (`sns=1 servreg=1 fastrpc_smmu=2 fastrpc_call=9
   fastrpc_pd=2 sns_accel_direct=1 sns_raw=1 slpi_listen=30`) and looks for
   "the DSP asks for handle ... sc ... with N bytes of inputs". The next2
   input buffer now matches the phone's `{0, -1, 0, 0}` too (`c548ece`); the
   build with both changes is ready, and nothing else differs in the listener
   call the spy showed.
2. **Populate the registry**: the DSP reads `/vendor/etc/sensors/sns_reg_config`
   (the string is in its own image; `registry config file '%s' is not present`
   is its own log) and decodes the groups. Either serve that read through the
   tunnel (1) or send the registry groups as `SNS_REGISTRY_WRITE_REQ` (the
   protobuf layout is in `slpi-fastrpc.md`); the write needs the registry
   service's SUID, which its lookup cannot answer while the registry is empty.
   To see what the phone's own reverse calls carry, `sscrpcd sensorspd`
   restarted under an init-followed `strace -e ioctl` shows its FastRPC traffic
   as the `_IOC(0x52, ...)` ioctls (1, 6, 8, 0xC) at once; strace shows only
   the ioctl pointer, so the invoke buffers (and the file paths inside them)
   need a small ptrace reader — the tool to build if the tunnel stays silent.

   **The attach's payload is a process id, not a client id**: the phone's
   `drivers/char/adsprpc.c` `fastrpc_init_process()` sends, for
   `FASTRPC_INIT_ATTACH_SENSORS`, one 4-byte buffer holding the calling
   process's **tgid** on handle `FASTRPC_STATIC_HANDLE_PROCESS_GROUP`, sc
   `REMOTE_SCALARS_MAKE(0, 1, 0)` (0x10000). Our chain's payload
   (`fastrpc_client`, default 0) has been 0; the DSP's process-group handle is
   the one the reverse calls are addressed to, so a nonzero pgid
   (`fastrpc_client=1`) is the difference under test now.
   Also: the phone allocates its remote heap from the reserved `sdsp_region`
   DMA pool; SCM-assigning a generic kernel heap (the first attempt got only
   16 KB) stalled the scene at the SCM call and the loader restarted the phone —
   that experiment was reverted and must not be repeated with ordinary heap
   memory.

3. **Get the DSP's own log**: `sns_diag_fw_log` exists in the SNS diag sensor
   (`diag_sensor`); the HAL's `get_qmi_debug_flag` (in `/vendor/lib64/libssc.so`)
   reads `persist.vendor.sensors.debug.ssc_qmi_debug`, and setting it true does
   not change the HAL's traffic at all (the same 65 lookups and 32 disables,
   `init_sensors.strace` from the 23:28 restart): on this firmware the flag is
   a libssc logging switch, not a way to reach the DSP diag sensor.

**The reverse tunnel works** (22:36): with the steps on their own thread,
`slpi_delay=40` and the corrected next2 (`sc 0x04020200`, input `{0,-1,0,0}`),
the DSP queues its first reverse calls and the log shows them end to end:

```
[client 1 pd 2] the DSP asks for handle 0 sc 0x20200 (41 bytes: "apps_std")
apps_std: the DSP opened its module; handle 1
[client 1 pd 2] the DSP asks for handle -1 sc 0x13050100 (110 bytes:
  "ADSP_LIBRARY_PATH", "/sys/devices/soc0/hw_platform")
apps_std: opened /fw/sensors/soc0/hw_platform (6 bytes) as 1
[client 1 pd 2] the DSP asks for handle -1 sc 0x4010200 (24 bytes: fread)
```

The soc0 values are device constants read off Android and carried in the
driver (`hw_platform=THYME`, `soc_id=356`, `revision=2.1`, ...). The DSP
re-opens and re-reads: the file-op reply layout (fread's numbers, the module
handle) still needs to match what its apps_std expects — the next piece
(`ffc5d99`).

The phone's `INVOKE_FD` at sc `0x05010200` was taken for a reply at first; the
**IDL settles it** (`qualcomm/fastrpc`, `idl/adsp_listener1.idl`): method 5 is
`get_in_bufs2(ctx, offset, bufs, bufsLenReq)` — the listener fetching more of
a call's input bytes when its own buffer is too small — not a reply at all.
The phone captured earlier was doing exactly that, which is why sending
method-5 calls from here answered EBADPARM across every argument layout
(the +1 context, the out sizes, the opened handle: `0x80000434` →
`0x8000040E` → `0x80000414` as those changed). **Replies are only ever folded
into the next `next2`'s prevbufs**, which this driver already does.

The scalars of that next2 are `{ctx, result, the previous outputs' length, the
input buffer's length}`: a run that set the last two to 0 (copying the phone's
initial `{0,-1,0,0}`) stopped the DSP from delivering the calls at all —
`apps_std ... the arguments do not decode` — so the driver's original values
stand (`0d3e0c4a`). The out buffers now take their lengths from the call
(`ce18bf6`); for every call tried they matched the previous sizes, so the
DSP's retry of `fread` lies in the reply's *content* semantics, not its
framing.

**A sharper observation** (logs, 10 November): the DSP's every `fread` asks
for `fd 1` — the first open's fd — while each new `fopen_with_env` from this
driver returns a growing fd (7, 8, 9...) that no `fread` ever names. Two
readings fit: either the DSP rejects the fopen replies and keeps reading the
one handle it accepted first, or `1` is its own file slot and the opens are
its retries for another reason. The firmware's strings for this path are
`fopen failed file=%s error=%i, retrying` and
`sns_registry_osa.c:fread failed error=%i` (`sns_registry_osa.c`, file offsets
0x6887df and 0x6888fc in the SLPI image), so the failing step is one of the
two and its error code would say which — that code only exists in the DSP's
log, which is why the diag path or the disassembly of that function is the
next step. The `fread` served against fd 1 does return the file's bytes
(`THYME\n`), four times over.

**Qualcomm's official listener source settles the model** (fetched 10
November, `qualcomm/fastrpc`, `src/listener_android.c`): the classic listener
replies **through the next `next2` call** —

```
sc = 0xffffffff;
if (result != 0) outBufsLen = 0;
adsp_listener_next2(ctx, result, outBufs, outBufsLen,
                    &ctx, &handle, &sc, inBufs, inBufsLen, &inBufsLenReq);
```

— which is the folded form this driver already sends. The difference is the
*outBufs* buffer: the served call's output buffers are the ones **the DSP
reserved inside the same argument blob** (the bytes after the input buffers,
`sizes` from `unpack_out_lens`); the served function writes into them and
`pack_out_bufs` encodes them for the reply. This driver allocates fresh output
buffers instead and encodes those, so the DSP's requested out-buffer lengths
and the reply's contents never line up and it re-issues the call. The next
piece is therefore clear: decode the out-buffer lengths from the call blob,
write the served data into the call's own out region, and reply with
`(ctx, result, outBufs = that region, outBufsLen)` on the next next2.

**The steps ran on the kernel's start thread** (found 21:45): with
`slpi_step=N`, `run_steps()` was called synchronously from the driver's init,
so any step that never returns hangs the whole boot at the Apple logo — the
loader's USB service dies with it and no log can be read. The steps now run on
a kernel thread, and `slpi_delay=S` holds them back S seconds so the system
starts first and the loader's live log can show the steps up to a stall
(`663644a`). The first threaded test still ended at the logo, so the stall is
more likely hard (an SMC into TrustZone, or a lock) than a sleeping wait; the
delayed form is the next probe, on the driver at `246232e` — the last state
whose sensor runs completed (`logs/221040-run.log`, `logs/221918-run.log`) —
so a regression from the later edits can be told apart from the phone's state.

The registry was also probed directly (`sns_registry_probe=1`): a read_req for
`sns_cm` addressed to the SUIDs the name would produce in the three spellings
the observed SUIDs use (`registry` NUL-padded, and `registry________` /
`sns_registry____` with underscores) is answered result 0 by the transport but
produces no read_event (`logs/223806-run.log`), so the SNS client service ACKs
any SUID and the probe cannot prove which spelling (if any) is the registry's.
