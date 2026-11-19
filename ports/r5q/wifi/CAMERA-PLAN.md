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

## Hardware, step 1: the cameras' power (Test 270, kernel t270, `campmic=1`; `campmic_peek` in redwood_sec_ts.c)

- **Read only.** About 30 s after boot, in the battery thread under `r5q_pmic_lock`, the probe reads the two Samsung
  S2MPB03 regulator chips at 0x56 on bit-banged I2C:
  - #1 on GPIO94/95 (NORTH tile 0x03900000);
  - #2 on GPIO47/48 (EAST tile 0x03500000).

  Pins and tiles come from the DT's `i2c-gpio` nodes and Linux pinctrl-sm8150.c. Register 0x00 and 0x02–0x2f are
  read; 0x01 (STATUS / an interrupt latch) is skipped. The pins go back as found.
- **Both chips answer: ID 0x02.** The same register map for both:

  | Reg | #1 | #2 | Meaning (Samsung s2mpb03.h/.c: 0 ID, 1 STATUS, 2 CTRL, 3–9 LDO1–7_CTRL, 0xa–0xb slew) |
  | --- | --- | --- | --- |
  | 0x03–0x09 | 1c 20 2c 20 0e 28 28 | 1c 20 2c 20 0e 28 28 | LDO1–7: **bit 7 (enable) clear on every one: all off**; bits 5:0 the voltage |

  Voltage: LDO1, 2 and 4 = 0.7 V + n × 12.5 mV; LDO3 = 0.7 V + n × 25 mV; LDO5–7 = 1.8 V + n × 25 mV. For example,
  LDO3 0x2c gives 1.8 V. These are the reset values, not the DT's sensor voltages.
- Done in this test: the regulators are reachable and readable from our kernel, and their register map is known
  (Samsung's open-source driver, e.g. github.com/ianmacd/gts6lwifi drivers/regulator/s2mpb03.c).
- **Next (writes, the first of the camera port):**
  1. set and enable one sensor's LDOs as the DT gives them (AVDD 2.8 V, DVDD about 1.05–1.1 V, IOVDD 1.8 V);
  2. its MCLK (camcc) and its reset GPIO;
  3. read its chip ID over CCI;
  4. power everything down again.

  This is the donor driver's cam=2 step, rewritten for SM8150's camcc, CCI and pins.

## Hardware, step 2: one sensor powered, its ID read, all back (Tests 271-273, `camsensor=1`; `camsensor_probe` in redwood_sec_ts.c)

- **The sensor:** DT `qcom,cam-sensor@2`, slave 0x5A on CCI 1 master 0 (GPIO31 SDA / 32 SCL, NORTH; the DT's
  `cci2_active`), on CSI-PHY 2. It was picked because its MCLK1 is 19.2 MHz (TCXO, no camera PLL).
  - Supplies: vio PM8150 L14 1.8 V (RPMh `ldoa14`); vana S2MPB03 #2 LDO6 2.8 V; vdig S2MPB03 #2 LDO2 1.05 V.
  - MCLK1: camcc 0xad00000, RCG 0x5024 / branch 0x503c, on GPIO14 (NORTH, function 1 cam_mclk).
  - Reset: GPIO22 (EAST).
  - Its I2C is bit-banged on the CCI pins; the CCI block is not used.
- The probe goes 40 s after boot, under `r5q_pmic_lock`. Everything is put back as found afterwards (LDO registers
  read back to verify). The owner's standing OK for power changes covers it ("stop asking, just proceed": charger
  writes and power votes).
- **Test 271:** stopped at the guard. GCC_CAMERA_AHB_CBCR read 0x80000003 and the guard took bit 31 for "off".
  - It is enabled (bit 0) and hardware-gated (bit 1), which reads idle; Linux gcc-sm8150 has hwcg_bit 1.
  - The guard now requires bit 0.
- **Test 272:**
  - L14 read status 0x03, enable 0 before the vote. The vote was done but not read back.
  - **S2MPB03 #2: LDO6 0x28 -> 0xa8 (on, 2.8 V), LDO2 0x20 -> 0x9c (on, 1.05 V)**, and both back to 0x20 / 0x28
    afterwards: the regulator writes work.
  - **MCLK1 did not run:** RCG cmd 0x80000000 (root off) with cfg 1 (TCXO / 1), and branch 0x80000001 (enabled,
    off).
  - **No acknowledge from 0x5A**, at register 0x0000 or 0x0016. The sensor is not alive without its clock (and
    possibly its I/O supply).
- **Test 273:**
  - L14 is read again after the vote;
  - camcc's `CAM_CC_GDSC_CLK` (0xc1e4) is turned on and **titan_top_gdsc** (GDSCR 0xc1bc) powered before the MCLK;
  - both are put back afterwards.
- **Test 273 result:** the power chain is up.
  - L14: status 0x87, enable 0x80 after the vote.
  - titan_top_gdsc: 0x22f001 -> 0xf822f000 (powered).
  - **MCLK1 runs:** RCG cmd 0, branch 0x1 (on).
  - Still no acknowledge at 0x5A.
- **Test 274 (kernel t274): THE SENSOR ANSWERS.**
  - Qualcomm's camera DTs give the **8-bit** I2C address: `slave-addr = <0x5a>` is **7-bit 0x2D**. The same
    applies to the other sensors: 0x34 is 0x1A.
  - A read-only scan of the bus finds one device, 0x2D.
  - **Chip ID at 0x0000: 0x30C6.** That is Samsung's S5K3L6 (about 13 MP), very likely the ultra-wide camera
    (CSI-PHY 2).
  - Then everything went back: GDSC collapsed (0x7022f001), LDO2/LDO6 0x20/0x28, reset low, MCLK off.
