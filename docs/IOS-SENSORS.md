# Sensors in iOS: virtual HID services, orientation, brightness, CoreMotion, compass

The kernel has the samples ([SENSORS.md](SENSORS.md)). This page is how iOS 16.5 (iPhone10,3 system image) is made
to use them, on a phone with none of Apple's sensor hardware (no motion coprocessor, no Apple ALS). All of it is in
`ports/r5q/wifi/inject_bbd.c` (a library inserted into backboardd) plus three small patches to Apple code made at
run time ([SYSTEM-PATCHING.md](SYSTEM-PATCHING.md)). Tests 220–243 in `ports/r5q/wifi/WIFI-PLAN.md`.

## 1. Virtual HID sensor services (in backboardd)

A thread in backboardd polls `kern.redwood.motion` every 40 ms and turns new samples into HID events on virtual
services that look like an iPhone's built-in sensors:

| Sensor | Usage page / usage | Event | Units and axes |
| --- | --- | --- | --- |
| Accelerometer | 0xff00 / 3 | `IOHIDEventCreateAccelerometerEvent` | g; iOS = −Android ÷ 9.80665 on each axis |
| Gyroscope | 0xff00 / 9 | `IOHIDEventCreateGyroEvent` | rad/s, Android's axes as they are |
| Compass (magnetometer) | 0xff00 / 0x0a | `IOHIDEventCreateCompassEvent` | µT, Android's axes, **hard-iron offset removed** (§6) |

- The client is `IOHIDEventSystemClientCreateWithType(NULL, 4, NULL)`, a client that only sends.
- Services are made by `IOHIDVirtualServiceClientCreateWithCallbacks` with `PrimaryUsagePage/PrimaryUsage`,
  `DeviceUsagePairs`, `Built-In = true`, `Transport = "SPU"`, `Product`.
- Each event gets integer field 0x4 ("built-in") = 1.
- Keep a service even when nobody opens it at once. After the CoreMotion patch (§4), backboardd's own CoreMotion no
  longer opens the accelerometer immediately. Giving up after 20 tries left the phone with no services at all.
- iOS sets `ReportInterval`/`BatchInterval` (µs) on the services. Ours ignore them and send at the DSP's rate.
  Measured in an app (`cm_rate.m`, one sensor alone): accel and gyro about 23/s, mag 10/s. That is enough.
- **Light (0xff00/4): don't.** CoreBrightness builds a model of an Apple ALS from calibration properties and
  dereferences it, so backboardd crashed on every start (Test 228). Brightness is done another way (§3).
- **Proximity is read but not given to iOS on purpose.** A wrong "near" turns the screen and touch off (pocket
  mode).

## 2. Device orientation (rotation)

backboardd asks CoreMotion for orientation updates (`-[CMDeviceOrientationManager
startDeviceOrientationUpdatesToQueue:withHandler:]`). On this phone CoreMotion never produces them, so the method is
swizzled:

- keep the queue and the handler;
- take CoreMotion's own `CMDeviceOrientation` object from `deviceOrientationBlocking`, and set its `fOrientation`
  ivar (an int, found by name at run time);
- call the handler on the given `NSOperationQueue` with `addOperationWithBlock:`.

The orientation comes from the accelerometer:

- |z| > 0.8 g → face up or face down;
- otherwise the larger of x and y, with a 0.2 g margin;
- a new orientation is sent once it has held for 5 samples.

Two things crashed backboardd and must be avoided: a `CMDeviceOrientation` of our own, and `dispatch_async` onto an
`NSOperationQueue`. Verified by hand: rotation works.

## 3. Auto-brightness without an Apple light sensor

`BackBoardServices`' `BKSDisplayBrightnessGetCurrent` / `BKSDisplayBrightnessSet(level, 1)` (dlsym, no
underscore), driven from the light sensor every 40 ms:

- the lux smoothed over about 1.5 s on a log scale: `s = log10(lux + 1)`, level `0.08 + 0.23·s`;
- the goal changes only when the light calls for a level more than 0.05 away (a dead band: no hunting);
- the screen eases towards it: 6% of the way per step, at most 0.006 per step, and stops within 0.002;
- if the brightness moves more than 0.04 away from what we set, the user moved the slider. Automatic changes then
  wait until the light changes about fourfold;
- off when `com.apple.backboardd`'s `BKEnableALS` is false.

The first version stepped 0.05 once a second and re-targeted on every flicker. The owner reported "not smooth,
doesn't settle". The values above are what they confirmed as good.

## 4. CoreMotion device motion (attitude, gravity, user acceleration)

CoreMotion fuses device motion **in the app's own process**. Which fusion it runs comes from
`CLPlatformInfo::motionCaps()`, a fixed table per hardware model (`queryMotionCaps`). For iPhone10,3 it says the
motion coprocessor (SPU) does the fusion:

