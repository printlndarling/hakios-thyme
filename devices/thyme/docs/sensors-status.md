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
  *(Corrected 11 November: that read is the fusion sensor's own init, not
  the registry's — see the last section.)*
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
  With the heap in place it still answers 5: the clean A/B (no PD restart,
  resident image, the loader's heap accepted with retval 0, the platform
  file served again) sends message 1 and the config, and both answer 5
  (`logs/114433-run.log`). The heap is not what the client manager lacks; the
  registry groups are.
- **The registry's real SUID and the `sns_cm` group are known** (10
  November): a freestanding QRTR probe on Android (`tools/qrtrprobe.c`: a raw
  `AF_QIPCRTR` socket, the phone's own SNS client request, the DSP's replies
  printed byte for byte) sent the "registry" lookup to service 400 (node 9,
  port 11). The event carries the name with low `0x5e2541b4701e2275` / high
  `0xe12754a7007f2759`; the same probe's "accel" event carries the known
  accelerometer SUID byte for byte, so the parse is right. That is the
  address a registry group is read from or written to, and none of the seven
  candidates tried earlier was it. The `sns_cm` file in the scene holds the
  client manager's `heap_size` (1572864), `max_batch_disabled` (0) and
  `min_batch_period_thrshld_ms` (10); the driver sends a
  `REGISTRY_WRITE_REQ` (0x201) with exactly those items to that SUID
  (`sns_registry_write=1`). The write is **accepted with result 0**
  (`logs/124920-run.log`, `logs/130603-run.log`) — the first sensor message
  this DSP has taken — but message 1 and the config still answer 5, whether
  the heap is grown on the FastRPC client the attach used (0) or on client 1,
  and the DSP makes no file call afterwards, so the group may live in the
  service's memory without reaching the client manager. A write that adds the
  persist file's "owner" as a string item (194 bytes) is not answered at all;
  the 169-byte one without it is the accepted form.
- **All 189 group files parse, and one boot sends 188 of them** (10
  November): the uploader had three faults. The item parser's `"type":"`,
  `"ver":"` and `"data":"` needles end with the value's *opening quote*, and
  the code stepped one byte past it, so `sns_json_string` rejected every item
  and every file failed to parse (the earlier "0 of 189"). The image keeps a
  file's bytes with no terminator, so the scanner then ran past the file's
  last `}` into whatever the image holds next and could assemble a group of
  several kilobytes; `sns_request` copied that payload through `body[64]` and
  an output buffer of 512 bytes on the stack, and the write smashed the
  sender's stack canary (panic `Kernel stack memory corruption detected`,
  `logs/140749-run.log`). The parser now reads a NUL-terminated copy bounded
  by the file's size, `sns_request` counts what it will write before writing
  it, and `sns_qmi_send` takes its buffers from the heap when a group needs
  more than the stack ones; `hakios_image_file`'s path buffer takes the
  nested names (192 bytes). One boot now sends **188 of 189** groups — the
  one left is `sensors_registry`, an empty file — without a panic
  (`logs/142126-run.log`), and every write is answered **result 5**, as the
  client request and the config are. The bytes and the message id were not
  the whole story after all: see the 0x202 bullet below, which shows that
  the "result 0" the probe saw on the phone was an ACK the client service
  gives any message of that kind, and that the write had gone out as a
  read_event. What had been settled with the probe is the protobuf itself:
  the same item and subgroup encoding is what the phone's DSP accepts.
