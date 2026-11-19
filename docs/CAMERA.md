# Camera: a Qualcomm CAMSS sensor into iOS's Camera app

Proven on the Galaxy S10 Lite (SM8150, Titan 175, Samsung S5K3L6), Tests 267-292. The detailed log is
`work/ios-r5q/wifi/CAMERA-PLAN.md`. The register-level spec is `work/ios-r5q/wifi/CAMERA-RX-SPEC.md`. This page is the
reusable recipe: what to gather from a new phone, in what order to bring the pieces up, and what bit us.

## The path

```
sensor (I2C/CCI: power, ID, init + mode tables, exposure, stream on)
  -> MIPI D-PHY lanes -> CSI-PHY n -> CSID-lite (RDI0, RAW10) -> IFE-lite write master
  -> SMMU (one stream match, bypass) -> RAM below 4 GB (the loader's camera block)
  -> the kernel's frame ring -> /dev/redwood-camera -> mediaserverd's camera vendor (camera_device.c) -> Camera.app
```

The iOS side (the `/dev/redwood-camera` session, the mediaserverd vendor, the synthetic-chart test) was proven
first with no sensor at all (`cam=5 cam_fake=1`, Test 269). Do that first on a new phone: it separates "iOS takes our
frames" from "the hardware gives frames".

## What to gather (from the live Android device tree and the vendor's camera files)

| What | Where it comes from | S10 Lite |
|---|---|---|
| Which sensor sits where, its I2C address and chip ID | `qcom,cam-sensor@N` nodes; the module file's slave info | `cam-sensor@2` = S5K3L6, 7-bit 0x2d, ID 0x30c6 at 0x0000 |
| The bus | `cci-master`; if the CCI is unusable, the pins to bit-bang | CCI 1 master 0 = GPIO31/32 (bit-banged) |
| Supplies | `cam_vana/vdig/vio-supply` → a PMIC LDO or an RPMh regulator | S2MPB03 #2 LDO6 2.8 V, LDO2 1.05 V; L14 via RPMh `ldoa14` |
| MCLK | `clock-names` "cam_clk" → camcc MCLKn RCG + branch, and its pin | MCLK1: RCG 0x5024 (TCXO), branch 0x503c, GPIO14 |
| Reset / standby GPIOs | `gpio-reset`, `gpio-standby` | GPIO22 |
| CSI-PHY index and lanes | `csiphy-sd-index`, `lanes` in the module file | CSI-PHY 2, 4 lanes, settle 14 |
| Init / mode / stream tables | the vendor's sensor module file (Chromatix: V1.4.0 uses 64-bit fields) | `pipeline/camsensor_v14.py` → `camera-<sensor>.tab` (kept local, never published) |

## Bring-up order (each step its own test; log a line after each)

1. **Rails and the camera clock controller.** Vote MMCX and MX (RPMh levels) *before* touching camcc: camcc lives
   on MMCX. Then CAM_CC_GDSC_CLK on and titan_top_gdsc powered (GDSCR bit 0 cleared, wait for bit 31).
2. **Sensor power, MCLK, reset, then the ID read.** If the bus answers nothing, check the MCLK RCG is really
   rooted (an RCG left "root off" = no clock) and the address is 7-bit.
3. **Tables:** init, then the mode table, then your own exposure (SMIA 0x0202 coarse integration, 0x0204 gain) and
   frame length (0x0340) for the frame rate you want, then stream on.
4. **Receive:** camcc PLL0 locked, the RCGs and branches, CSI-PHY (lane enable, settle), CSID RX + RDI0. Count
   packets and ECC/CRC errors before going further.
5. **Memory:** the camera NoC clock, the GCC camera AXI clocks, an SMMU stream match with a bypass S2CR copied from
   a non-translating bank, an IFE-lite write master in frame mode, a buffer below 4 GB. Fill the buffer with a
   pattern first, so "bytes changed" proves the write.
6. **The session:** ping-pong between two buffers, register update per frame, hand each finished frame to iOS.

## Lessons (each one cost at least one hang or one test)

- **Park every RCG on the crystal before turning its PLL off** (what Linux's `clk_rcg2_shared_ops` does). The S10
  Lite's teardown turned camcc PLL0 off while five RCGs still selected it; the next bring-up's switch of the camera
  NoC's RCG (0xc170) then started from a dead source and stalled the whole SoC bus, at random, only at session
  transitions (Tests 290-326, 40 tests to find). Also enable `cam_cc_camnoc_dcd_xo_clk` (camcc 0xc194) with
  `camnoc_axi`. After the fix: six full power cycles in a row and minutes of streaming, no hang (Test 327).
- **Keep the pipeline up between sessions.** iOS opens, switches and closes the camera constantly; bringing the
  sensor and receive path up once and only starting/stopping the hand-over makes switches instant and avoids the
  power transitions entirely.
- **One bit-bang bus pointer per thread.** If touch is bit-banged too (GSI-only serial engines), a global "current
  bus" pointer lets a touch poll redirect a camera transaction mid-way, explicit-pin writes included.
- **Log so that a hang cannot eat the last lines:** a timestamped line before every bring-up/teardown step, and the
  kernel log streamed off the phone over Wi-Fi every ~50 ms (`klog -f` + `phone.py wlog`), not a 2 MB USB snapshot
  every few seconds.

- **slow_ahb must stay on TCXO.** Rooting the camera's slow AHB RCG on PLL0 hung the whole system about 3 s later,
  whatever came next (Tests 276-286).
- **An RPMh level vote replaces this voter's earlier vote, the boot's included.** "Releasing" MMCX/MX to 0 at
  teardown took camcc's power away (every camcc register read 0, the next session's sensor never answered), and
  probably caused a hang (Tests 290-291). Keep the votes, or put back the level you found.
- **Run the long probe in its own kernel thread, and don't hold a lock other threads need while frames flow.**
  Inside the touch thread, any probe longer than ~3 s hung. A streaming session that kept the battery's bus lock
  (which the touch thread takes every 5 s) stalled that thread the same way and took the phone down (Test 292).
  Hold the lock for the power-up and teardown only.
- **One mode for every select.** iOS's camera switch passes its own mode choice (4000x3000 for the front camera);
  clamp every select to the mode the hardware path actually streams and the buffer actually fits.
- **Don't reuse another phone's pin tables.** The donor's `find_pins()` data-aborted on this phone.
- **Stride:** the CSID writes RAW10 lines padded to 16 bytes (1028 pixels → 1296 bytes).

## Privacy and licensing

Never publish the vendor's sensor module files, the tables derived from them, or the sensor registry. The tables
live in the local firmware folder and are copied into the test root at build time only.
