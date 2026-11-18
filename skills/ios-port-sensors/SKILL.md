---
name: ios-port-sensors
description: Bring up a Qualcomm phone's motion and environment sensors in a native iOS port and make iOS use them - start the SLPI sensors DSP and talk FastRPC + SEE from the kernel, then feed iOS through virtual HID services (accelerometer, gyroscope, compass), orientation, auto-brightness, CoreMotion device motion and the compass. Use when sensors, rotation, brightness, CoreMotion or the compass don't work on a port, or when starting sensors on a new phone.
---

# Sensors: from the SLPI to CoreMotion

Two halves, each with its own page. Read the page for the half you're in before changing anything:

- **Kernel: the SLPI, FastRPC, SEE** → `docs/SENSORS.md`. The 10 steps, their order, and where each value comes
  from in a new phone's device tree.
- **iOS: virtual HID services, orientation, brightness, CoreMotion, compass** → `docs/IOS-SENSORS.md`.
- **Changing Apple code safely** (shared cache, daemons) → `docs/SYSTEM-PATCHING.md` and `tools/ios/`.

## Order of work on a new phone

1. Collect from Android (read-only):
   - the SLPI PIL node: PAS id, memory region, firmware name, `sensor_vdd-supply`;
   - `qcom,smp2p-dsps`, the `dsps` GLINK edge, and the `sdsprpc-smd` FastRPC SIDs;
   - `/mnt/vendor/persist/sensors/registry/registry/*`, `/vendor/etc/sensors/*`, `/sys/devices/soc0/*`.

   **The persist and vendor files are device data: they stay local** (the export skips `*sensor_files.h` and
   `work-sensors/`).
2. Loader: load the SLPI and hold it (`slpi=5`). Kernel:
   - the supply vote, SMP2P, FastRPC SMRs (highest free), the buffer, release, GLINK;
   - attach, the listener (retried), `init2`, `next2`, the file server, then SEE.

   Watch `kern.redwood.slpi_log`.
3. When `kern.redwood.motion` counts rise for every sensor, start the iOS side:
   - the services in backboardd;
   - orientation;
   - brightness. **Never a light HID service**: CoreBrightness crashes on it.
4. CoreMotion device motion needs the motion-caps patch; the compass also needs the compass-factory patch, in
   CoreMotion **and** locationd. Find both by their strings on your iOS build (`docs/IOS-SENSORS.md` §4–5).
   **Patch the page hash with the code, or the whole UI loses its entitlements** (Test 233: no touch).
5. Record a figure-8 with `magrec`, run `tools/ios/magfit.py`, and put the offset in the backboardd library.

## Rules learned the hard way

- One run per authorization; the iOS-side libraries can be swapped live with r5qd `reload` (no reboot), the
  kernel cannot.
- Keep a virtual service even if nobody opens it at once.
- SMMU stream-match ranges: Wi-Fi lowest free, FastRPC highest free, anything else a third range — never the same.
- Proximity stays out of iOS until there is a safe way to test pocket mode.
- Hand-check what software can't: rotation, brightness in real light, the compass outdoors.

## Where the S10 Lite stands (Test 243)

**Works:**
- accel, gyro, mag, light and proximity in the kernel;
- CoreMotion accel, gyro, mag and device motion in apps;
- rotation;
- smooth auto-brightness.

**Partly:**
- the Compass app shows a heading, then freezes;
- the field indoors is tilted −84° where about −67° is expected. Check outdoors next.
