# Sensors on a Qualcomm phone: the SLPI, FastRPC and SEE

How the S10 Lite (SM8150) port gets its accelerometer, gyroscope, magnetometer, light and proximity sensors with no
Android: the sensors DSP (the **SLPI**) is started by our own loader and kernel, and the kernel speaks the sensors
framework's protocol (**SEE**) itself. Tests 204–231 in `ports/r5q/wifi/WIFI-PLAN.md` have every step; this page is the
method, with where each value comes from on another phone.

## The chain, in the order it has to happen

| # | Step | S10 Lite value | Where it comes from on another phone |
| --- | --- | --- | --- |
| 1 | The SLPI's firmware loaded by the EFI loader and **held** (not released yet): `slpi=5` → start 3 | PAS id 12, carve-out 0x8c400000 + 0x1400000, firmware `slpi.mdt` from the vendor partition | the SLPI PIL node: `qcom,pas-id`, `memory-region`, `qcom,firmware-name` |
| 2 | The sensors' supply voted on through RPMh (a VRM write, never a PMIC write) | `ldoc8` (PM8150L LDO8, 1.8 V) | `sensor_vdd-supply` of the PIL node → the regulator → its cmd-db name |
| 3 | SMP2P: our outbound entry made (master-kernel 0, sleepstate 1) and the doorbell rung | SMEM items 481 (in) / 430 (out, ours), APCS IPC 0x17c0000c bit 26 | `qcom,smp2p-dsps`: `qcom,smem`, `qcom,ipc` |
| 4 | FastRPC's SMMU stream matches set before the DSP runs | SIDs 0x5a1–0x5a3 | `msm_fastrpc_compute_cb*` with `label = "sdsprpc-smd"`: `iommus` |
| 5 | FastRPC's shared buffer allocated where the DSP's session can reach it | 16 KB at 4–8 GB physical (bits 32–33 = session 1) | trial: an address the DSP rejects gives `AEE_EBADPARM` (0x8000040e) at attach |
| 6 | The SLPI released: TrustZone `auth_and_reset(12)` | — | the PAS id |
| 7 | GLINK to the SLPI, channels `IPCRTR` and `fastrpcglink-apps-dsp` | host 3, doorbell APCS IPC bit 24 | the `dsps` GLINK edge: `qcom,remote-pid`, `mboxes` |
| 8 | FastRPC: attach to the sensors PD, open and register `adsp_default_listener`, `init2`, then the `next2` loop | PD 2 | same on every SM8150-class DSP |
| 9 | The listener serves the DSP's file calls (`apps_std`): the sensor registry and configs | 127 files, 93 KB | the phone's `/mnt/vendor/persist/sensors/registry/registry/*` and `/vendor/etc/sensors/*` |
| 10 | QRTR, then the SEE QMI service: SUID lookups, then streams | service 400 | same |

Order matters:

- **The SMMU before the DSP runs.** Pick the *highest* free SMRs for FastRPC. Wi-Fi takes the lowest free ones, and
  two drivers picking the same slot broke Wi-Fi (Test 230). Copy the S2CR of a bank that already runs with
  translation off; a blind copy of SMR0's hung the core (Test 205). Any other driver that adds stream matches later
  (the microphone's SLIMbus code) takes a third range (highest free below 80).
- **Release from the kernel, not the loader.** With the SLPI released by the loader, it ran but never called us. The
  loader now only loads it (`start 3`), and the kernel releases it after steps 2–5.
- **The supply first.** Without `ldoc8` every sensor answered "not available" (SEE error event 130, value 6).
- **The listener open is retried** (30 tries, 500 ms): the first open comes too early and fails with a bad
  parameter.

## The file server (step 9)

The SLPI reads its whole configuration through FastRPC calls to the host (`apps_std` handle 1): `fopen`, `fread`,
`fseek`, `fstat`, `opendir`/`readdir`, `getenv`, `fwrite`. The kernel answers them from a table compiled in:
`redwood_sensor_files.h`, made on the PC by `ports/r5q/wifi/mk_sensor_files.py` from:

- the persist registry (`persist/sensors/registry/registry/*`: per-sensor calibration and settings),
- the vendor configs (`vendor/etc/sensors/sns_reg_config`, `hals.conf`, `config/*.json`),
- `/sys/devices/soc0` values (`hw_platform`, `soc_id`, `platform_subtype(_id)`, `platform_version`) and the
  sensors' `ssc_hw_rev`, which the configs are matched on.

**This file is device data and vendor files: generate it locally, never publish it** (the export skips
`*sensor_files.h`). Paths are normalised (`..`), and writes go to an in-memory layer (160 × 4 KB): the SLPI writes
its registry back and refuses to go on if writes fail.

Once a boot the SLPI refused one of our answers (`next2` answered 0x8000040e) and the sensors never came up. The
listener now sends the same answer twice more, then a plain failure for that one call, and only stops after five
refusals in a row (Test 237).

## SEE: the sensors framework (step 10)

- QMI service 400 over QRTR; messages are protobuf, encoded by hand (`see_*` in `redwood_adsp.c`).
- Look each sensor up by its data type (`"accel"`, `"gyro"`, `"mag"`, `"ambient_light"`, `"proximity"`): the
  answer is a SUID (two 64-bit words). Then enable a stream on that SUID:
  - accel and gyro at 25/s, mag at 10/s (sample-rate config);
  - light and proximity "on change" (message 514).
- Events arrive as indications. Route them by SUID. Proximity can come as message 1024 or 769.
- Units as Android: m/s² (accel), rad/s (gyro), µT (mag), lux. The kernel keeps the latest of each plus counters
  in `kern.redwood.motion`, and a 32 KB `slpi:` log in `kern.redwood.slpi_log` (read with `r5q_sysctl`).

## Checking a new phone

1. `slpi_log` shows each step's outcome (`slpi: frpc:`, `slpi: see:`). Compare it with the S10 Lite's in WIFI-PLAN.
2. If FastRPC attach fails with 0x8000040e, the buffer's physical address is the first suspect.
3. If the SLPI makes no calls, check the release order and the listener open.
4. If every lookup answers "NO SENSOR", the registry files did not reach it. Look at the `call N from the SLPI`
   lines for paths it asked for and did not get.
5. If a sensor reports "not available", its supply is off.
6. The raw magnetometer carries the phone's own hard-iron offset (hundreds of µT). That is normal: see
   [IOS-SENSORS.md](IOS-SENSORS.md) for calibrating it before iOS sees it.
