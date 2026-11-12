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

### The file API writes now, and the error codes are the DSP's own (2026-11-11)

- **The write half of `apps_std` is served.** `fopen` (method 0) with
  `"w"`/`"a"` creates or truncates a file in a 4 MB RAM store, `fwrite`
  appends, `fflush`/`fsync` succeed, `fclose`, `fremove`, `mkdir` and `stat`
  work, and every read path (`fopen`, `fopen_with_env`) consults the store
  before the scene. The registry parser's output directory is where the
  phone's DSP writes its 189 group files through exactly these calls, so the
  write half is a prerequisite for the parser to complete; the store lives
  until the boot ends, which is all a RAM boot can offer.
- **The module open now gives `sns_registry` its own handle.** Handle 0 is
  `apps_remotectl.open`: `apps_std` gets 1, `sns_registry` gets 2, and method
  0 on handle 2 is `get_property` (the empty string, what an unset Android
  property gives) with the layout read from `libsns_registry_skel.so`. Before,
  every open answered with the `apps_std` handle, so a `get_property` would
  have been decoded as an `apps_std` call.
- **The AEE constants were wrong.** The driver answered every bad argument
  with `0x80000434`, which `AEEStdErr.h` defines as `AEE_EINVALIDPROCESS`,
  not `AEE_EBADPARM` (`AEE_EOFFSET + 0x0E = 0x8000040E`); `AEE_EFAILED` and
  `AEE_ENOMEMORY` were `0x80004005`/`0x80000002` instead of
  `0x80000401`/`0x80000402`. A refusal this driver sent was telling the DSP
  that its *process* was invalid.

The first thing the next sensor run will show is whether the fresh PD
(`fastrpc_restart=4`) reaches the registry parser: the parser opens
`/vendor/etc/sensors/sns_reg_config`, the config directory and the persist
output directory, and any of those calls in the log is the signal that the
registry sensor finally started. Every file-API call now says so on the
console -- `fopen` (read and write), `stat` (the store's file or the image's,
with the size or "not there"), `mkdir` and `fremove`, besides `opendir`'s
entry count -- so the parser's first step is visible even when it only stats
the config path.

### The registry SUID is the firmware's own UID (2026-11-11)

The 16-byte SUID this driver addresses
(`75221e70b441255e59277f00a75427e1`) is not just what the phone's lookup
returned: it is in the image as the registry sensor's own UID. The registry
sensor's API table (file 0x4cb938 / 0x6f1db8) carries, right after the
`SNS_REG_INIT` routine, a function at 0xb22d2b74 that returns a pointer to
exactly these bytes; they are stored at file 0x690553 (vaddr 0xb23c3553),
directly behind the `sns_registry.proto` name, low word first -- unlike the
accel/gyro SUID table at 0x481948, which stores its entries high word first.
The same 16 bytes appear again at 0x619718 in `sns_suid_sensor.c`'s area, so
the SUID sensor's own database knows the registry by this UID too.