| Bit | Meaning | Effect on this phone |
| --- | --- | --- |
| 3 | 6-axis fusion on the SPU | waits for an SPU HID service that never comes: 0 samples |
| 4 | 9-axis fusion on the SPU | the same |
| 5 | the SPU's compass (`CLCompass7`, which asserts bit 4) | north frame aborts (SIGABRT) |
| 7 | 3-axis fusion on the SPU | the same as 3 |

The fix clears bits 3, 4, 5 and 7 where `queryMotionCaps` stores the table, by changing the mask it builds:
`mov x9, #0xfeff` becomes `mov x9, #0xfe47`. CoreMotion then fuses on the application processor
(`CLSensorFusionService…AP`, an MEKF) from our accelerometer, gyroscope and compass.

Result in an app (`cm_test.m`): about 8 samples/s. Face down, gravity reads (−0.015, 0.016, 1.000) and roll −179°.
The magnetic-north frame runs.

**Finding it on another iOS build:** the string `CLPlatformInfo::MotionCapabilities CLPlatformInfo::queryMotionCaps()`
is used inside the function (`tools/ios/dsc_whose.py`). Near its end, the table is ANDed with a mask built by
`mov x9 / movk ×3`, ORed with one bit from a preference (`OverrideMotionCapEclipseService`), and stored. The bits'
meanings come from the assert strings `CLPlatformInfo::motionCaps().deviceMotion6Service` / `deviceMotionService` /
`deviceMotion3Service` and the `tbnz w0, #N` checks around them in
`CLDeviceMotion::enableSensorFusionWithMode`.

## 5. The compass (heading)

With bits 4 and 5 clear, the compass factory makes the application processor's compass `CLCompass6`. It does that
only if a second per-model check says the model has one, and iPhone10,3 has none. So there was **no compass object
at all**: no heading, no calibration, and the north frame drifting.

The fix patches the factory's call of that check to `mov w0, #1`. The check itself stays untouched, because it is
used in many other places.

- **CoreMotion:** factory at CoreMotion+0x2b60c0, the call at 0x191c450e8.
- **locationd's own copy** (Compass and CoreLocation headings come from locationd): factory found from
  `CLCompass7 requires deviceMotionService` → the CLCompass7 constructor → its one caller → the factory. The call is
  at file offset 0x6e4360. locationd's own `queryMotionCaps` mask is patched the same way, at 0x3b6e38.

Status (Test 243):

- The Compass app showed a real heading for the first time, then it froze.
- CoreMotion in an app still reports the calibrated field as uncalibrated (accuracy −1).
- Where the owner tested, the field's tilt measured −84° against about −67° expected for the area. That leaves only
  about 5 µT horizontal: likely indoor interference, which iOS checks against the World Magnetic Model.
- Next: test outdoors; if it still freezes, read locationd's `CLCompassCalculator` interference checks.

## 6. Magnetometer calibration (hard iron)

The raw field includes the phone's own magnets. On this phone the offset is (−95, 39, −426) µT, against an Earth
field of about 50 µT. Android removes it in the DSP (`sns_mag_cal`). Here backboardd removes it before iOS sees the
field:

- it starts from an offset measured once per phone, by a figure-8 recording (`magrec.c` → `tools/ios/magfit.py`);
- it refines the offset with an online sphere fit over up to 48 samples spread at least 12 µT apart;
- it only accepts a fit with radius 25–70 µT and residual under 3 µT.

`magfit.py` also checks:

- **units:** the radius should be about 25–65 µT. The S10 Lite gave 50.5;
- **axes:** the angle between field and gravity should hold steady. The script tries all 48 axis permutations and
  sign combinations, and the identity was best here;
- the factory correction matrix, which made no improvement here.

## Tools (in `ports/r5q/wifi/`, built with the iOS SDK, run over r5qd)

| Tool | What |
| --- | --- |
| `cm_test SECS [north] [log]` | CoreMotion as an app sees it: availability, samples, device motion (frame, gravity, attitude, heading, calibrated field). `log` re-executes itself with `OS_ACTIVITY_MODE=debug OS_ACTIVITY_DT_MODE=1` to print CoreMotion's own messages |
| `cm_test peek ADDR…` | the shared cache's words at unslid addresses, to see a patch is really there |
| `cm_rate a\|g\|m INTERVAL SECS` | the delivered rate of one sensor |
| `cl_test SECS --heading` | CoreLocation headings (a bundle-less tool gets none: authorization 0) |
| `orient_test` | backboardd's orientation as BKHIDServices reports it |
| `magrec SECS` | raw magnetometer + accelerometer from the kernel, for `magfit.py` |
| `r5q_sysctl NAME` | any sysctl's bytes (`kern.redwood.slpi_log`, `kern.redwood.motion`) |