- **Proven from our kernel:** the camera power chain (L14, S2MPB03 LDOs), the camera power domain, MCLK from TCXO,
  the sensor's reset, and its control bus. The working sequence is `camsensor_probe` (redwood_sec_ts.c).
- **Next, the data path** (the donor driver's cam=3/4 steps, rewritten for SM8150):
  1. the CCI block itself, or keep bit-banging for register tables: about 1000 writes at roughly 100 kHz takes
     seconds;
  2. the sensor's register tables. The S5K3L6's init and mode tables are Android's sensor module files
     (/vendor/lib64/camera on the S10 Lite): the vendor's, needed from the phone's own Android backup, never put
     in the repository;
  3. CSI-PHY 2 (v1.1 tables), CSID-lite / RDI, IFE-lite write master, SMMU;
  4. RAW10 frames into `/dev/redwood-camera`, where the iOS side is already proven (Test 269).

## The sensors' register tables (desk work, 2026-11-19)

- This phone's own vendor partition (the local dump `work-sensors/vendor.img`, never in the repository) has
  `/lib64/camera/com.samsung.sensormodule.<n>_<maker>_<sensor>.bin`:
  - `0_sony_imx586`: main, 48 MP;
  - `1_sony_imx616` and `8_sony_imx616_full`: front, 32 MP;
  - **`2_lsi_s5k3l6`: the sensor that answered in Test 274 (0x30C6)**;
  - `3_lsi_s5k5e9`: probably the macro or depth one.

  Extracted for local use to `$IOS_WORK/linux-source/fw-camera-r5q/` (the vendor's: never in the repository).
- **The format differs from the donor's.** Header "QTI Chromatix Header / Parameter Parser V1.4.0" (the donor's
  POCO files are V3.0.1), so `tools/camsensor.py` refuses it:
  - the entry table at 0xa0, count 3 (both from the word pair at 0x98), each entry {id, offset, size} as u64:
    symbols at 0xe8 (0xc9a8 bytes), data at 0xca90 (0x6470 bytes), a third block at 0x12f00 (0x18 bytes);
  - symbol records are **72 bytes**: {id u64, name[32], 4 x u64}. The donor's are 56 bytes with u32 fields.
  - The same symbol names as the donor's (sensorDriverData, powerSetting, resolutionInfo, regSetting,
    slaveAddr/registerData/delayUs ...), so the same tree, wider fields.
- **Next:** teach camsensor.py the V1.4 layout. Map the four u64 fields against the V3 ones (kind, parent, ref,
  size) and the regSetting element size, then make `camera-s5k3l6.tab` locally for the SM8150 data path.
- **Done: `pipeline/camsensor_v14.py`** reads the V1.4.0 module files (layout in its docstring: every field u64,
  symbols 72 bytes, register settings 80 bytes, resolutions 264 bytes with the fps a double at +80) and writes the
  kernel's RWCTAB02 tables. For the S5K3L6 (local `fw-camera-r5q/camera-s5k3l6.tab`, 2732 bytes, never in the
  repository):

  | Item | Value |
  | --- | --- |
  | Slave | 0x5A (7-bit 0x2D) |
  | ID | 0x30C6 at 0x0000 (2/2 bytes) |
  | Lanes | lane assignment 0x3210, 4-lane D-PHY |
  | Colour order | GRBG |
  | Mode 0 | 4000x3000 RAW10 @ 30, pixel clock 476 MHz, line 4896, frame 3253, settle 14 |
  | Mode 1 | 4000x2256 @ 30 |
  | Mode 2 | 1028x772 @ 120 |
  | Tables | init 22 writes; res0/1/2 53/53/52; on (0x0100=0x0100), off, hold on/off, test pattern off/on |
  | Power up | kind 10 (reset) low; VANA, VDIG, VIO; MCLK 19.2 MHz; reset high + 18 ms (CamX kinds) |

  That matches what Tests 271-274 did by hand.
- **Next:**
  1. the sensor's tables written over the bit-banged bus (init, res2, on), with the frame counter read back to see
     it stream;
  2. then the SM8150 receive path (CSI-PHY 2 v1.1, CSID-lite, IFE-lite, SMMU), so RAW10 frames reach
     `/dev/redwood-camera`.
- **Test 275 (kernel t275, root test275 with the local S5K3L6 table in /fw; `camsensor=1`): THE SENSOR STREAMS.**
  - After the ID read, `cam_table` (redwood_sec_ts.c) wrote its own tables over the bit-banged bus: **init 22,
    res2 52 (1028x772 RAW10 @ 120), on 2, every write acknowledged**.
  - SMIA registers read back: mode_select 0x01 and the **frame counter (0x0005) 1, 3, 6, 8** over the four reads
    (25 ms apart, plus the bus time).
  - Then "off", and everything back as found: GDSC 0x7022f001, LDO2/LDO6 0x20/0x28, reset low, MCLK off.
  - **Proven from our kernel: the sensor powered, configured from the phone's own tables, and producing frames on
    its MIPI lanes.** Nothing receives them yet.
- **Next, the receive path on SM8150 (Titan 175):**
  - CSI-PHY 2 (v1.1 D-PHY, 4 lanes, settle 14);
  - CSID-lite RX from that PHY, RDI 0 with RAW10 (data type 0x2b);
  - IFE-lite write master into memory, through the camera SMMU;
  - then frames into `/dev/redwood-camera`, with the donor's software ISP on the iOS side (GRBG).
  - Register references: Qualcomm's camera driver for SM8150 (cam_csiphy_1_1_hwreg.h, cam_csid_lite_17x/175,
    cam_vfe_lite), and the donor's cam=3/4 code for the order of steps.
