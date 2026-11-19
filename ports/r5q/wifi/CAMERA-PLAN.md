# Camera on the S10 Lite (from 2026-11-19)

Research: `CAMERA-FEASIBILITY.md` (how iOS 16.5 reaches its camera, the phone's camera hardware from the DT, the
candidate designs).

## Design (A): the donor's capture path, this phone's hardware

- **iOS side: already there.**
  - `libredwood-media.dylib` (injected into mediaserverd by the root build) gives CMCapture its own capture device
    (`camera_device.c`) and a software ISP (`camera_isp.c`).
  - It reads RAW10 frames from the kernel's `/dev/redwood-camera`.
  - iOS's own path would need Apple's ISP (`AppleH10CamIn`, H10ISP.mediacapture, about 94 user-client selectors,
    Apple's ISP firmware commands): not imitable in reasonable time.
- **Kernel side:** the donor's `redwood_camera.c` is compiled into our kernel (it was from the start). Its hardware
  path is the POCO X5 Pro's (SM7325). To port, for SM8150:
  - pins and regulators: two S2MPB03 regulator chips on bit-banged I2C, and PM8150 L14;
  - the MMCX rail;
  - the PLL type;
  - CSI-PHY v1.1 tables;
  - SMMU streams 0x840 / mask 0x620;
  - CSID-lite0 0xacc8000 and IFE-lite0 0xacc4000, the same addresses as the donor's.

## Safety

- (Kernel t267) The overlay copy of `redwood_camera.c` **refuses every `cam=` except `cam=5 cam_fake=1` on r5q**.
  Any other level would write the POCO's register addresses on this phone.
- The standard args have no `cam=`.

## Tests

- **Test 267 (kernel t267, root test267; `cam=5 cam_fake=1`):** the first milestone.
  - With fake mode, the driver serves a colour chart through `/dev/redwood-camera` and touches no hardware.
  - Its modes come from a synthetic `fw/camera-s5khm2sd.tab` (`pipeline/camfaketab.py`: 1920x1080 and 4000x3000
    RAW10, no register writes, no vendor data). `build_root_app.sh` makes it into `../fw-camfake`.
  - Expected: Camera.app shows the chart through iOS's real capture graph.
  - **Result:**
    - The kernel side came up: "cam=5 cam_fake=1: the wide (S5KHM2SD) camera's frames for iOS are a chart made
      here; nothing of the phone is touched". The synthetic table loaded (2 modes, 5 empty tables), and
      `/dev/redwood-camera` serves mode 1, 4000x3000.
    - mediaserverd's injected capture device registered **two streams, back and front**, and the graph started
      them.
    - **Camera.app opens with its full interface** (modes, shutter, 1×, switch camera) and the green in-use dot,
      **but the preview is black.**
    - mediaserverd logs "no /dev/redwood-camera (Input/output error): no frames". **The open returns EIO even for
      root** (new tool `wifi/devread.c`: `/var/r5q/bin/devread PATH [BYTES] [r|rw]`). The node is major 20 and does
      not clash with the others (torch 19, haptic 21, Bluetooth 22).
    - The driver returns EIO only when `broken` is set, yet its "nothing more is written" line never appears. Its
      `say()` prints to the console only while interrupts are enabled.
- **Test 268:** `give_up` keeps its reason and `camdev_open` logs every open and every refusal.
- **Test 268 result:** the open works.
  - The back camera's chart streamed ("3 done, 2 given"). Then iOS switched to the front camera at once, and the
    driver had no `camera-ov16a1q.tab`. It gave up for the rest of the run ("a sensor that cannot be used"), so
    every later open got EIO.
  - In Test 267 that line was never seen because of the log's timing.
- **Test 269 (kernel t268, root test269: synthetic tables for all four sensors the driver knows): THE CAMERA APP
  SHOWS FRAMES.**
  - The first cycle's flash was refused ("a kernel is running"); a second `cycle` flashed it.
  - The driver served the back chart, then the front chart (OV16A1Q's order), 22 frames to the preview.
  - The injected library: "PrimaryScaler: the first picture given (1504x1128 420f)".
  - **Camera.app shows the colour chart live** (white, beige, red, yellow, green, cyan, blue and grey bars, and
    the grey ramp) through iOS's own capture graph.
  - Camera asked for location, and Photos for notifications. Both were answered "Don't Allow" (the
    privacy-preserving choice).
  - **The shutter:** pressed once. The thumbnail stayed empty, and the Photos library looked empty behind an
    "Apple Music & Privacy" notice (left for the owner to answer). The still-picture path is still open.
  - `cam=5 cam_fake=1` is not in the standard args: it is a synthetic source, not the phone's camera.
- **What the iOS side needs is proven. Next is the hardware:** port `redwood_camera.c`'s path to SM8150 (see the
  design above). The first step is a read-only look (`cam=1` equivalent, rewritten for this phone) at the camera
  regulators (S2MPB03 over bit-banged I2C) and the CCI buses.
