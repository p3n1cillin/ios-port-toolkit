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

## The receive path: spec (2026-11-19, desk research) -> `CAMERA-RX-SPEC.md`

- **The donor's CSID-lite / IFE-lite code is Titan 165 and uses the same register maps as SM8150's** (cam_ife_csid_lite17x,
  cam_vfe_lite17x, bus ver2). Its CSID and IFE steps port with the same offsets and values.
- **Differences:**
  - camcc PLL0 is a Trion (camcc-sm8150 config values) feeding the RCGs: cphy_rx 400, csi2phytimer 300, IFE-lite
    and CSID 400, camnoc_axi, slow_ahb;
  - GCC hf/sf AXI branches;
  - **the titan power domain is 0xc1bc**: the donor's 0xc194 is a clock register here and must not be written;
  - CSI-PHY v1.1 at 0xac65000 + 0x1000·n, with its own 4-lane D-PHY tables (settle 14 at lane +0x08);
  - RPMh: MMCX and MX levels and `ldoc3` (1.2 V CSI-PHY supply), through the Wi-Fi driver's TCS helper (one owner
    of TCS 1).
- **SMMU:** one SMR (0x86200840: stream 0x840, mask 0x620) with an S2CR copied from a boot context bank whose
  translation is off. This is the method that worked for Wi-Fi, SLIMbus and FastRPC. The IFE then writes
  **physical addresses below 4 GB**.
- **The blocker: memory below 4 GB.** Only the loader's shared 64 KB is there. A 1028x772 RAW10 frame (stride 1296)
  is about 1 MB. Two options:
  - the loader reserves about 3 MB below 4 GB for the camera (a loader change, with every SMR/S2CR readback
    checked);
  - a first proof crops a 32 KB window in the CSID.
- **Biggest risk:** with translation off, a frame longer than the buffer overruns it. Install the SMR before the
  write master and remove it only after the write master is off plus one frame time.

## The receive path, first attempts (Tests 276-283; `camrx=N` with `camsensor=1`; `cam_rx_up` in redwood_sec_ts.c)

- New: `redwood_r5q_rpmh_level(id, vlvl)` in redwood_wifi.c. It votes a cmd-db rail by level: the vlvl is turned
  into its position in the rail's aux level list and written through the same TCS 1 helper.
- New: `rxlog()`. Camera lines go to the loader's log ring as well, and `camrx=N` stops after stage N: 1 rails, GCC
  and PLL0; 2 RCGs and branches; 3 the CPAS read; 4 the CSI-PHY; 5 the CSID. The teardown undoes only the stages
  that ran.
- **Test 276 (full path):** the system ended (back in the loader) with no camera line in any log. The console's
  lines are lost when the system hangs, and the loader's ring does not hold this kernel's lines after a hang.
  Test 277's first build failed on a `\n` broken by a heredoc.
- **Test 278 (`camrx=1`): works, and the phone stays up.**
  - rails: MMCX at level 128 (position 3), MX at 192 (position 3), ldoc3 on;
  - GCC camera hf/sf AXI on;
  - **PLL0 locked** (mode 0xd0800005, opmode 1, user_ctl 0x3107);
  - all undone afterwards.
- **Tests 279-282 (`camrx=3`):** every one hung on the **same line of the log, about 3.3 s after PLL0 locked**,
  whatever that step was:

  | Test | Variant | Hung at |
  | --- | --- | --- |
  | 279 | full stage 3 | branch 0x6068, right after camnoc_axi 0xc18c |
  | 280 | without camnoc | branch 0xc038 (ife_lite_0_csid) |
  | 281 | without the GCC camera AXI | branch 0xc038 |
  | 282 | CSID and IFE-lite from TCXO instead of PLL0 odd | branch 0xc038 |

  The live log shows each step after a 300 ms pause, so the hang is tied to time since the rails/PLL0 came up,
  not to one register.
- **Test 283 (20 ms pauses):** the live log lost the last lines before the hang, so it shows nothing.
- **Phone:** back on image 275 with the standard args (no `cam*`), up.
- **Leads, not yet tested:**
  1. PLL0's 1200 MHz needs MX at SVS_L1 **and** the camera's own clock voting on MMCX: something (AOP/RPMh) may
     drop or collapse a rail or the titan GDSC about 3 s later. Read back MMCX/MX/GDSC state every 0.5 s
     *without* PLL0, then with PLL0 at a lower rate (e.g. 600 MHz, L_VAL 0x1F, which needs only MX SVS);
  2. hold r5q_pmic_lock for 3 s without any camera writes, to rule out the probe thread itself (it is the touch
     thread);
  3. log through a channel that outlives the hang: a small ring in the loader's low-DMA block, read back by the
     loader after the reset.
- **Test 284 (the probe in a thread of its own):** the same hang. It is not the touch thread.
- **Test 285 (`cam_nopll=1`, every RCG from TCXO): no hang.**
  - All branches on (csiphy2, timer, CSID, cphy, IFE-lite, cpas_ahb).
  - **CPAS answers: camera version 0x10705 (Titan 1.7.5), CPAS version 0x10000001.**
- **Test 286 (PLL0 at 600 MHz, `cam_pll600=1`):** hung as before. So it is not PLL0's rate: it is an RCG fed from
  PLL0.
- **Test 287 (PLL0 600; slow_ahb on TCXO; cphy_rx, CSID and IFE-lite from PLL0 odd 200, timer from even 300;
  `camrx=5`): THE CSID RECEIVES THE SENSOR'S FRAMES, WITHOUT ERRORS.**
  - CSI-PHY 2 configured (lane enable 0xd5, settle 14). CSID reset (version 0x10000000), RX cfg 0x232103 / 0x41,
    RDI0 cfg 0x802bf007, ctrl 1.
  - While the sensor streams 1028x772 RAW10 @ 120:
    - **RX irq 0xff**: start and end of transmission on all four lanes;
    - **packets 1009 -> 3002 -> 4952 -> 6945**, ECC 0, CRC errors 0;
    - **RDI0 irq 0x1ff8** (SOF, EOF, SOL, EOL, ...);
    - **byte counters 992020 / 992020 per frame = 1028 x 10 / 8 x 772 exactly.**
  - Then stream off, `cam_rx_down`, everything back; **the phone stays up.**
  - **The hang's cause:** `slow_ahb` (the camera's register bus, RCG 0x7058) fed from PLL0. On TCXO it is fine.
    Why (a vote, a divider of 7.5 or 4 on even, something else) is not known; TCXO serves.
- **Next: frames into memory.** IFE-lite 0 RDI0 write master (spec (e)), the camera NoC, the GCC camera AXI, the
  SMMU SMR 0x86200840 with a copied bypass S2CR, and a buffer below 4 GB (the loader's lowdma block grown, or a
  crop window to fit 64 KB first).