- **The registry write went out under the wrong message id** (10 November,
  later): the registry suite's enum is read_req 0x200, read_event 0x201,
  write_req **0x202**, write_event 0x300 (`libsnsapi`'s
  `sns_registry_msgid_IsValid` accepts `msgid - 0x200 < 3 || msgid == 0x300`,
  in the .proto's order). Every write this driver made — the `sns_cm` group
  and later the 188 group files — went out as 0x201, a *read_event*, and the
  SNS client service acknowledges such a message whatever it holds: on the
  phone, a write sent to an SUID that does not exist (all 0xab) is answered
  result 0 exactly as one sent to the registry's own SUID
  (`tools/qrtrprobe2.c`). So "accepted with result 0" never meant the
  registry took the group. Writes now use 0x202 (`logs/164538-run.log`:
  QMI 514). The registry read path (0x200) has never produced a read_event,
  on the phone or in this kernel: the 8-byte client id in TLV 0x10 that a
  first message's reply carries is rejected at the QMI layer, the 1-byte one
  is accepted and still silent, so a write cannot yet be confirmed by reading
  the group back.
- **The client manager never comes up in any HakiOS boot logged today** (10
  November): message 1 (the phone's first per-sensor request) and the config
  are answered **result 5** in every run, including the ones whose sensor
  process served its `apps_std` calls (`logs/154557-run.log`,
  `logs/165607-run.log`). The write's own ACK is not a readiness signal
  either: the same 169-byte `sns_cm` write was answered 0 in some boots and 5
  in others inside the same half-hour (`logs/123830-run.log` all 5,
  `logs/124346-run.log` and `logs/124920-run.log` 0), and waiting does not
  change it — 45 repeats over 90 s never left 5 in a fresh boot
  (`logs/165607-run.log`). The sensor process is equally unsteady: the same
  binary and flags serve the `hw_platform` read in one boot and no call at
  all in the next, and a second kernel in the same loader session is not
  worth reading (`xl run` warns it "died at once"; `xl reboot` first). What
  makes the DSP's framework finish its initialization is still the open
  question; the registry's persist directory through the reverse file API and
  the client manager's own heap/timer setup are the two candidates, and the
  DSP's log (QSH/diag) is what would name the step it stops at.
- **The DSP's log is not plainly in the shared memory** (10 November, later):
  `slpi_smem_dump=1` lists every item after the SNS part and prints the
  printable runs of each large one (`logs/173648-run.log`). Our partition
  holds 10 items (430, 479, 478, 481, 480, 608, 613, 611, 621, 623; the
  queues 479/480 are 16 KB each), the global one 23 (the largest are 470,
  79 at 40 KB and 426 at 80 KB), and neither 79 nor 426 holds a single run of
  eight printable characters, so whatever the DSP writes about its own
  startup is not a text ring there. A second dump at a 4 KB threshold reads
  the smaller ones too (`logs/180503-run.log`): the 16 KB item 479 holds our
  own upload as it left the AP — the QRTR queue with the groups' names
  ("tcs3701_platform.prox", "near_threshold", ...), item 469 starts with the
  firmware's version string `00:BOOT.XF.3.2-00304-SM8...`, and 79, 426 and
  470 are zeroes or carry no printable run. Getting the DSP's own log would
  need the diag/QSH channel (the DSP offers none of its GLINK channels by
  that name) or its DDR; the SMEM path is closed.
- **The DSP's DDR window is not the AP's to read**: mapping and scanning
  0x88000000-0x8b000000 (the segments' window) took the kernel down — the
  loader reported "hung" and restarted the phone (`logs/183746-run.log`, no
  kernel log at all) — so that diagnostic is removed and the DSP's log can
  only come through the diag/QSH protocol, not a memory read.
- **The DSP answers no "DIAG" channel either**: GLINK channels may be asked
  for by either side, so the driver now defines one named "DIAG" and opens it
  once the versions agree (the link hex-dumps what would arrive,
  `diag_input`). Nothing came back — no open acknowledgement, no packet, in a
  run that otherwise went through its SNS part normally
  (`logs/191016-run.log`) — so this firmware's diag router is not reachable
  from the AP's side of GLINK, and the sensor framework's own log stays out of
  reach. The remaining ways to it would be the QRTR diag service (if any of
  the DSP's 8 services is one) or a userspace diag daemon on a future day.
- **Our messages do not disturb the sensor process** (10 November, later):
  with `sns_quiet=1` the listener and the file service stay up and not one
  SNS message goes out (`logs/175050-run.log`, 0 QMI sends), and the DSP still
  stops after the `hw_platform` read exactly as when the messages are sent.
  So the stall is in its own initialization, not in an ill-timed request; and
  the `hw_platform` content is not the cause either — the phone's own
  `/sys/devices/soc0` values (THYME, Unknown, 0, 589824, SM8250, Snapdragon,
  356) are what the driver serves, and the config directory has the same 49
  files on both sides.
- **The wire is settled** (10 November): on Android the phone's DSP takes the
  same read and write bytes this driver builds, for the group
  `ak0991x_0`, for an int-only group and for a name it has never
  seen (`tools/qrtrprobe2.c`); its first message returns a type-4 event
  carrying the SUID, and a following request can carry the 1-byte client id
  from that reply in TLV 0x10 (8 bytes is rejected at the QMI layer). The
  registry's `read_event` still does not come back to the probe on Android
  either, so a write cannot yet be confirmed by reading the group back.
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
   heap is no longer the proven blocker. The daemon order
   (`fastrpc_bare`, the listener registered before any attach) does not work
   on this firmware: with no sensors PD session yet, init2 answers
   0x80000434 even after the registration, and the process the DSP started
   itself stalls into its timer watchdog (`logs/121802-run.log`). The
   non-bare order — attach first, then open/register/init2 — is the one whose
   init2 succeeds and stays the baseline.
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

### The official apps_std layouts, checked against a real call (2026-11-11)

Qualcomm's `apps_std.idl`, its generated `apps_std_skel.c` and the
reference `apps_std_imp.c` (the sources fetched on 10 November) give the
exact shapes the listener must honour:

- `fopen_with_env(in string envvarname, in string delim, in string name,
  in string mode, rout long psout)`: **five** input buffers -- the 16-byte
  scalar blob (the four string lengths) and the four strings, each
  non-empty and NUL-terminated -- and **one** output scalar, 4 bytes.
- `fread(in long sin, rout sequence<byte> buf, rout long bytesRead,
  rout long bEOF)`: inputs `{fd, buf_size}` in the scalar blob and the
  data buffer as the first out buffer; outputs `{written, is_eof}`.
- The listener's `outBufs` buffer is packed as the reference
  `pack_out_bufs` does it: for each out buffer a 4-byte length, an 8-byte
  alignment, then the data; the DSP-side `unpack_out_bufs` verifies each
  length against the one it asked for.

A real `fopen_with_env` call from the phone (110 bytes, log
`logs/235047-run.log`) decodes byte for byte as the IDL says:
`ADSP_LIBRARY_PATH\0`, `;\0`, `/sys/devices/soc0/hw_platform\0`, `r\0`,
and the out-length tail is a single 4. The driver's reply for it (length
4 + the file handle) matches that shape, and its `fread`/`fseek`/`fclose`
handling matches the IDL's scalar layouts too.

What the same log shows is the DSP repeating `open(handle 0, sc 0x20200)`
then `fopen_with_env(handle -1, sc 0x13050100)` with identical arguments
about fifty times, without ever issuing a `fread` and without our listener
answering an error. Since the framing and the lengths match the reference,
the rejection is on the DSP-side stub's acceptance of the reply, not its
shape; the driver now logs, for the first sixteen calls, both the out
lengths the call asked for and the bytes it encoded back, so the next run
can compare them exactly.

### The DSP's own listener carries the failure path we see (2026-11-11)

The sensor DSP firmware (the slpi.b* segments in the scene root) contains the
reverse-tunnel's client strings: `listener_hexagon.c`, `next2 unpack error`,
`Error %x: LISTENER unpack out bufs failed`, plus the get-in-bufs and
initialization failures. That failure path is the one the 9-10 November loops
took: a reply whose out lengths did not match the call's made the DSP-side
unpack fail, and the caller reissued the call.

With the out lengths indexed by the buffer's wire position (the 10 November
fix) the sequence no longer loops: `logs/082231-run.log` and
`logs/175050-run.log` show one `remotectl_open("apps_std")`,
`fopen_with_env("/sys/devices/soc0/hw_platform")`, two `fread`s (6 bytes,
then the end) and `fclose`, and the DSP does not repeat it. What the DSP does
*not* do is ask for anything else through the file API -- not
`/vendor/etc/sensors/sns_reg_config` (which it lists in its own strings), not
`/vendor/etc/sensors/config`, not `/mnt/vendor/persist/sensors/registry/...`
-- so its registry stays empty, the client manager never comes up, and every
SNS client message is answered result 5.

The phone's persist directory (`sensors-persist.tar`) shows what a working
boot holds: `registry/sns_reg_version` = `version=4` and the 189 group files
(`sns_cm` with heap_size 1572864 and the two batch knobs among them), written
by the DSP itself through this same file API. So the file API is the right
one; the DSP is simply not reaching the code that uses it in these boots.

Two experiments are prepared for the next run. `fastrpc_restart=4` releases
the sensor PD after the listener is initialized and attaches it again with a
fresh init2, so the registry read runs with a working listener from its first
instruction; the plain restart=1 left the DSP answering no next2 at all
(`logs/112921-run.log`), and a listener session belongs to the PD it was
initialized for, so it has to be initialized again. `fastrpc_wait=S` is a
plain delay before the chain; it does not repair the 0x80000434 answers seen
when the listener calls ran before the PD existed -- those decode to
`AEE_EINVALIDPROCESS` (`AEE_EOFFSET + 0x34`), i.e. no user PD yet, so the
attach must come first (`logs/121802-run.log`).

### The platform read is the fusion sensor's; the registry parser never runs (2026-11-11)

Reading the reassembled firmware (`tools/slpi-split.py`; the plain `cat` of
the split parts misaligns every segment after the first) separates two things
the earlier notes had run together:

- The `/sys/devices/soc0/hw_platform` read the listener serves comes from
  `sns_fusion_sensor.c`: the routine that builds the path (vaddr 0xb22f7880)
  sits among `sns_fusion_sensor_init acc: %f, mag: %f, gyro: %f`, `name=%s`
  and the `orientation` / `Calibrated Mag` / `mag_cal` / `xiaomi/akm`
  strings. It is the fusion sensor's own init, not the registry's.
- The registry's file backend (`sns_registry_parser.c`, `sns_registry_osa.c`)
  is complete in the image: `registry config file '%s' is not present`,
  `no update of sns_reg_config version`, `update of sns_reg_config version
  %s:=(expected)/NOT_EQUAL/(current)=:%s`, `skip loading properties file
  '%s'`, `skip loading output directory '%s'`, `skip loading config directory
  '%s'`, `mkdir error`, `stat error`, `Path is not a directory`. It begins at
  the config path `/vendor/etc/sensors/sns_reg_config` (vaddr 0xb23c4aa8;
  the code at 0xb22d369c reads it, then the `file=`, `output=` and
  `property=` entries).
- The parser is not called at boot in our runs: the registry is a *sensor*
  (`sns_registry_sensor.c`; the `SNS_REG_INIT` name at vaddr 0xb23c33d8 is
  used by the routine at 0xb22d2b04, whose pointer sits in the sensor API
  tables at file offsets 0x4cb9c8 and 0x6f2008), and nothing in our boots
  instantiates it. The registry stays empty, the client manager's registry
  read never completes, and every SNS client message is answered result 5 --
  the same conclusion the logs reached from the other side.

The AP-side half of the registry is known now too: the phone's
`libsns_registry_skel.so` implements one method, `get_property(name, value,
len)`, over Android's `property_get` -- the scalar blob is `{name length
including NUL, value length}`, the name is the second in buffer and the value
the first out buffer (method 0, sc 0x00010200). The parser's `property=`
lines resolve through it. The driver answers every `remotectl_open` with the
apps_std handle, so if the DSP ever opens that module the call needs a handle
of its own and this layout; it never has in any log so far.

Next on the firmware side: find what instantiates the registry sensor (the
sensor framework's boot list), since that is what gates the parse.
