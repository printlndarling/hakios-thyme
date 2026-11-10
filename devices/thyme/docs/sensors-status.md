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
  config, and the sensor process's registry build is not finished, so no
  stream is created. Everything below it works (the tunnel, the module
  open, the reads and closes), and the client manager's instance is what is
  still missing.
- **The registry read now terminates** (10 November): the fread reply's data
  buffer was sized from the wrong entry of the call's out-buffer lengths —
  the primary out slot (the 8-byte numbers) instead of the sequence buffer
  (the 512 bytes the DSP asked for). The DSP's IDL stub asserts its data
  buffer is at least as long as it asked, so every reply was invalid and the
  registry re-opened and re-read the file forever. With the lengths indexed
  by the buffer's position on the wire, the whole sequence is served: open
  the apps_std module (handle 0), `fopen_with_env`
  `/sys/devices/soc0/hw_platform` (fd 100), `fread` twice (6 bytes, then
  the end), `fclose` — and no retry (`logs/082231-run.log`,
  `logs/083020-run.log`).
- **The registry asks only for the platform file**: even with the listener
  brought up before the sensor PD starts (`fastrpc_early=1`), the DSP reads
  `hw_platform` once and goes no further. The per-sensor registry groups do
  not come through the file API; the AP uploads them over the SNS client
  service (`slpi-fastrpc.md`), and no sensors HAL runs in these scenes. The
  client manager's `sns_cm` group must therefore come from such an upload.
- **The client manager's instance** (`sns_client_manager.c`, which creates the
  per-processor info and needs the registry group `sns_cm`) never comes up,
  and every sensor message is answered with **result 5 in TLV 0x11**: the
  direct configs (513), the registry reads to seven candidate SUIDs (the
  three ASCII spellings and the four in the firmware's own SUID table at file
  offset 0x481948), and the phone's first per-sensor message. The phone sends
  **message 1** with the 4-byte payload `12 02 10 00` to each sensor's SUID
  before configuring it (root strace of the HAL; 32 of them), and its answer
  carries the client id (0x1d, 0x1e, ...) in TLV 0x10; here message 1 answers
  5, so the client manager's allocation is what fails (`logs/094020-run.log`).
  With the heap in place it still answers 5 (`logs/112921-run.log`).
- **The heap is in place and `grow_heap` accepts it**: the loader scans the
  firmware's memory map (the phone's map has `b0000000-bbb00000` and
  `c0000000-f0000000` free below 4 GB) for an 8 MB, 4 MB-aligned free range
  outside XNU's window and the payload, reserves it and passes
  `slpi_heap_pa`/`slpi_heap_size` on the kernel command line; the chosen range
  is 0xc0000000, `fastrpc_heap_reserve` takes it, and `remotectl_grow_heap`
  answers **retval 0** (`logs/112921-run.log`). That works because the
  interface's address is 32 bits (the IDL's `uint32 phyAddr`): every runtime
  allocation had come back above 4 GB (XNU ignored `max_pnum` and
  `KMA_LOMEM`, and its DMA allocator had no low range even at boot). The two
  shortcuts had taken the DSP down instead: the shared-memory partition as
  the heap (the sensor process's fastrpc memcpy, PC 0xb20c2dac) and a high
  kernel range (the DSP's logging path, PC 0xb01c1f2c).

**The boot layout for these probes**: `xl ramdisk system.apfs` must upload the
resident system image — it is md0 and holds `/sbin/launchd`, while the small
scene is md1 on `/redwood`, where the driver reads `fw/sensors` from. Without
it the kernel panics at 16 s ("exec of /sbin/launchd failed") before the
steps run (`logs/111936-run.log`). And the kernel's command line is 607
characters: the daily set plus the sensor arguments overflows it, the loader
says by how much, and the tail is cut — which silently dropped
`sns_gyro_direct`, `slpi_listen` and `slpi_delay` and made several runs look
broken (`logs/112318-run.log`).

## The next pieces, in order

1. **The restarted sensor process does not use the tunnel**: with
   `fastrpc_restart=1` the whole sequence succeeds — attach, grow_heap
   (retval 0 at 0xc0000000), listener open/register/init2, release, a fresh
   `INIT_ATTACH_SNS (again)`, and a waiting next2 (`logs/112921-run.log`) —
   yet the fresh process issues no reverse call and message 1 still answers
   5. The next question is the FastRPC client id of the restarted PD (the
   listener waits on client 1) and what the client manager still lacks; the
   heap is no longer the proven blocker.
2. **Then the registry upload**: the client manager needs the group `sns_cm`;
   the phone's HAL uploads the groups over the SNS client service with the
   `SNS_REGISTRY_WRITE_REQ` protobuf (message ids and field layout in
   `slpi-fastrpc.md`). The registry sensor's SUID is still unknown — the
   lookups answer with name-only events, and the firmware's four built-in
   SUIDs all answer result 5 to a read request.
3. **See the DSP's side**: the DSP log is still closed. The SLPI announces no
   diag service of its own (`logs/221040-run.log`, `logs/221918-run.log`:
   services 15, 24, 43, 51, 66, 400), the SMEM items the DSP makes (608, 611,
   613) hold no text (`logs/084550-run.log`), and `sns_diag_fw_log` needs the
   client manager the other pieces are for. What is left is the disassembly
   route in `slpi-re-plan.md`.

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