The addressing of the registry reads and writes was therefore already right,
and whatever keeps the client manager from getting `sns_cm` is not a wrong
SUID: it is the registry sensor's own instantiation, which nothing in these
boots reaches (the parser's file reads never happen — see the section above).

### `fastrpc_restart=4` ran; the parser still does not (2026-11-11)

The first prepared experiment was run (`logs/092230-run.log`, kernel
`16883c86`, scene `root-slpitest` `2164c9c7`, SHA-256
`6118cda12f39d1d34ff8033c07ff4de0360413af1e5dabe6d7b678a924db9a90`). The
sequence is exactly the designed one:

```text
fastrpc: INIT_ATTACH_SNS: ctx 0x102 retval 0
fastrpc: adsp_default_listener_register: ctx 0x302 retval 0
fastrpc: adsp_listener_init2: ctx 0x402 retval 0
fastrpc: INIT_RELEASE: ctx 0x502 retval 0
fastrpc: INIT_ATTACH_SNS (again): ctx 0x102 retval 0
fastrpc: adsp_listener_init2 (again): ctx 0x402 retval 0
fastrpc: a listener waits for client 1 in PD 2 (tid 2)
```

Every step answers 0, and `next2` then waits with the listener in place. No
`apps_std` call is logged: the DSP never opens, stats or lists any file, so
the registry parser is still not reached even when the PD is released and
attached again with a listener initialized for it. The listener's readiness
at PD start was therefore not the missing piece.

What remains is the reason the sensor process never instantiates the registry
sensor: the earlier notes' `sns_cm`/registry-group path and the client
manager's own start-up. The next probe should look at the DSP's own start-up
sequence (the reassembled firmware's `sns_client_manager`/registry init) rather
than at the AP side of the tunnel, which is now demonstrated end to end.

### The boot list, and where it stops before the registry (2026-11-11)

Disassembling the reassembled image (`tools/slpi-split.py`; the file offset of
a vaddr in the code segment is `0x4cd000 + (vaddr - 0xb2200000)`, not
`vaddr - 0x8A000000`, which is the segment's *physical* load address):

- **The sensor framework boots a static list.** `sns_fw_init` at `0xb22a21e4`
  reads the count `memw(0xb23b93d0)` = **63** and walks `0xb23b91d8 + 8*i`;
  each entry is `{init_fn, n}`. The log string is
  `sns_fw_init.c:Register Sensor from static list %i` (`0xb23d5e58`). The
  fusion sensor is entry 28; its thunk `0xb22f7dd8` calls `0xb22f78dc` ->
  `0xb22f7760`, which builds `/sys/devices/soc0/hw_platform` (`0xb23cc1be`)
  and calls `fopen` (`0xb227a720`). **Correction:** that read is what entry 28
  does *when the process runs*; our logs show **no** such call -- the scene's
  `/redwood/fw/sensors/soc0/hw_platform` appears only in the file listing, and
  no `apps_std` call is logged in any sensor run (checked `logs/235100-run.log`
  and `logs/101729-run.log`). So the list is the framework's design, but our
  boots do not reach it.
- **The registry is entry 45.** Its entry at `0xb23b9340` is
  `{0xb22d2aec, 1}`; the thunk loads the registry's API record at
  `0xb2424dc4` and calls slot0, `0xb22d2b04`, which loads `SNS_REG_INIT`
  (`0xb23c33d8`), creates the sensor object and the instance/thread
  (`0xb2248ff0`, body `0xb22d2e8c`), whose first act is the parser init
  `0xb22d3694` opening `/vendor/etc/sensors/sns_reg_config` (`0xb23c4aa8`).
- **The API records** are a contiguous array of
  `{u32 size; void *slot[(size/4)-1]}`. The registry's (RW copy base
  `0xb2424dc4`, rodata `0x2b91944`): size `0x18`, slot0 = boot init
  `0xb22d2b04`, slot1 null, slot2 = the SUID getter `0xb22d2b74` (returns
  `0xb23c3553`, the 16 bytes `75 22 1e 70 b4 41 25 5e 59 27 7f 00 a7 54 27 e1`
  inline after `sns_registry.proto`), slot4 = the message handler
  `0xb22d2b80` (ids 0x200/0x201, "Failure creating registry group"). The
  fusion family's record at `0xb2424d20` has slot0 `0xb22f7dd8`.

Nothing in our boots reaches `sns_reg_init`'s fopen, while the fusion entry 28
before it does run, so the boot loop stops between entries 28 and 45 (or the
registry's thread creation fails); the exact stopping point is not proven.

The next measurement is a SUID lookup for the firmware's own name:
`sns_ask_name=SNS_REG_INIT` (boot arg added; the driver already asks for
"registry"). If the registry's SUID (`0x5e2541b4701e2275` /
`0xe12754a7007f2759`) is answered, entry 45 ran and the failure is in its
thread/parser; if it stays silent, the loop never reached it and the stall is
in entries 28-44.

### The SUID lookup answers zero for every name; the framework never runs (2026-11-11)

The probe ran (`logs/101729-run.log`, kernel `09cb2178`, scene `root-slpitest`
`2164c9c7`, args include `slpi_step=10 sns=1 fastrpc_restart=4
sns_ask_name=SNS_REG_INIT`, SHA-256
`6dbc79db4676d28f74e71209a65a08ba245ec64c0ff8799669af1ee1c28bac02`). The three
SUID requests ("registry", "accel" and the firmware's own "SNS_REG_INIT") are
accepted with QMI result 0, and each event carries the name with an **all-zero
SUID**:

```text
slpi: sns: "registry" has SUID 00000000000000000000000000000000
slpi: sns: "accel" has SUID 00000000000000000000000000000000
slpi: sns: "SNS_REG_INIT" has SUID 00000000000000000000000000000000
```

even "accel", whose real SUID the phone's own capture shows. The DSP is alive
(176 s, clean stop) and the SNS service answers, but its registry has no sensor
at all. Together with the missing file-service calls this is the same
conclusion from a new side: **the sensor process does not reach the framework's
static list**, so neither the fusion entry (28) nor the registry (45) runs.

The firmware-side map now says what *would* happen once the process runs; the
gate is in the process's own start-up (the PD/QDI path the earlier sections
describe), before `sns_fw_init`'s loop. The next measurement should read that
start-up's state after `fastrpc_restart=4` (item 424's text, the QDI device
opens, the user-PD watchdog) rather than the AP side of the tunnel or the
sensor names.

### The framework init is one phase of a boot state machine (2026-11-11)

Following the call chain above `sns_fw_init` upward (direct calls only; the
image's data sections hold no 32-bit pointer to any of these functions, so the
chain is straight code):

- `sns_fw_init` (`0xb22a21e4`) is called from `0xb22493e0`, inside the
  framework-init function at **`0xb22492e8`**. That function first tests a byte
  at **`0xb2a234e0`** and returns if it is non-zero, calls three init helpers,
  runs the 63-entry sensor list, and sets the byte to 1.
- Its only caller is **`0xb220750c`**, one of the boot-phase functions around
  `0xb22073c0`-`0xb22074b0`. Each phase function ends by writing a **boot
  phase marker** to the byte at **`0xb242fd79`**: 1 after its first group
  (`0xb22073c0`), 2 after the second (`0xb2207414`), 3 after the third
  (`0xb22074b4`), and the framework phase writes **8** (`0xb2207504`) -- it
  calls `0xb22492e8` first (`0xb220750c`).
- Those phase functions are called from `0xb2207528`-`0xb2207534`, which is
  called from **`0xb22b0ee0`**, inside the process's top-level init
  (`0xb22b0e94`): a long chain of init calls whose results are stored in a
  structure at `0xb2a28a80` (offsets +0x18, +0x1c, +0x20), with the sensor
  framework at `0xb22b0ee0` in that order.

So the framework's list runs only after boot phases 1-3 have completed, and
the process's phase byte (`0xb242fd79`) is written 8 only after it. The process
in our boots reaches neither the file service nor that call: the stall is in
the top-level init (`0xb22b0e94`) or in phases 1-3, which are the QDI/timer
and service bring-ups the earlier sections name. A runtime view of those
phases is what the next probe needs; static reading has taken the chain as far
as it can without knowing which call blocks.

### The SLPI's DDR is not readable from the AP (2026-11-11)

The phase byte lives at paddr **`0x8a22fd79`** (the segment vaddr
`0xb23f8000` maps to paddr `0x8a1f8000`, memsz `0x635000`), so a boot arg
`slpi_phase=1` mapped `0x8a22f000` after the SNS asks and read it. The run
(`logs/110459-run.log`, kernel `35d4e53f`, SHA-256
`35d1db22844d8de2c5881dc67ad16a64e701772d528e1075a3813dd65de212cc`) **never
printed the line** and ended at the read:

```text
psci: cpu 1 ends the system: the return to the boot loader is the boot processor's to make
psci: cpu 1 cannot return to the boot loader and the boot processor does not answer: all cores stop here
```

The SLPI's DDR carveout is the DSP's; the AP's access aborts (the hypervisor
does not grant it, and the return path itself failed at that point). So the
phase byte cannot be observed from the AP this way, and the runtime channel
has to come from the DSP's side: a crash dump it writes (item 424 via its
user-PD watchdog or an induced fault), its QDI/diag service, or its own logs.

