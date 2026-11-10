# Reading the SLPI firmware (the sensor registry question)

The reverse tunnel reaches the DSP and serves its `apps_std` calls: the module
open, `fopen_with_env`, two `fread`s and `fclose`, once, without a retry
(`logs/082231-run.log`, `logs/175050-run.log`). The file they read is
`/sys/devices/soc0/hw_platform`, and the code that asks for it is the **fusion
sensor** (`sns_fusion_sensor.c`), not the registry parser: the disassembly of
the routine that builds that path sits next to `sns_fusion_sensor_init acc:
%f, mag: %f, gyro: %f` and `name=%s` (vaddr 0xb22f77xx in the image below).

What the DSP never does in these boots is run its registry's file parser: no
`/vendor/etc/sensors/sns_reg_config`, no `/vendor/etc/sensors/config`, no
`/mnt/vendor/persist/sensors/registry/...` call ever arrives at the listener.
That parser exists (`sns_registry_parser.c`, `sns_registry_osa.c`), and the
strings below say what it would do. The remaining question is what starts it —
the registry is a sensor in SEE (`sns_registry_sensor.c`, the `SNS_REG_INIT`
name), and its init/parse has not run by the time the system is up.

## The image

The SLPI payload in `fw-slpi/` is a Qualcomm split ELF: `slpi.b00` holds the
ELF header and the program headers, and `slpi.b01..b20` are one blob per
program header. The parts are **not** the file laid end to end: each belongs
at its header's `p_offset`, with the gaps zeroed. `cat slpi.b00 slpi.b01 ...`
puts every part after the first at the wrong offset — the header still parses,
but from the second blob on the disassembly is shifted bytes, not the
firmware. `tools/slpi-split.py` reassembles it correctly:

```sh
tools/slpi-split.py fw-slpi -o /tmp/slpi.img
llvm-readelf -h /tmp/slpi.img      # Qualcomm Hexagon, entry 0x88000000
llvm-objdump -d /tmp/slpi.img > /tmp/slpi.dis
```

`llvm-objdump` in this environment is Hexagon-capable (`clang --print-targets`
lists it), so the code can be read without vendor tools. The image has 22
program headers; the code lives in the `R E` segments at vaddr 0xb0152000
(0x1f88ec bytes), 0xb2200000 (0x1f7344), 0xb0031000 (0x1b31c), 0xb0000000
(0x712c), 0xb2062b80 (0xac244) and 0xb0100000 (0x142b8). The disassembly
prints the `p_vaddr` space, data pointers inside the image are 32-bit vaddrs
of that same space (`0xb2xxxxxx`), and code reaches strings with `rN =
##0x...` immediates — search the disassembly for a string's address, not the
binary for a little-endian pointer.

Useful strings and their file offsets (`strings -t x /tmp/slpi.img`; the
address is the segment base plus the offset into it): `fastrpc_shell_2`,
`fastrpc_tracer`, the QMI listener messages (`REGISTER_LISTENER_REQ failed`,
...), the SNS error handler texts seen in the DSP's last words
(`sns_err_handler.c`), and the registry block at 0x690000-0x691b00:
`sns_registry_parser.c`, `registry config file '%s' is not present`,
`no update of sns_reg_config version`, `skip loading properties file '%s'`,
`skip loading output directory '%s'`, `skip loading config directory '%s'`,
`/vendor/etc/sensors/sns_reg_config`, `sns_registry_osa.c` and its
fopen/fread/fwrite/readdir errors.

## What to find

1. **What runs the registry parser.** The parser starts with the config file's
   path (`r0 = ##0xb23c4aa8` at 0xb22d369c) and its messages name the whole
   flow: present-check, version compare (`no update of sns_reg_config
   version`), properties file, output directory (mkdir/stat), config
   directory, per-file `file=` entries. It has no direct `call` site; it is
   reached through the sensor API tables (a pointer to the `SNS_REG_INIT`
   routine at 0xb22d2b04 sits at file 0x4cb9c8 and 0x6f2008). Finding the
   framework's sensor-instantiation path — which sensors start at boot and
   what instantiates the registry sensor — is the next step.
2. **The AP-side `sns_registry` module.** `libsns_registry_skel.so` (pulled
   from the phone) implements exactly one method, `get_property(name, value,
   len)` over Android's `property_get`: the listener call carries a scalar
   blob `{name length including NUL, value length}`, the name as the second
   in buffer and the value as the first out buffer (sc 0x00010200, method 0).
   The parser's `property=` lines (`persist.vendor.sensors.enable.property`,
   ...) resolve through it. The DSP never opens that module in our boots; the
   driver answers every `remotectl_open` with the apps_std handle, so if it
   ever does, that call needs its own handle and this layout.
3. The DSP-side FastRPC kernel (`adsp_listener`/`apps_std` dispatch) is no
   longer suspect: the replies match the official `listener_buf.h` contract
   and the sequence completes without a retry.

## Alternative that may come first

The DSP's own log through diag (`sns_diag_fw_log`) would say why the registry
sensor does not start; `sensors-status.md` records that the HAL's
`ssc_qmi_debug` property is only a logging switch, so the diag path needs its
own client (QSHPD subscription over QRTR) — also a project, but a smaller one
than reading the firmware. One fact from the service lists (10 November): the
SLPI announces only 15, 24, 43, 51, 66 and 400 over QRTR
(`logs/221040-run.log`, `logs/221918-run.log`) — **no diag service** — so a
diag client would have to reach the ADSP's diag stack, not the SLPI's own,
which makes the firmware route the more direct one of the two.
