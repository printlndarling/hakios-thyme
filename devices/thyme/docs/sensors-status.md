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

## Not working yet

- **No sensor data**: no `SNS_STD_SENSOR_EVENT` (1025) follows the accepted
  config. The stream is not created.
- **The SUID lookup (512) takes the DSP down**: its service reads the sensor
  attributes from the registry, which is empty here, and the sensor process
  crashes (`irq115`, then `EX:kernel:...sns_sig_remote_...`). A config to a
  known SUID does not.
- **The reverse FastRPC tunnel never receives a call**: no `apps_std` request
  (the DSP's file service) arrives in any ordering tried — the default chain,
  `fastrpc_early=1` (listener at the DSP's boot), the dual PD-0/PD-2 listeners
  (`fastrpc_phone=4`) with a direct-SUID config as well (`logs/225533-run.log`:
  both listeners wait, the config is accepted, not one reverse call), the late
  attach on its own client, the restart ordering, or the phone's own daemon
  order (`fastrpc_phone=2/3/6`). So the sensor process's registry read of
  `/vendor/etc/sensors/sns_reg_config` and the persist files (both served in
  the scene) never reaches the AP.
- **The client manager's instance** (`sns_client_manager.c`, which creates the
  per-processor info and needs the registry group `sns_cm`) evidently never
  comes up: the stream cannot start. Its strings name the missing group and the
  heap it would allocate.

## The next pieces, in order

1. **Make the reverse tunnel work**: the DSP's `apps_std` calls must reach the
   listener's `next2`. The phone's userspace opens `adsp_default_listener`,
   which attaches the sensors PD and then calls `adsp_listener_init2` and starts
   `next2`; the register (`sc 0`) comes after. Our chain registers first, then
   init2, then next2 — matching the phone's *order* of init2/next2/register is
   the one untried difference.
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

3. **Get the DSP's own log**: `sns_diag_fw_log` exists in the SNS diag sensor
   (`diag_sensor`); the HAL's `get_qmi_debug_flag` (in `/vendor/lib64/libssc.so`)
   reads `persist.vendor.sensors.debug.ssc_qmi_debug`, and setting it true does
   not change the HAL's traffic at all (the same 65 lookups and 32 disables,
   `init_sensors.strace` from the 23:28 restart): on this firmware the flag is
   a libssc logging switch, not a way to reach the DSP diag sensor.

The registry was also probed directly (`sns_registry_probe=1`): a read_req for
`sns_cm` addressed to the SUIDs the name would produce in the three spellings
the observed SUIDs use (`registry` NUL-padded, and `registry________` /
`sns_registry____` with underscores) is answered result 0 by the transport but
produces no read_event (`logs/223806-run.log`), so the SNS client service ACKs
any SUID and the probe cannot prove which spelling (if any) is the registry's.