### The diagnostic needs its own scene; the accelerometer's uuid is in b13 (2026-11-12)

`devices/thyme/slpi-run.sh` defaults to the daily `root-wirelessfull` scene,
which carries no `fw/slpi.*`, so every step stops at step 2 ("no fw/slpi.mdt
in the small image nor on the root"); `SCENE=out/thyme-boot/root-slpitest.apfs`
is the scene to run the steps with.

The accelerometer's suid from the phone's own capture -- low
`0x9a418d19f8d7ba44`, high `0x1fbb6afc01727ea6` -- is in the firmware as a
16-byte constant, the low half first, at `slpi.b13` offset `0x1f7d8`
(segment vaddr `0x02b30000` + `0x1f7d8`), and it heads a four-entry table of
`0x28`-byte records (uuid, a `0x18`, then five 32-bit pointers into the BSS
segment at `0xb210f000`): `44bad7f8198d419aa67e7201fc6abb1f`,
`c1111549108a41b9a072f96989c4d397`, `cd4b1a6d68da4a7785ab46e2cda5680b`,
`7932b89cdc0240afa38f80c0c0153697`. The registry sensor's uuid is not in
that table, and the framework's name strings are a packed blob with no
direct pointers to them (`accel` at b13 `0x61c3a`, `registry` at `0x61e20`),
so a uuid-to-name map is still to be found -- the next places to look are the
larger uuid tables in `slpi.b15` and the registry constants the framework
copies into its BSS at init.

A calibration run with that scene and only `slpi_step=10 sns=1`
(`logs/143543-run.log`, kernel `7fa5e99b`, scene `2164c9c7`) reaches step 7
and stops: TrustZone authenticates the image, the DSP says it is up after
102 ms, the SMP2P items appear and RPMh is handed back, and the log ends on
the third SMP2P item. Steps 8-10 and the SNS stage need the longer argument
set the sections above describe (`slpi_step=10 sns=1 fastrpc_restart=4
sns_ask_name=SNS_REG_INIT`), so a future run should take the args from the
specific section it reproduces rather than from this minimum.

The full set ran too (`logs/145037-run.log`): steps 1-10 complete -- TrustZone
authenticates the image, the DSP says it is up 109 ms after the start, GLINK
opens both ways, QRTR lists the DSP's services (400, the SNS client, among
them), and the servreg locator answers the DSP's two requests. The step-10
summary ("after 3 s: ..."), the final "THE SENSOR DSP IS UP AND LINKED" line
and every SNS ask are missing, so the step thread stops inside step 10 after
the two advertise calls. The next probe prints around the second advertise
and the listen loop to place it; `slpi-run.sh` now takes `BASE_ARGS` like the
modem probe (the loader's 607-character cmdline had room here, but not
always).

