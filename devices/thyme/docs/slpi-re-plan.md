# Reading the SLPI firmware (next step for the sensor stream)

The reverse tunnel now reaches the DSP and serves its `apps_std` calls, but the
DSP re-opens its module and re-reads `/sys/devices/soc0/hw_platform` about
every two seconds without crashing. Everything that public sources define is
implemented (see `sensors-status.md`); the remaining question is what the
DSP's own `apps_std` checks before it accepts a reply, and that is inside the
firmware.

## The image

The SLPI payload in `fw-slpi/` is a Qualcomm split ELF: `slpi.b00` holds the
ELF header and `slpi.b01..b27` the segments. Concatenating them in order gives
a valid **Hexagon ELF32 EXEC**:

```sh
cd fw-slpi && cat slpi.b0* slpi.b1* slpi.b2* > /tmp/slpi.bin
llvm-readelf -h /tmp/slpi.bin     # Qualcomm Hexagon, entry 0x88000000
```

`llvm-objdump`/`llvm-mc` in this environment are Hexagon-capable (`clang
--print-targets` lists it), so the code can be disassembled without vendor
tools. There are 22 program headers; the main executable segments are at
0x88000000 (R E), 0x88052000 (R E, 1f88ec bytes) and 0x8a000000 (R E,
1f7344 bytes), with data segments after each.

Useful strings and their file offsets (`strings -t x /tmp/slpi.bin`):
`fastrpc_shell_2`, `fastrpc_tracer`, the QMI listener messages
(`REGISTER_LISTENER_REQ failed`, ...), and the SNS error handler texts seen in
the DSP's last words (`sns_err_handler.c`).

## What to find

1. The DSP-side FastRPC kernel (the `adsp_listener`/`apps_std` dispatch): where
   a reverse call's `sc`/method is decoded and where the reply's `prevbufs` are
   unpacked (`unpack_out_bufs`-like code), and what makes the `apps_std` fread
   reply acceptable.
2. The sensor process's registry read loop: what makes it re-open the platform
   file (a failed parse, a missing group, a size check).
3. The strings above locate the neighbourhoods by xref; the Hexagon
   disassembly then has to be read around them. This is the part REA cannot
   help with (it has no Hexagon backend), so `llvm-objdump -d
   --triple=hexagon` (optionally `--start-address/--stop-address`) is the
   tool.

## Alternative that may come first

The DSP's own log through diag (`sns_diag_fw_log`) would say why the read is
retried without any disassembly; `sensors-status.md` records that the HAL's
`ssc_qmi_debug` property is only a logging switch, so the diag path needs its
own client (QSHPD subscription over QRTR) — also a project, but a smaller one
than reading the firmware.