### The asks match the phone and no longer crash the process (2026-11-12)

With `slpi_pause=0 slpi_listen=1` -- the steps' 2200 ms pauses off and the
listen at one second -- the whole path fits in the log the loader keeps: the
three SUID requests (65, 62 and 69 bytes: `registry`, `accel`,
`SNS_REG_INIT`) go out to node 9 port 12 as QMI 512 with TLV `0x10` = 1, the
DSP acks every one with result 0/error 0, and the answers stay silent --
`registry silent, accelerometer silent`, `"SNS_REG_INIT" silent`
(`logs/150750-run.log`, kernel `7fa5e99b`). No protection-domain failure
follows, item 424 stays empty, and the steps end by stopping the DSP
cleanly; where earlier kernels lost the sensor process a few seconds after
any request, this one does not.

That leaves the missing SUID events as the SNS gate: the DSP accepts the
lookup and does not answer it. The next run takes the document's next
ordering, `fastrpc_early=1`, so the reverse tunnel's listener is up when the
sensor process starts and reads its registry.

The early ordering ran (`logs/151721-run.log`, `fastrpc_early=1`, 50 s so the
whole path stays inside the log): "FastRPC early: the channel is open; the
listener comes before the listen", then the same three asks, the same
result-0 answers, and the same silence -- `registry silent, accelerometer
silent`, `"SNS_REG_INIT" silent`. The FastRPC records no request from the
DSP at all (the only line is the loader's 8 MB heap), so the reverse tunnel
is up and unused. The gate is unchanged and now measured from both orders:
the sensor process neither answers the SUID lookups nor asks the tunnel for
its registry, which is the framework init the boot-phase disassembly was
following.

A correction to the "no longer crashes" reading above: `slpi_step=10`
*stops* the DSP when the steps end (the step-10 teardown line says so), so
the runs above ended around +30 s with the DSP already off and the 164 s
watchdog never had a chance to fire. `slpi_watch_item=424`, a new probe
argument, reads shared-memory item 424 every 20 s after step 10; in that
run the item goes from empty to `sys_m_smsm_non_mpss.c:278:APPS force stop`
-- the DSP's record of this driver's own stop, nothing more. Whether the
sensor process still stalls or crashes is measured by the next run, the
same configuration with `slpi_keep=1`, which leaves the DSP up for the whole
200 s.

That run settles it: with the DSP kept up (`slpi_keep=1`,
`logs/153617-run.log`) item 424 holds, from the first watch's read on,

```text
EX:kernel:0x0:sns_sig_remote_:0x2062:PC=0xb01c1f2c
```

so the sensor process still dies in the SNS remote-processor-state signal
path -- the same PC the earlier `0x2061` text named, with a different code
word. The PC is in `slpi.b04` (segment vaddr `0xb0152000`, offset
`0x6ff2c`), and there it is a string compare: the entry at `0xb01c1f20`
takes the first pointer into r3 (`{ r0 = #0x0; r3 = r0 }`, then
`loop0(0xb01c1f2c, r2)` and `memb(r3+#0x0)`), so the caller passes NULL as
the string it compares. The call sites are at `0xb0155dd0`, `0xb0157110`,
`0xb01686b0`, `0xb016b148`, `0xb016b4f4` and `0xb016cc44`; naming which of
them is the signal path, and what pointer is NULL there, is the next read.

Reading them narrows it: the site at `0xb0157110` is a name-keyed list walk
(`r23 = memw(r18+#0xc)`, compare `r22` with the entry's name at `+4` and its
length at `+8`, then `r23 = memw(r23+#0)`), but every path into it sets `r22`
from either the stack (`r29+#0x20`) or `r16` with a null check, so its key is
not the NULL. The other four compare against fixed globals with fixed lengths
(27, 26, 20 and 12 bytes) and take their first pointer from an argument. The
servreg registration itself matches mainline (`TLV 0x01` one byte enable,
`TLV 0x02` the path with its NUL), and the region around the registry
descriptor's code (`0xb22d2b04`, `0xb22d2b74`) is code, not a uuid table.

That points the NULL back at the sensor process's own missing state: the
signal path compares a name it expects to have from its registry, and the
registry is precisely what this PD never gets. The crash and the empty
registry are one gate, and the registry's SUID -- or a way to make the
reverse tunnel deliver the files -- stays the piece to find.

### The missing entry: the AP's "sleepstate" in the dsps SMP2P block (2026-11-12)

The section above left the crash and the empty registry as one gate. The crash is
not the registry's: it is one SMP2P entry the phone's kernel creates and this one
did not.

The phone's device tree (`/soc/qcom,smp2p-dsps`, `qcom,smem = <0x1e1 0x1ae>` --
the pair 481/430 this driver already uses) has four children: `master-kernel`,
`slave-kernel`, `sleepstate` (the child `sleepstate-out`, the AP's sleep state)
and `sleepstate_see` (the child `qcom,sleepstate-in`). The two "in" entries are
the DSP's own and the DSP makes them; the two "out" entries belong to the AP's
half, and Android's `qcom_smp2p` driver creates both at probe. Item 430 here
carried only `master-kernel`, so the DSP's sensor framework never found the
`sleepstate` entry the phone always has -- and its remote-process-state sensor
(`sns_remote_proc_state_sensor.c`, the thread `sns_sig_remote_proc_state`) is
exactly the consumer of it.

`step_our_item()` now writes `sleepstate` as entry 1 (`slpi_sleep=0` leaves it
out for an A/B). The item reads back as

    our item reads back: SMP2P version 1, features 0x1, from processor 0 to 3,
    flags 0, 2 of 16 entries: master-kernel = 0; sleepstate = 0;

and the DSP's first reaction is measurable at +146 ms: it adds its
`sleepstate_see` entry then, where every earlier boot added it at 2.2 s
(`logs/162243-run.log`: "the DSP's item changes: SMP2P version 1, features 0x1,
from processor 3 to 0, flags 0, 3 of 16 entries: smp2p = 0; slave-kernel = 0x6;
sleepstate_see = 0;").

The exception at `PC=0xb01c1f2c` is gone. `slpi_watch_item=424` reads item 424
during that boot at +20, +149, +169 and +189 s and never sees the
`EX:kernel:0x0:sns_sig_remote_:0x2062` text; what it holds is the SFR driver's
own boot note, "SFR Init: wdog or kernel error suspected.", which is what an
unclean stop of the previous boot (here: the forced TrustZone shutdown the step
10 teardown reports) leaves behind. The strncmp that crashed was not comparing a
registry name: `b01c1f20` is the ordinary `strncmp(r0, r1, r2)` (r3 takes the
incoming r0; the packet's r0 = 0 is the return value), and its 38 call sites in
slpi.b04 include the DAL device-name compares and the SMP2P entry-name walk --
the frame is the sensor library asking its own signal service to register the
entry, which did not exist. That corrects the closing paragraph above: the empty
registry was a symptom of the crashed framework, not its cause.

With `sns=1` the same run answers the three SUID reads with **result 0** and
does not print a single `result 5` (`logs/162659-run.log`), where the upload run
before the entry answered every message 5 (`logs/160654-run.log`).

What the entry did *not* change is the registry write path. The 189 groups still
answer 5 when they are sent at +1.2 s after the DSP's start
(`logs/163203-run.log`), and message 1 with the accelerometer's phone SUID is
still answered 5 in the same position (`logs/163822-run.log`). The sensor
framework is no longer dead, but the client manager's instance is not up yet
when the steps reach it; the driver now repeats message 1 (`sns_client_tries`)
and the whole group set (`sns_upload_rounds`) and stops at the first result 0,
which is the next measurement.

That measurement, on a boot short enough that the loader kept the whole log
(`logs/170433-run.log`): eight `sns_cm` writes over sixteen seconds and four
client requests, and not one result 0 ("the client request was never answered
0"). So the client manager is not created in the first twenty seconds. The
189-group upload answers 5 the same way at +1.2 s whether the framework has the
entry or not, and its 3103-byte group is rejected at the QMI layer (result 1,
error 19, `logs/164410-run.log`); a full set sent in one burst does not change
the outcome.

The other thing the same boot shows is that the sensor PD does not stay up on
its own: about two minutes after its start its own user watchdog ends it
(`EF:sensor_process:0x1:TMR_CLNT_1:...:dog_virtual_user.c:240:U`, then
`irq115:...:sns_err_handler.c:180:Crash`) with or without the upload. servreg
restarts the PD and it comes back with its services re-announced; the restarted
process accepts the stop when the run ends (`logs/165848-run.log`: "THE SENSOR
DSP ACKNOWLEDGES THE STOP after 20.6 ms").

The DSP makes no apps_std call at all in any boot after the entry (the count is
0 in `logs/162243-run.log`, `logs/162659-run.log` and `logs/165017-run.log`), so
its registry parser -- `sns_registry_parser.c`'s "Config dir '%s' not
available" and "Unable to open '%s' %i (%s)" -- is not reached, and the file
service is not what holds the registry up yet. The framework blocks before the
registry parse, and its user-PD watchdog ends it; what it waits for is the next
thing to find.

Two more candidates were tried and both are negative:

- **The restarted PD.** `sns_start_delay=150` waits past the first watchdog
  restart and probes the new instance. `logs/171852-run.log`: message 1 to the
  registry's own SUID and to the accelerometer's were "never answered 0", the
  write loop got nothing back, and the sensor process was already in its
  crash-and-restart cycle (four restarts in that run, each ending in
  `sns_err_handler.c:180:Crash`).
- **The AP's sleep state.** The phone's `smp2p_sleepstate` driver keeps
  `SLEEP_STATE_WAKEUP` (bit 1) in the `sleepstate` entry while the AP is awake,
  and a sensor that only acts on changes would never see one from a constant
  0. `slpi_sleep_value` (default 2) is written at creation and announced once
  with a 0 -> value transition and the SMP2P doorbell after the DSP is up
  (`logs/173017-run.log`: "the AP's sleep state is 0x2 ... and the DSP has its
  doorbell", at +147 ms; the DSP still adds its `sleepstate_see` at 147 ms).
  The hang is unchanged: the watchdog fires 77 s after the start
  (`+77075.967 ms: signal 0 ...`, `TMR_CLNT_1:...:dog_virtual_user.c:240:U`)
  and the restarts repeat the pattern.

So the block is inside the framework's own start-up and not in anything the
AP says through SMP2P. The next probe has to see the DSP's own log; its diag
router was asked for its channel (`DIAG`) but sends nothing, so either the
channel's log mask has to be configured (the AP's diag daemon does that on the
phone) or the framework's start-up chain has to be read from `slpi.b15`.

### The listener's order was the gate (2026-11-12, later)

The 77 s hang was the registry parser's first file call with no listener to
answer it. The driver had been running `fastrpc_restart=4`, whose chain never
opens `adsp_default_listener` on the DSP. The bare/middle order
(`fastrpc_bare=1 fastrpc_attach_mid=1`) opens it before the sensors PD exists,
and the DSP refuses that open: `logs/181928-run.log` logs
`> adsp_default_listener ... sc 0x20200` answered `retval -2147482572`
(`0x80000434`, invalid process), and the run still ends in the watchdog. The
phone daemon's own order does what the DSP expects: attach the sensors PD
(`INIT_ATTACH_SNS`), `adsp_listener_init2` on the fixed handle 3, **then**
open `adsp_default_listener` (retval 0, handle `837051616`), register it, and
only then wait in `next2` (`fastrpc_phone=2`).

With that order and the sleepstate entry the sensor process lives:

- `logs/182550-run.log` (`fastrpc_phone=2 fastrpc_pd=2 fastrpc_smmu=2
  fastrpc_call=9 fastrpc_early=1 fastrpc_grow_kb=4096
  fastrpc_grow_after=1`, `sns=0`) has **zero protection-domain failures and
  zero watchdog lines** through its 110 s, and the DSP acknowledges the stop
  in 20.5 ms when the run ends;
- the DSP's nested calls arrive and are served: it opens the `apps_std`
  module on handle 0 (`sc 0x20200`), then `fopen_with_env` (`0x13050100`),
  `fread` twice (`0x4010200`) and `fclose` (`0x3010000`) on handle 1, and the
  driver serves `/fw/sensors/soc0/hw_platform` (6 bytes, fd 100) -- the
  framework's first file read, the fusion sensor's platform query.

So the framework reaches and uses the file service; the gate that remains is
the run's length (the registry parser's own reads come later) and the SNS
client path. The first run with the messages on top of the live framework
(`logs/183050-run.log`, same arguments plus `sns=1`, message 1 and the
`sns_cm` write) dies differently: the DSP's **`frpc_dsp`** thread takes an
exception at `PC=0xb20c2dac` (`slpi.b20`: segment vaddr `0xb2062b80`,
offset `0x6022c`) and the err handler follows
(`irq115:0x1072:sns_err_handler.c:180:Crash`); its two SNS answers are
result 5.

That crash is **not** the SNS path's, and the correction is the important
part. Each half is clean alone (`logs/183840-run.log` the `sns_cm` write
only, `logs/184512-run.log` message 1 and the config only: zero
protection-domain failures), and a `sns=0` run of the same **150 s** length
crashes at the same `PC=0xb20c2dac` (`logs/185755-run.log`, and the combined
run reproduces it, `logs/185125-run.log`). Its ktime is `[271-275 s]`,
after a 150 s run -- the teardown, not the run -- and the shape is the
listener's `next2`: the call waits 120 s, times out, and the driver issues
another one, so at the stop the DSP holds two calls on one thread and its
frpc thread dies in a `trap0` return stub next to the libc string routines
(`0xb20c2da8`/`0xb20c2dac`). The 110-120 s runs never cross the timeout and
stop cleanly.

The listener now stops at its first timeout instead of re-issuing
(`hakios.thyme_slpi.c`, `fastrpc_listener_thread`), and the 150 s run has
zero protection-domain failures and a clean 20.6 ms stop
(`logs/190531-run.log`). The cost is that a request arriving after two
minutes is not served until the listener is restarted; the framework's own
reads so far (the five nested calls for `/fw/sensors/soc0/hw_platform`)
happen well inside that window, so the next run can go longer and look for
the registry parser's own reads.
