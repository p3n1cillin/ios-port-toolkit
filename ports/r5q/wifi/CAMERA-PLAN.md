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
- **Test 288 (kernel t288; `camrx=6 cam_pll600=1`): THE FIRST REAL FRAME IN MEMORY.** Code: `cam_rx_memory_up`,
  `_poll`, `_down` in redwood_sec_ts.c.
  - Camera NoC: RCG 0xc170 from PLL0 even 300 / 2, branch 0xc18c on (0x221). GCC camera AXI on.
  - SMMU: **SMR69 = 0x86200840**, with the S2CR of SMR0 (a non-translating boot bank).
  - Buffer: the loader's lowdma block 0xdbca9000 (64 KB), filled with 0xa5.
  - The CSID's RDI0 is set again to PLAIN16_10 with a 1024x16 crop (cfg 0x802b2467). IFE-lite reset (version
    0x10000000, bus 0x20000). WM0 in frame mode, FRAME_INC 0, register update 0x2.
  - **FRAME DONE** at the first poll: bus status1 bit 0, top status 0x8000020 (SOF + register update),
    **violation 0, no SMMU fault** (global 0). WM0 was stopped there, and the match taken away 20 ms later.
  - **The buffer: exactly 32768 of 65536 bytes written**: 1024 x 16 x 2, with the 0xa5 past it untouched.
  - **Pixels 63..66**, the sensor's black level (64 on 10 bits): the picture is dark. Either the phone lay with no
    light on that camera, or the init and mode tables' default exposure is short.
  - The CSID kept counting whole frames (32768 bytes each); the phone stayed up.
- **Proven: sensor -> MIPI -> CSI-PHY 2 -> CSID -> IFE-lite -> SMMU -> RAM, all from our kernel.**
- **Next:**
  1. light: the exposure/gain registers (the module file's integration and gain addresses), and a test pointed
     at light;
  2. whole frames: a larger buffer below 4 GB (the loader's low block grown to about 3 MB) and RDI0 back to RAW10
     packed (992020 bytes per frame);
  3. hand the frames to `/dev/redwood-camera`: port the donor's `ios_session` onto this path, so Camera.app shows
     the S5K3L6's picture through the iOS side proven in Test 269;
  4. then the other sensors (IMX586 main, IMX616 front, S5K5E9).
- **Test 289 (kernel t289 + loader with `camdma_pa`; `camrx=7 cam_pll600=1`): A WHOLE FRAME IN MEMORY.**
  - The loader now reserves **4 MB below 4 GB for the camera** (`camdma_pa=`, port/xnuloader/loader.c): here at
    0xd8200000.
  - Our own exposure, by SMIA: 0x0202 = 768 lines, 0x0204 = 0x100 (about 8x). Both writes acknowledged.
  - RDI0 kept at RAW10 (0x802bf007), with no crop. WM0 in frame mode at 0xd8200000, FRAME_INC 0. Frame done at the
    first poll, violation 0, **no SMMU fault**.
  - **1,000,386 bytes changed, the last at 0xf443f = 1296 x 772 - 1**: exactly one frame at the CSID's 16-byte line
    stride (1296 for 1028 RAW10 pixels), nothing past it.
  - **Still black:** mean high byte 15 (the black level), and the 64x24 character picture all blank. Most likely
    the rear (ultra-wide) camera faced the desk; the owner was not there to point it at light.
- **Next:**
  1. a frame with the phone pointed at light (owner), or the front camera;
  2. the hand-over to iOS: an r5q back end in redwood_camera.c's `/dev/redwood-camera` session (the donor's
     `ios_session`). Its frame ring goes in the 4 MB camdma block (three 1 MB places plus spare); its power, receive
     and capture steps are the ones proven here.
- **Test 290 (kernel t290, root test275; `cam=5 cam_r5q=1`): THE S5K3L6 BEHIND /dev/redwood-camera.**
  - New boot-arg `cam_r5q=1` (with `cam=5`): redwood_camera.c's iOS side (the one proven with the synthetic chart
    in Test 269) serves the S10 Lite's own S5K3L6 for every camera iOS asks for. The table is
    `fw/camera-s5k3l6.tab`; `find_pins()` (the POCO's pins) is skipped, it data-aborted at 0x15000.
  - The session: `r5q_session()` in redwood_camera.c calls `redwood_r5q_cam_run(cb, ctx)` in redwood_sec_ts.c.
    That runs the Test 289 bring-up (power, ID, tables, exposure, CSI-PHY, CSID, IFE-lite, SMMU) with PLL0 at
    600 MHz, sets the frame length to 3268 lines (about 30 fps), then loops: wait for IFE bus status1 bit 0
    (100 us polls, up to 100 ms), clear it, point WM0 at the other 1 MB half of the camdma block, register
    update, and hand the finished half to the callback (`r5q_frame`, which copies it into the frame ring that
    Camera.app reads). It stops when the callback says so (close, switch, nobody reading) and takes everything
    down again.
  - **Result:** "frame length 3268 lines: taken", "SESSION: frames for iOS from the S5K3L6", and **2 frames
    handed to iOS**. Then Camera.app switched to the front camera. That select passed the switch's own mode
    (`ios_switch_mode`), which picked **mode 0, 4000x3000**: too big for the 1 MB halves, and not a mode
    `redwood_r5q_cam_run` streams. "Nobody reads: 0 done", and the phone then hung into the loader (fastboot).
  - **Fix (Test 291):** `ios_select` forces mode 2 (1028x772) for every select when `cam_r5q` is on.
- **Test 291 (kernel t291; `cam=5 cam_r5q=1`, mode 2 forced): the switch is fine, the second session finds camcc dead.**
  - The first session again: sensor ID, tables, frame length 3268, CSI-PHY, CSID, IFE-lite, SMMU, **2 frames handed
    over** (RX: 1548 packets, ECC 0, CRC 0; RDI0 992020/992020 bytes; no SMMU fault; 2,000,750 bytes written =
    two frames in the two 1 MB halves). Still dark (mean high byte 15).
  - Camera.app then asked for the front camera; the select now keeps **mode 2, 1028x772**.
  - **The second session: `CAM_CC_GDSC_CLK 0 -> 0; titan_top_gdsc 0 -> 0`, MCLK RCG cfg 0, and the sensor never
    answered.** Every camcc register read 0: camcc had no power. Cause: `cam_rx_down` voted MMCX (and MX) to 0.
    camcc lives on MMCX, and the boot's own vote, from the same HLOS voter, is what had kept it up through
    Tests 270-290. Our zero replaced it.
  - The phone stayed up this time (Test 290's hang likely came from the same zero votes, MX in particular).
  - **Fix (Test 292):** the sensor probe votes MMCX 128 / MX 192 before it touches camcc, and `cam_rx_down` keeps
    both votes.
  - **Lesson (for every port):** an RPMh level vote replaces this voter's earlier vote, including the boot's. Never
    "release" a rail to 0 that you did not raise from 0 yourself.
- **Test 292 (kernel t292; votes kept): REAL FRAMES READ BY iOS, through the camera switch.**
  - First session (the back camera): 3 frames handed over (RX 2322 packets, ECC 0, CRC 0; no SMMU fault). The
    teardown now leaves camcc powered (`titan_top_gdsc back: 0x7022f001`).
  - Camera.app switched to the front camera. **The second session came up fully** ("rails before camcc: mmcx 3,
    mx 3", GDSC powered, the sensor answered, the same tables), and **mediaserverd read real frames**:
    `frame 2 read: 1000512 bytes ... copied in 260 us`, then `frame 2: exposure 7.8 ms, gain 1.0x, green 449 above
    black; lux index 101, about 5351 K`, then frame 3. So the sensor saw real light this time (green 449 above
    the black level), and iOS's own exposure/white-balance code ran on it.
  - **Then the phone went down** (the next screenshot showed the loader; the reset overwrote the RAM ring).
  - Likely cause: `redwood_r5q_cam_run` holds `r5q_pmic_lock` for the whole session. The touch thread takes it
    every 5 s for the battery gauge, so it stalls for as long as the session runs, and Tests 279-283 hung whenever
    the touch thread was held up for more than ~3 s. The first sessions were too short to show it.
  - **Fix (Test 293):** the lock is let go for the streaming loop (which touches only IFE-lite registers and the
    caller's ring) and taken back for the teardown. The session also logs every 300 frames.
- **Test 293 (kernel t293; the battery lock let go while frames flow): the same end.** Session 1 (3 frames) and the
  switch as before; session 2 up, iOS read frames 2 and 3 (green 449 above black, lux index 101), and the phone went
  down again, about 3-4 s after the second session's PLL0 lock (timestamps around it: 08:20:10 -> the last line
  08:20:14). No "300 frames so far" line, so it never reached 10 s. So the lock was not (the whole) cause.
  - Re-reading Tests 276-289: Test 287 kept PLL0 on for more than 3 s (the 300 ms pauses after each step) without a
    hang, so PLL0's time alone is not it. What is new in a session: IFE-lite writing memory continuously through the
    camera NoC for seconds (Tests 288-289 stopped the write master after one frame), and iOS reading.
  - **Test 294 (`camsensor=1 cam_soak=20`, no `cam=5`):** a 20 s session whose only reader is the kernel (one
    line a second with the middle line's mean). Hangs ~3 s in: the streaming hardware path. Survives: the iOS side.
- **Test 294 (kernel t294; `camsensor=1 cam_soak=20`, no iOS camera): THE HARDWARE STREAMS FOR 20 s.** One session,
  read only by the kernel: **600 frames, 0 misses; RX 464,400 packets, ECC 0, CRC 0; RDI0 992020/992020 bytes a
  frame**; one line a second throughout ("30 frames (1 s) ... 600 frames (20 s)"), then the teardown and "the phone
  is still up". The live log went on for 2 more minutes with no panic. So PLL0, the camera NoC, the GCC AXI,
  IFE-lite writing continuously and the SMMU match are fine for 20 s.
  - The picture: middle line mean 15 of 255 (the black level) all along: the phone lay face down.
  - Found later in fastboot (the loader), at some time after 08:31; the reset wiped the RAM ring, so when and why
    is unknown.
  - Left: (a) the second session after a teardown (Tests 292-293 always died in the second one), (b) iOS reading.
  - **Test 295 (`cam_soak_first=3`):** a 3-frame session, taken down, then the 20 s one, as iOS's switch does it;
    followed for 400 s to see a later death.
- **Test 295 (kernel t295; `cam_soak=20 cam_soak_first=3`): TWO SESSIONS, BOTH FINE.** 3 frames, teardown
  (`titan_top_gdsc back: 0x7022f001`), then 20 s: 600 frames, RX 464,400 packets, ECC 0, CRC 0, "the phone is still
  up", and the live log ran on to the end of the 400 s watch with no panic.
  - The picture: middle line mean **127 of 255, the same in every one-second line**; Test 294's was 15 (black)
    throughout. iOS's "green 449 above black" in Tests 292-293 is the same mid-grey (64 + 449 = 513 of 1023). A
    perfectly constant middle line for 20 s is suspicious (a flat field or a test pattern rather than a scene):
    to look at with a whole-frame dump.
  - **Re-reading Tests 292-293:** each live log's file time is the end of its watch window (boot + FOLLOW), not
    the last timestamped line. The phone was *not* proven dead right after frame 3; it was found in the loader
    after the window. **Every run with a working session (292-295) ended in the loader some minutes later; Test 291
    (whose second session never powered) did not.** So the problem is a later death after a camera session.
  - **Test 296 (the t295 kernel again, FOLLOW=900):** a 15-minute watch to catch when it dies and its last words.
- **Test 296 (t295 kernel, FOLLOW=900): no death at all.** iOS ran its whole time; the run ended in the orderly
  way at 1015 s ("iOS returned": sound, audio DSP and GPU stopped and put back). **Correction to Tests 292-295:**
  `phone.py raw devices` says "xnuloader fastboot" *while iOS runs too* (live mode: the loader serves USB on core 0
  while iOS runs on core 7), and each live.txt ends when its FOLLOW window ends. So "found in the loader" after
  those runs was either the run's time limit or a misreading. **None of Tests 292-295 is a proven death.** New
  tool: `phone.py alive` (exit 0 iOS running / 1 maintenance mode / 2 nothing answers).
- **Test 297 (t295 kernel; `cam=5 cam_r5q=1`):** at boot, mediaserverd opened the camera on its own: back camera
  (2-3 frames), the switch to the front, **96 frames read by iOS**, then iOS stopped the stream. `alive` = iOS
  running; then the Camera app was opened, and iOS was gone within ~30 s.
- **Test 298 (the same image):** the follow captured nothing (an early-capture gap); a plain `phone.py restart`
  showed iOS fully up (SpringBoard, Wi-Fi joined). With the camera on, iOS was gone ~2 minutes after boot. On a
  third restart, iOS stayed up until **the Camera app was opened (09:25:04): gone by 09:25:34.** Reproduced.
- **Test 299 (kernel t299: `ring_free()` waits for reads in flight):** a real race in camdev_read (it copies up to
  1 MB out of the ring with ios_lock dropped, and a session's end freed the ring under it). Fixed: `ring_readers`
  counted under ios_lock, the ring's base and sizes taken under the lock, `ring_free()` waits up to 5 s. **Not the
  cause:** boot-time sessions fine (back, front, 108 frames to iOS, "closed"), then the Camera app opened at
  09:28:45 and iOS was gone by 09:29:16.
  - The merged log ends at 09:28:43 with a userland line (`IOServiceMatching failed for: AppleM2ScalerCSCDriver`)
    and nothing after: **the loader's USB stopped answering too, so it is a whole-system hang**, not a panic (no
    panic buffer saved). The camera driver's own lines never got out (the PC fetches the log every ~2 s).
  - Pattern: every hang came at the **third camera session after boot** (290: back, front, the 4000x3000 one;
    297-299: back and front at boot, then the Camera app). Runs with two sessions (292, 293, 295) survived.
  - phone.py's follower now writes live.txt as it grows (it was written only when the window closed).
- **Test 300 (`camsensor=1 cam_soak=20 cam_soak_first=3 cam_soak_n=5 cam_steplog_ms=1500`, no iOS):** five short
  sessions, then 20 s, with 1.5 s after every camera line so the last step before a hang reaches the PC.
- **Test 300 (kernel t300; five short sessions then 20 s, `cam_steplog_ms=1500`, no iOS): HUNG IN THE FIRST
  TEARDOWN.** With 1.5 s after every camera line, the first session ran (3 frames, RX 113,004 packets, ECC 0, CRC 0),
  then "SMR69 taken away; global fault status 0", 24 picture rows (36 s at 1.5 s each), "the whole frame ... mean
  high byte 15", and nothing more: the next line, "camera rx: down", never came. So the hang is in `cam_rx_down`'s
  register writes, which had run cleanly dozens of times.
  - What made it likely here: the order of the teardown. The session ended with the sensor's "off" table first;
    then `cam_rx_memory_down` wrote WM0 cfg 0 + register update 0x2, **but a register update is taken at the next
    frame start, and a stopped sensor sends none**, so the write master stayed armed; then the SMMU match was taken
    away. Anything that started a frame afterwards (the CSI-PHY reset in the teardown) let IFE-lite write as an
    **unmatched stream**: an SMMU fault, and the camera NoC stalls the bus. The 36 s between the match's removal and
    the rest of the teardown widened the window; iOS's many sessions (open, switch, close) hit it sooner or later.
  - The GPU's MX/CX votes were checked and ruled out: they go through the GPU's own RSC (another voter; RPMh takes
    the highest of the voters).
- **Test 301 (kernel t301):** (1) the SMMU match is put in once and kept for later sessions (`rx_smr_kept`; it only
  lets the IFE's own streams through untranslated), so there is no unmatched window; (2) a session ends with WM0
  off + register update **while the sensor still streams**, 50 ms (more than a frame), then the sensor's "off".
  Test 300's harsh soak again.
- **Test 301 (kernel t301; the SMMU match kept, WM0 off before the stream's off; Test 300's soak): the teardown now
  gets through ("SMR69 kept; global fault status 0", "camera rx: down"), and then hangs one step later**, before
  "titan_top_gdsc back" (the pins, MCLK off, the GDSC). iOS gone.
  - Caution about this soak: `cam_steplog_ms` sleeps while the probe holds `r5q_pmic_lock`. Test 284 already
    showed the probe in its own thread hung the same way, so the touch thread waiting on that lock is not the cause.
  - **The loader's watcher (core 0) keeps a record that survives the reset** ("N s: the kernel has logged X bytes,
    heartbeat H; the last of it:"; `phone.py logs`, firmware-ring-now.bin / boot-manager-*.txt). For the iOS run
    that died, the kernel's last lines were "SESSION ends: 2 frames handed over" and "table off": the hang came
    between the sensor's "off" and the first CSID/PHY read.
  - **Three hangs, three different camera register accesses in a teardown** (299-ish: before the CSID read;
    300: in cam_rx_down's writes; 301: at the pins/MCLK/GDSC). A random hang on access points at the register bus's
    clock. Every session's log says `GCC camera hf_axi 0x1 sf_axi 0x1 xo 0x80000000`: **GCC_CAMERA_XO_CBCR
    (0x10b044) is not enabled**, while slow_ahb (the camera's register bus) runs from TCXO since Test 287, and
    GCC_CAMERA_AHB (0x10b008) runs under hardware gating (0x80000003). Linux keeps the camera's GCC AHB and XO
    clocks always on (critical clocks).
- **Test 302 (kernel t302):** at the start of camera use, GCC_CAMERA_XO enabled and GCC_CAMERA_AHB forced on (HW_CTL
  cleared), both kept on afterwards. Test 300's harsh soak again.
- **Test 302 (kernel t302; GCC camera XO and AHB on): "GCC camera XO 0x1, AHB 0x1" (both running), and the same
  hang** as Test 300 (after "the whole frame", before "down"). So those clocks were not it.
  - **What kind of hang:** none of the hung runs (loader starts 750-770) saved the loader's "iOS returned" or "final
    reset" record, so the loader's own watcher (which ends a run whose heartbeat is silent for 50 s) never got to.
    Both cores stopped at once: a hardware bus hang, then the watchdog's reset.
  - In Tests 300 and 302 the camera thread was **asleep** (between paced lines) when it hung: no camera register
    was being touched. Something in the running camera hardware stops the bus by itself after a while.
  - The thread through every hang since Test 276: PLL0-fed clocks running. Tests 279-286 (slow_ahb on PLL0) hung
    ~3 s after PLL0 locked; Test 285 (`cam_nopll=1`, every camera clock from TCXO) never hung; since Test 287 (only
    the data path on PLL0) the hangs come after tens of seconds.
- **Test 303 (the t302 kernel; `cam_nopll=1`, three short sessions then 120 s, no pacing):** every camera clock
  from TCXO. Whether frames still come at 19.2 MHz, and whether 2 minutes pass without a hang.
- **Test 303 (`cam_nopll=1`: every camera clock from TCXO; three short sessions, then 120 s): no hang, and no
  data.** "RX irq 0, packets 0", 50 misses each session; iOS up afterwards. At 19.2 MHz the receive side takes
  nothing (the CSI-PHY's clock is too slow for the lanes). So with PLL0 off there is no hang, but no camera either.
- **Test 304 (kernel t304; `camrx=5 cam_pll600=1 cam_hold=150`): THE RECEIVE SIDE ALONE FOR 150 s, NO HANG.**
  PLL0 at 600, CSI-PHY 2, CSID streaming with no IFE, camera NoC or AXI writes: "held 1 s: 73,484 packets" ...
  "held 150 s: **11,152,021 packets, ECC 0, CRC errors 0**", 992020/992020 bytes every frame, then a clean teardown
  ("titan_top_gdsc back: 0x7022f001"); iOS up. The probe held `r5q_pmic_lock` the whole 150 s too, so that theory is
  finally out.
  - **So the hang is in the memory-write path:** IFE-lite -> camera NoC -> multimedia NoC -> DDR. We had never
    cast a bandwidth vote for it. On SM8150 the camera's writes cross `qnm_camnoc_hf` (BCM MM1) to
    `qns_mem_noc_hf` (MM0) (the SF side MM2), then LLCC (SH0) and DDR (MC0). Without our votes those clocks follow
    whatever the other voters ask; an in-flight camera write over a NoC that is dropped or slowed under it can
    stall the bus.
- **Test 305 (kernel t305):** new `redwood_r5q_rpmh_bcm(name, avg, peak, mask)` in redwood_wifi.c (cmd-db aux
  `{unit, width}`, AB/IB in that unit, 14 bits each, valid + commit; Linux's bcm-voter). The camera's memory stage
  votes MM0, MM1, MM2, SH0 and MC0 (0.5 GB/s average, 2 GB/s peak) once, and keeps them (this voter may hold the
  boot's display votes on the same BCMs). Test 300's paced soak, with a 150 s main session.
- **Test 305 (kernel t305; the bandwidth votes):** "BCM MM0 at 0x50084 (unit 1670000, width 64): AB 300, IB 1198",
  MM1 and MM2 the same, SH0 (unit 3910000) AB 128 IB 512, MC0 (unit 2050000) AB 244 IB 976, all committed. **The
  same hang** as Test 301 (after "down", before the GDSC line). So bandwidth starvation was not it either; the
  votes stay in (they are right to have).
- **Test 306 (t305 kernel; an unpaced 3 short + 150 s soak):** **iOS died ~30 s after boot, before the camera
  started**: the log ends with Bluetooth's bring-up, "stage 3: HCI_Reset: sent", with no answer, just before the
  switch to 3 Mbit/s (a serial engine's GCC clock change). A separate hazard, not the camera's (noted for later).
  The camera lines of finished runs are found again after a reset in the loader's ring (`rxlog` ->
  `loader_log_putc`; firmware-ring-now.bin): Test 304's 150 holding lines, intact.
  - GPU temperatures 52-59 °C in every run, the battery full: not heat or power.
- **Where it stands:** the receive side alone runs 150 s (304). Full unpaced sessions of ~20 s run (294-296). The
  paced runs hung while the **memory stage** (camera NoC, GCC AXI, IFE-lite, the SMMU match) stayed up but idle
  for over a minute after only 3 frames, and iOS's long Camera-app session hung. So the trigger looks like the
  memory stage being up for more than ~30-60 s, not the amount written.
- **Test 307 (t305 kernel; `camrx=6 cam_pll600=1 cam_hold=150`):** the memory stage held 150 s, IFE-lite writing
  the cropped frame into the 64 KB low block every frame.
- **Test 307 (t305 kernel; `camrx=6 cam_pll600=1 cam_hold=150`): the memory stage held 150 s, no hang.** IFE-lite
  wrote the cropped frame (32768/32768 bytes) into the 64 KB low block every frame: "held 150 s: 11,151,343
  packets, ECC 0, CRC 0", a clean teardown, iOS up. Being up a long time is not the trigger.
- **Test 308 (t305 kernel; `camrx=7 cam_pll600=1 cam_hold=150`): whole frames into the 4 MB camera block held
  150 s, no hang.** 11,152,055 packets, ECC 0, CRC 0, 992020/992020 bytes a frame, the frame at 0xd8200000 (last
  byte 0xf443f), a clean teardown, iOS up. **The full level-7 hardware path is stable for minutes.** What is left
  is the session machinery (ping-pong IMAGE_ADDR rewrites, bus IRQ_CLEAR1/GLOBAL_CLEAR per frame, the callback,
  the lock juggling). Its writes were checked against the spec (0x2054 IRQ_CLEAR1, 0x2068 IRQ_GLOBAL_CLEAR,
  0x2214 IMAGE_ADDR, 0x4ac REG_UPDATE): right registers.
- **Test 309 (t305 kernel; Test 306 again):** died the same way as 306, ~34 s after boot, after Bluetooth's
  "stage 3: HCI_Reset: sent", and the loader's ring shows the camera probe never started. **A Bluetooth bring-up
  hang, independent of the camera** (2 of the last ~12 starts). To look at on its own.
- **Test 310:** Test 306's unpaced session soak with Bluetooth off (`-bt -bt_vsc -bt_baud`).
- **Test 310 (t305 kernel; the session soak, `-bt`): the follower's log ended at ~35 s again, but the run's
  surviving kernel bytes (the boot-manager slot, just before the next boot's "Loading DxeCore") go to
  `[117.813498]` (Bluetooth stage 4: the `-bt` removal had not taken).** So in Tests 306, 309 and 310 **iOS did not
  die at 35 s: the USB follower went silent** (each fastboot call had a 600 s timeout; one stuck `oem log` ended the
  capture without a word) while iOS ran on for minutes. The "Bluetooth hang" reading of 306/309 is withdrawn.
- **Logging fixed (2026-11-19), so no run is lost again:**
  1. `follow_run`: 20 s / 40 s timeouts per call, a `== [follower: USB snapshot failed at HH:MM:SS] ==` line in
     the log for every failed snapshot, live.txt written after every snapshot.
  2. **A second channel over Wi-Fi:** wifi/klog.c (`proc_kmsgbuf`, the kernel's message buffer, last N bytes) and
     `phone.py wlog FILE SECS`: through r5qd every second, merged, written after every poll, a gap line with the
     PC's time when r5qd is silent, ending 60 s after the phone goes silent.
  3. **Kernel t312:** every camera line stamped `[seconds.microseconds cCPU]`; the session logs once a second (IFE
     bus status 0-2, top status, violation, WM0 status and address, CSID packets/ECC/CRC, SMMU global fault
     status); every teardown block has its own line before it runs ("down 1: memory stage" ... "down 11:
     titan_top_gdsc"), so a hang names its block.
  4. Test 311 (the t311 kernel, bandwidth votes opt-in/off; the unpaced 150 s session soak) runs with both
     followers.
- **Test 311 (kernel t311; the unpaced soak, with the Wi-Fi follower): a real hang, caught.** Both channels went
  silent at the same moment (11:33:35). The Wi-Fi log has the kernel's last lines: the first short session ran
  (3 frames, RX 3096 packets, ECC 0, CRC 0), "SMR69 kept; global fault status 0" at 78.586 s, then nothing (the
  4 MB scan and the picture rows never came). Bluetooth's stage 3 ended at 78.576 s in the same instant; Test 310,
  with Bluetooth off, ended in the loader too, so Bluetooth is not needed for the hang.
- **Test 312 (kernel t312: stamped verbose lines; Bluetooth off): hung inside its first session.** "[78.592737 c1]
  SESSION: frames for iOS" and no "SESSION ends" (~100 ms later for 3 frames): the hang came while frames flowed.
  The hold runs (307, 308) never hung on the same hardware path.
- **Test 313 (the t295 kernel, from before today's changes; the same 3 + 150 s soak, Bluetooth off): NO HANG.**
  **4,500 frames, 0 misses, "the phone is still up"**, iOS running afterwards. **So the hang is a regression from
  a change after t295.** Of those, t299 (ring_readers) is not used in a soak, t301 (match kept, WM0 off before the
  stream's off) acts only at a session's end, t305's votes have been off since t311, t312 is logging. Left: **t302,
  GCC_CAMERA_AHB forced on (its hardware gating cleared) and GCC_CAMERA_XO enabled**, active from the probe's
  first step. Overriding that clock's hardware gating likely breaks the handshake the camera's register bus relies
  on. (Wi-Fi did not come up in this boot, so only the USB follower recorded it.)
- **Test 314 (kernel t314):** the GCC forcing opt-in (`cam_gccfix=1`), off; everything else kept (match kept, WM0
  order, verbose lines). The same 3 + 150 s soak, Bluetooth on (standard), both followers.
- **Test 314 (kernel t314; the GCC forcing off): still a hang, now at the second session's start.** The Wi-Fi log
  shows session 1 complete and its teardown clean step by step ("down 1" ... "down 11", "titan_top_gdsc back:
  0x7022f001", the S2MPB03 back), then session 2: supplies on, GDSC powered (0xf822f000), "[78.679080 c0] THE
  SENSOR ANSWERS", then nothing (the tables and the receive path come next). So the GCC forcing was not the cause.
  The USB follower failed from the hang on, and said so in the log (113 "snapshot failed" lines); nothing was lost.
  - What remains from after t295: Test 301's two changes. The kept SMMU match stays installed through the camera
    GDSC's collapse and power-up into the next session, exactly where this hang is.
- **Test 315 (kernel t315):** both Test 301 changes opt-in (`cam_smrkeep=1`, `cam_wm0first=1`), off: t295's session
  behaviour with today's verbose logging. The same 3 x 3 short + 150 s soak, both followers.
- **Test 315 (kernel t315: Test 301's changes off too, i.e. t295's session behaviour + verbose logging): still a
  hang**, at session 2's memory stage ("[78.670121 c0] SMR69 0x86200840 ... buffer 0xd8200000", then nothing).
  With every camera change of today switched off, it still hangs; the t295 baseline did not.
- **Test 316 (the t315 kernel; no Wi-Fi traffic from the PC, USB follower only): hung too**, around the camera's
  start (the follower failed from 12:16:31; iOS had joined Wi-Fi: "wifi: join: CONNECTED", r5qd listening).
- **The difference was never in the camera code: it was Wi-Fi.** The only run that survived, the t295 baseline
  (313), was **the one boot in which Wi-Fi never came up**. The Wi-Fi driver writes `TLBIASID` + `TLBSYNC` to the
  apps SMMU (redwood_wifi.c, radio_space) whenever it maps memory for the radio; a TLB sync is broadcast to every
  TBU of that SMMU, the camera's too. **The r5q camera path never took `redwood_smmu_lock`**, the lock Wi-Fi, the
  sound and the donor camera code share: the camera's GDSC power-up/collapse, its SMMU match install/removal and its
  camera NoC/AXI bring-up and teardown could all overlap a TLB sync in flight, and a sync caught by a TBU in a
  power transition never completes: the whole bus hangs. That fits every hang: session boundaries (311, 314, 315,
  the iOS switch), the Camera app (opened over Wi-Fi via r5qd), the paced runs (minutes of exposure).
- **Test 317 (kernel t317):** the camera takes `redwood_smmu_lock` around the GDSC's power-up and collapse, the
  memory stage's bring-up (camera NoC, AXI, SMMU match) and the whole of `cam_rx_down`. Not during streaming (Wi-Fi
  must keep mapping). The 3 x 3 short + 150 s soak with Wi-Fi joined and `wlog` traffic every second.
- **Test 317 (kernel t317; the SMMU lock around the camera's transitions): further, still a hang.** Two whole short
  sessions with Wi-Fi traffic, then session 3 hung after "table init" (bit-banged sensor writes: no SoC camera
  register touched there). Right before the end: `map_with_linking_np: [208(klog)]` (r5qd running klog over
  Wi-Fi). The same in 314 (klog) and 311/315 (`tcp connect incoming ... r5qd`). **The hang lands when Wi-Fi is busy
  while the camera is powered**, wherever the camera is, not at a camera step.
  - Downstream Linux's arm-smmu powers on every TBU's GDSC before any TLB maintenance; a TLB sync that meets a TBU in
    a power transition never completes. Our port never votes the multimedia TBUs: the live DT has them as GCC
    votable GDSCs `hlos1_vote_mmnoc_mmu_tbu_hf0` 0x17d050, `_sf` 0x17d054, `_hf1` 0x17d058 (aggre-NoC TBUs
    0x17d040-0x17d04c, Turing 0x17d05c/60). CAMERA-RX-SPEC (Test 288) had assumed hf0/hf1 on "because the display
    runs"; nothing of ours votes them. The IFE's streams cross mnoc_hf_0_tbu and mnoc_hf_1_tbu.
- **Test 318 (kernel t318):** at the camera's start (under the SMMU lock) the three mnoc TBU GDSCs are voted on
  (SW_COLLAPSE cleared, PWR_ON waited for), their before/after values logged, and the votes kept. The 3 x 3 short +
  150 s soak with Wi-Fi traffic.
- **Test 318 (kernel t318; the mnoc TBU votes):** the three TBU GDSCs read **0x80000001 before our vote: already
  powered** (bit 31) by other voters, our vote off (bit 0); after: 0x80000000. So the TBUs were not collapsed. The
  phone still died early in session 1, right after `map_with_linking_np: [206(klog)]` again. klog (my Wi-Fi log
  tool) ran right before the end in 311, 314, 315, 317 and 318: suspect that the tool itself (proc_kmsgbuf copying
  the message buffer under its lock while the camera thread prints through printf and the loader-ring hook with
  interrupts off per character) can freeze the CPUs; the hypervisor watchdog then resets the phone.
- **Test 319 (the t318 kernel; no klog, USB follower only): hung too**, around the camera's start (12:44:16 = boot
  + ~78 s); the follower lost the last lines and the slots held only an older run (812 s). So klog is not needed
  for the hang.
- **Test 320 (the t318 kernel; Wi-Fi and Bluetooth off: no wifi_modem/rails/wlfw/smmu, no bt):** the soak with
  no Wi-Fi at all, like the one run that survived (313).
- **Test 320 (the t318 kernel; Wi-Fi and Bluetooth off, 0 joins): still a hang**, right after session 2. **So Wi-Fi
  is not needed for it**; the Wi-Fi/SMMU reading of Tests 316-318 was wrong as the root cause (the SMMU lock and the
  TBU votes stay: right to have, harmless).
  - What had changed with the code was the **test's shape**: the surviving t295 baseline ran 1 short session +
    150 s; since Test 309 the soaks ran 3-5 short sessions + 150 s (`cam_soak_n`, which t295 cannot do).
- **Test 321 (the t318 kernel; the baseline's shape, Wi-Fi on): streamed 148 s (4,440 frames), then died around
  the session's end**, at its stop and teardown.
- **The pattern of every hang, 290-321: a power or teardown transition** (a session's start or end, the iOS camera
  switch, the Camera app opening, the stretched teardowns of the paced runs). **Streaming itself holds** (148 s
  here; the 150 s holds of 304/307/308).
- **Test 322 (kernel t322): A PERSISTENT PIPELINE.** The camera comes up once and stays up. A session only starts
  and stops handing frames to iOS (`cam_session_frames()`); when the caller says stop, the sensor keeps streaming,
  IFE-lite keeps writing into the camera block, nothing is torn down or powered off, and `cam_pipe_up` sends the
  next session straight to its frames. `cam_persist=0` brings back the teardown after every session. The 5 short +
  150 s soak with Wi-Fi traffic.
- **Test 322 (kernel t322, the persistent pipeline; 5 short + 150 s soak with Wi-Fi traffic): NO HANG.** All six
  sessions; the second to sixth "(the pipeline was already up)"; 4,500 frames, 0 misses in the long one; "the phone
  is still up"; afterwards iOS running, USB and Wi-Fi both answering.
- **Test 323 (the t322 kernel; `cam=5 cam_r5q=1`): THE CAMERA APP RUNS ON THE S10 LITE'S OWN CAMERA, STABLE.**
  - At boot mediaserverd's sessions (back, the switch to the front, 115 frames, closed) all on the persistent
    pipeline: CSID 71,208 packets, ECC 0, CRC 0, no IFE violation, SMMU global fault 0.
  - The Camera app opened (`phone.py open com.apple.camera`, 13:26:15): **iOS alive every 10 s for minutes** (before
    this fix it died within 30 s every time). The green camera-in-use dot on; 1,740 then 2,223 frames to iOS in one
    session ("camera: frame 1500 read: 1000512 bytes ... copied in 129 us"); CSID 10.5 M packets, ECC 0, CRC 0.
  - Two camera flips (the flip button): each a 2-frame session and a new one, "(the pipeline was already up)", no
    power cycle, iOS up.
  - iOS asked for location access for photo tags: answered "Don't Allow" (privacy-preserving default).
  - **The picture is dark:** a fixed short exposure (768 lines = 7.8 ms, gain 1x; frames "green 0 above black").
    iOS's auto-exposure requests (REDWOOD_CAMERA_EXPOSURE) are not applied on the r5q path yet. Next.
- **Root cause, as far as it is known:** the S5K3L6's receive and memory path is stable while streaming (minutes,
  zero errors), but power/teardown transitions of the Titan block (the GDSC, PLL0, the camera NoC and AXI, the IFE
  write master, the SMMU match) occasionally hang the whole SoC bus; many transitions in a row (iOS's open/switch/
  close, the multi-session soaks) made it near-certain. The exact transition is not pinned down. The persistent
  pipeline avoids them. **Open:** a safe idle power-down (the sensor keeps streaming while nothing reads: power).
- **Pushed: 9af48da** (Test 323: the camera in Camera.app on the persistent pipeline).
- **A real race found in the code (2026-11-19, looking for why transitions hang): the bit-bang bus pointer.**
  redwood_sec_ts.c drives several I2C buses by hand through ONE global pointer `bb` (`BB_CFG(n)` = bb->tlmm +
  0x1000 * n). On the S10 Lite **touch is bit-banged too** ("the engine's FIFO interface is disabled (GSI DMA only):
  the bus is driven by hand"), and the touch thread's `sec_ts_read`/`sec_ts_command` set `bb = &touch_bus` on every
  poll, many times a second, with no lock. Since Test 284 moved the camera probe out of the touch thread, a touch
  poll in the middle of a camera transaction redirected the camera's remaining bit-bang to the touch bus, and the
  probe's **explicit-pin** writes (`rd/wr(BB_CFG(31))`, `(32)`, `(47)`, `(48)`: saving and restoring its pins at
  every session start and end) went to `SOUTH tile + 0x1000 * 31`: the wrong TLMM tile, at an offset with no
  GPIO31 behind it. That fits the whole record: hangs at session starts/ends (where those writes are), never in
  steady streaming (no bit-bang then), none before Test 284 (the probe ran in the touch thread: no race), fewer in
  hold runs (one start and end), none with the persistent pipeline (no probe after the first session). It may also
  explain frames whose content differed run to run (sensor tables written partly to the touch controller).
- **Test 324 (kernel t324): `bb` is one pointer per side**: the touch thread (touch, gauge, the regulator reads,
  the charger dump) has its own, every other thread (the camera's) another; `bb` is a macro choosing by
  `current_thread()`. The decisive check: **the persistent pipeline off** (`cam_persist=0`: a teardown and power-up
  per session), the 5 short + 150 s soak that hung reliably, Wi-Fi traffic.
- **Test 324 (kernel t324; the per-thread bus pointer; `cam_persist=0`): hung in the first bring-up**, right after
  "exposure 768 lines: taken" (next: frame length, then the rails, GCC AXI, PLL0, the RCGs). So the touch race was
  real (the fix stays) but not the whole story.
  - **The pattern in the record:** every bring-up that hung ran **fast** (`rxlog_fast`, sessions since Test 290:
  312, 318, 324, likely 316/319); **no paced bring-up ever hung** (300 ms after each "camera rx: step": the holds
  287-289, 304, 307, 308; 1.5 s: the paced 300-305, which hung only later, at teardowns). Something in the
  back-to-back sequence (clocks, PLL0, RCG switches, PHY/CSID/IFE resets, the camera NoC) needs settling time.
- **Test 325 (kernel t325):** sessions bring the camera up paced too (`cam_fast=1` for the old fast way); ~6 s once
  per boot with the persistent pipeline, within iOS's 15 s wait for the first frame. The hard test: `cam_persist=0`
  (a paced bring-up and a teardown for every one of 5 short sessions, then 150 s), Wi-Fi traffic.
- **Test 325 (kernel t325; paced bring-up; `cam_persist=0`): two full power cycles, then session 3's bring-up
  hung.** The Wi-Fi log (1 s polls) ended at "step: RCGs" (90.486 s).
- **The Wi-Fi log made streaming (2026-11-19):** `klog -f SECS` sends only what is new every 50 ms (~60 KB a run,
  r5qd relays output as it comes) and `phone.py wlog` restarts it at once, writing each chunk to the file as it
  arrives: a hang loses ≤ ~50 ms.
- **Test 326 (the t325 kernel again, streamed log): the same place, exactly.** Session 3: CSI-PHY 2 configured,
  CSID resets, "CSID RX cfg 0x232103 0x41, RDI0 ... ctrl 0x1", **"[94.978812] step: camera NoC (SMMU lock taken)"**,
  then nothing: the camnoc RCG (0xc170) switch and the camnoc_axi branch (0xc18c). Test 279's first hang note said
  the same ("hung right after camnoc_axi came on").
  - **Cause found in our teardown:** PLL0 was turned off while five RCGs still selected it (cphy_rx 0xa064, timer
    0x604c, CSID 0xc020, IFE-lite 0xc004, camnoc 0xc170). Linux's camera RCGs (shared ops) park on the crystal before
    their PLL goes; ours were left on a source killed beneath them, and the next bring-up's switch of the camnoc RCG
    started from it. Also `cam_cc_camnoc_dcd_xo_clk` (0xc194, the camera NoC's dynamic clock divider's crystal) was
    never enabled; the spec had dismissed it.
- **Test 327 (kernel t327):** teardown parks the five RCGs on the crystal (cfg 0x001 + update, waited for) before
  PLL0 goes off; bring-up enables camnoc_dcd_xo before camnoc_axi (and teardown turns it off after). The
  `cam_persist=0` 5 short + 150 s soak, streamed log.
- **Test 327 (kernel t327: the RCGs parked on the crystal before PLL0 goes off; camnoc_dcd_xo on): ROOT CAUSE
  FIXED.** With the persistent pipeline OFF (`cam_persist=0`), the soak that hung reliably: **five short sessions,
  each a full paced bring-up and teardown, then 150 s (4,500 frames, 0 misses), then a final teardown: no hang**;
  30 "parked on the crystal: cmd 0x80000000 cfg 0x1" lines (6 teardowns x 5 RCGs); "the phone is still up"; iOS
  running afterwards.
  - **The cause:** our teardown turned PLL0 off while five camcc RCGs still selected it (cphy_rx 0xa064, timer
    0x604c, CSID 0xc020, IFE-lite 0xc004, camnoc 0xc170). The next bring-up's switch of the camera NoC's RCG started
    from a source killed beneath it and could stall the SoC's bus (the camnoc is the camera's port into the
    multimedia NoC). Hence: hangs only at session transitions (311-326), never while streaming; none with the
    persistent pipeline (no teardown); the first hang of the whole hunt (Test 279) "right after camnoc_axi came on".
  - Kept from the hunt, each right on its own: the persistent pipeline (iOS's switches are now instant), the paced
    bring-up, the bit-bang bus pointer per thread (a real race), the SMMU lock around the camera's transitions, the
    mnoc TBU votes, the frame-ring read/free fix, the verbose stamped logging, the streamed Wi-Fi log.
  - **Toolkit lesson (docs/CAMERA.md):** park every RCG on the crystal before turning its PLL off (Linux's
    `clk_rcg2_shared_ops`), and enable `cam_cc_camnoc_dcd_xo_clk` with `camnoc_axi`.
- **Open:** a safe idle power-down now that teardowns are safe (the pipeline stays up while nothing reads); the
  exposure (the picture is dark: iOS's requests not applied yet); the paced bring-up could become fast again.
- **Pushed: eedc182** (Test 327: the root cause fixed).
- **Test 328 (kernel t328; `cam=5 cam_r5q=1`): iOS'S AUTO-EXPOSURE DRIVES THE SENSOR.**
  - redwood_camera.c's `r5q_frame` passes iOS's pending REDWOOD_CAMERA_EXPOSURE on (`redwood_r5q_cam_exposure`):
    gain converted from the camera entry's units (GAIN_UNITY = 1x: 32 for the back slot, 0x100 for the front) to the
    S5K3L6's SMIA analog gain (32 = 1x, clamped 1x-16x); frame lines at least 3268 (~30 a second); lines up to the
    frame less 8. iOS is told what was applied. The session (`cam_exposure_apply`) writes it between two frames over
    the sensor's own bus (a kept copy of the probe's), under the grouped-parameter hold: 0x0104=1, 0x0340, 0x0202,
    0x0204, 0x0104=0. At a session's start iOS is told 8x in its entry's units (the probe's 0x100 is 8x).
  - The Camera app opened: "exposure from iOS: 1594 lines (14.64 ms), gain 512/32 (16x), frame 3268 lines: taken",
    every request applied; iOS up. **The viewfinder now shows the sensor's grain** (16x gain on a dark scene)
    instead of flat black: iOS's exposure loop reaches the sensor and comes back in the picture.
  - The scene itself was dark (the phone lying on a surface): a picture of something lit needs the owner.
- **Open (camera):** pointing it at light (owner); the shutter/photo; a safe idle power-down (teardowns are safe now);
  the front/other sensors (all of iOS's cameras are the S5K3L6 for now); colour tuning for this sensor (iOS says "no
  colour tuning ... no colour matrix, no lens shading"); a fast bring-up again (it is paced, ~6 s once per boot).
- **Pushed: 6805124** (Test 328).
- **The shutter (t328 kernel, Camera.app, 2026-11-19 14:31:59): THE PHOTOGRAPH IS TAKEN, NOT SAVED.**
  - camera_device.c: "a photograph asked for StillImageCaptureNow", "StillImage: 4032x3024 made of every pixel in
    124 ms", "the first picture given (4032x3024 420f)", "the photograph is given (1 so far)".
  - Then mediaserverd's HEIC encoding (the software HEVC encoder VCPHEVC.videocodec): "Recon frame creation
    failed", "Frame 3: Precompress failed", `std::bad_alloc`, **mediaserverd exited on SIGABRT** and launchd
    restarted it. /var/mobile/Media/DCIM has only .MISC (no 100APPLE). iOS itself stayed up.
  - **Next:** the still is 12 MP (camera_device.c line 435: { 4032, 3024, 30 } "Photo", upscaled from the sensor's
    1028x772). Either a smaller still format there (e.g. 2016x1512 or the sensor's own size) so the software HEVC
    encoder fits mediaserverd's memory, or Camera's "Most Compatible" (JPEG) format so HEVC is not used at all.
- **THE SHUTTER SAVES A PHOTOGRAPH (t328 kernel, 2026-11-19 15:10).** The HEIC failure above is mediaserverd's
  software HEVC encoder running out of memory on a 12 MP still. Camera's "Formats" setting is the preference
  `CAMUserPreferenceCaptureEncodingBehavior` (domain com.apple.camera, user mobile; found in the dyld shared cache:
  also `...Source`); unset = High Efficiency. Set to **1 (Most Compatible: JPEG)** with
  `r5qd run /var/r5q/bin/cfpref com.apple.camera mobile CAMUserPreferenceCaptureEncodingBehavior int 1`, the Camera
  app restarted (`phone.py rd kill Camera`), the shutter tapped:
  - "StillImage: 4032x3024 made of every pixel in 166 ms", "the photograph is given", **"a photograph of 4032x3024
    encoded in 162 ms"**, no crash; **/var/mobile/Media/DCIM/100APPLE/IMG_0001.JPG, 1,888,541 bytes**; its thumbnail
    in the Camera app.
  - Kept across reboots: `r5qd run /redwood/bin/var_store save` (/private/var is a RAM disk here).
  - Also seen: assetsd exited on SIGSEGV once at 15:01 (before the JPEG run, after the HEIC crash); not seen since.
- **Open (camera):** bake the JPEG preference into the image (or make the software HEVC encoder fit); a picture of a
  lit scene (owner); idle power-down; colour tuning; the other sensors.
- **Test 329 (kernel t329): THE CAMERA POWERS OFF WHEN iOS CLOSES IT, AND STAYS UP ONLY ACROSS A SWITCH.**
  - redwood_camera.c's `r5q_frame` tells the kernel why a session ends (`redwood_r5q_cam_keep(ios_open &&
    ios_switch_to != 0)`): a switch keeps the pipeline; a close or nobody reading takes it down. The probe keeps what
    its power-down needs (`pipe_keep`: pins, reset, GPIO14, MCLK, GDSC, the S2MPB03's LDO values, the table) when it
    leaves the pipeline up; `cam_pipe_down()` does the full power-down later from the camera thread (stream off,
    `cam_rx_down` with the RCGs parked, pins/reset/MCLK back, the GDSC collapsed, the LDOs back).
  - At boot iOS opened and closed the camera: one session, then a full power-down ("titan_top_gdsc back: 0x22f001",
    "S2MPB03 #2 back: LDO2 0x20 LDO6 0x28").
  - In the Camera app: open, flip, flip back, Home, reopen, shutter, with `phone.py alive` = iOS running after every
    step. Log: "THE PIPELINE STAYS UP" / "(the pipeline was already up)" for the switch; "the pipeline goes down",
    "THE PIPELINE GOES DOWN", "... the camera is off" on the close; fresh bring-ups on the reopens; **"a photograph of
    4032x3024 encoded in 164 ms"; IMG_0002.JPG saved** (1,890,464 bytes).
- **Open (camera):** a lit scene (owner); colour tuning; the other sensors; the JPEG preference built into the
  image (on this phone it is kept by var_store); a fast bring-up again (paced, ~6 s per open).
- **Test 330 (the t329 kernel; `cam_fast=1`, `cam_persist=0`, 8 short sessions + 60 s): THE FAST BRING-UP HOLDS.**
  Eight full power cycles in ~3 s (~0.37 s each), then 60 s (1,800 frames), "the phone is still up". The pacing was
  only ever hiding the RCG/PLL0 problem fixed in Test 327. (The first try never flashed: the cycle's "end the run"
  step had not stopped iOS -- "a kernel is running (oem stop ends it)"; `phone.py stop` first, then it went.)
- **Test 331 (kernel t331): fast by default** (`cam_slow=1` for the pacing). In the Camera app: the first session
  0.13 s after the camera's power-up began (was ~6 s), "a photograph of 4032x3024 encoded in 187 ms", saved;
  Home: "the camera is off". iOS up throughout.
- **Colour tuning for the S5K3L6 (2026-11-19, desk work):** this phone's vendor partition (local dump) has
  `/lib64/camera/com.samsung.tuned.lsi_s5k3l6.bin` (5,359,760 bytes, "Parameter Parser V1.4.0"; extracted to
  `fw-camera-r5q/`, never in the repository), with the modules the donor's tools/camtuning.py takes (mod_cc13,
  mod_lsc34, refPtV1, initDataV1). New **`pipeline/camtuning_v14.py`** reads V1.4.0 (tree records with 8-byte fields,
  data packed as 4-byte floats; lsc34 grids 222 floats apart: 221 = 17 x 13 and one more) and writes the same
  "RWCISP02" the media library loads:
  - colour matrix: one range, **identity** (Samsung's pipeline colour-corrects elsewhere);
  - lens shading: four light ranges; **corner gains 5.5x (R) / 5.3x (G) / 4.9x (B) at low light, 4.4x at mid,
    1.0 everywhere in the middle** (the ultra-wide's strong vignetting);
  - the AWB's ten reference lights (R/G 0.32 -> 2.0, B/G 1.30 -> 0.30 from 10000 K to a candle).
  `camera-s5k3l6.isp`, 14,408 bytes (local). build_root_app.sh installs it in /fw under the two slot names
  `cam_r5q` serves from this sensor (camera-s5khm2sd.isp, camera-ov16a1q.isp).
- **Test 332 (the t331 kernel, a new root with the tuning; `cam=5 cam_r5q=1`).**
  - **The tuning loads for both slots:** "the s5khm2sd's colour tuning from /redwood/fw/camera-s5khm2sd.isp: 1 colour
    matrices' lights, 3 lens shadings'" (the front slot's likewise); white balance starts from the reference lights
    ("about 5000 K"); "a photograph of 4032x3024 encoded in 167 ms"; iOS up. (The first try built the root but no
    image: `MSYS_NO_PATHCONV=1` exported in the same shell as test.sh breaks its Python steps' paths.)
  - Root `test332-root.apfs.new` is the one to use from now on (the tuning in /fw). Whether the shading looks right
    needs a lit scene (owner).

## The front camera, the IMX616 (2026-11-19)

- **Desk work** (live DT `qcom,cam-sensor@1` under `cci@ac4a000`; Samsung's `sm8150-camera-sensor-r5q-r01.dtsi`;
  Linux `pinctrl-sm8150.c` and `s2mpb03.c`/`.h` from github.com/sm8150-samsung/android_kernel_samsung_r5q):
  - front = yaw 0 (`@1`, and its "full"-mode twin `@8`); slave 0x34 (7-bit 0x1a); CSI-PHY 3; CCI0 master 1 =
    **GPIO19/20** (CCI pin groups: CCI0 m0 17/18, m1 19/20; CCI1 m0 31/32 (the S5K3L6's), m1 33/34);
  - MCLK2 on **GPIO15** (NORTH, function 1 cam_mclk), 24 MHz; reset **GPIO82** (SOUTH);
  - supplies: vio 1.8 V L14; vdig 1.05 V **S2MPB03 #2 LDO1** (reg 0x03, 0.7 V + 12.5 mV steps: 0x9c); vana 2.9 V
    **S2MPB03 #2 LDO5** (reg 0x07, 1.8 V + 25 mV steps: 0xac); the same chip as the S5K3L6's LDO2/LDO6;
  - module file `com.samsung.sensormodule.1_sony_imx616.bin` (local): ID 0x616 at 0x16, 4 lanes, RGGB; modes from
    6528x4896@24 down to **1632x1224@120** (2048-byte RAW10 lines, 2.5 MB a frame: bigger than the 1 MB halves; a
    first version can crop the middle 1028x772 in the CSID, a later one needs a bigger camera block);
    power-up: vana 1 ms, vdig 1 ms, vio 1 ms, reset 10 ms, MCLK 2 ms.
- **Test 333 (kernel t333; `camsensor=1 camfront=1`): `frontsensor_probe()`**: power, MCLK2 from TCXO (19.2 MHz; the
  tables want 24, fine for the ID), reset, a scan of GPIO19/20 and the ID at 0x16 from 0x1a; all put back.
- **Test 333 result: THE IMX616 ANSWERS, first try.** "S2MPB03 #2 LDO1 0x1c LDO5 0x0e as found" -> "LDO1 0x9c LDO5
  0xac now" (1.05 V, 2.9 V); titan GDSC powered; "MCLK2 RCG cmd 0 cfg 0x1 branch 0x1; GPIO15 0x44; reset GPIO82
  0x200 io 0x3"; **"the bus GPIO19/20 answers at: 0x1a"**, **"ID 0616: THE IMX616 ANSWERS"**; everything back as
  found ("LDO1 0x1c LDO5 0x0e"); iOS up. Its tables made locally (`fw-camera-r5q/camera-imx616.tab`, never in the
  repository). Next: its tables, CSI-PHY 3, and the CSID cropping the middle 1028x772 of the 1632x1224 mode.
- **Test 334 (kernel t334, new root with `camera-imx616.tab` in /fw; `camsensor=1 camfront=2`): the IMX616's
  frames.** After the ID: `init` (730 writes) and `res3` (1632x1224@120, 4x4 binned), then under the grouped hold
  the sensor's own digital crop (Sony/SMIA `0x0408`/`0x040a` offsets 306/226, `0x040c`/`0x040e` 1028x772; even
  offsets keep RGGB), output size `0x034c`/`0x034e` 1028x772 and frame length `0x0340` 5160 (~30 fps at the
  tables' 24 MHz; ~24 at the TCXO's 19.2). The crop happens in the sensor because the CSID cannot crop packed RAW10;
  the result has the rear sensor's frame shape, so CSID, IFE-lite, the 1296-byte stride and the camera block stay
  as proven. The receive path now takes its CSI-PHY as a parameter (`cam_phy_index`, `cam_phy_settle`): the PHY's
  registers at `0x0ac65000 + 0x1000 * N`, the CSID's `RX_CFG0 = 0x00032103 | (N << 20)`, and the PHY's own camcc
  clocks moved 0x24 per index (`cam_phy_clk()`: PHY 2's timer RCG 0x604c and branches 0x6064/0x6068 become
  0x6070/0x6088/0x608c for PHY 3, as SM8150's camcc lays them out). Then `on`, 5 s of IFE frame-done counts and the
  middle line's mean, `off`, `cam_rx_down()`, PHY 2 / settle 14 back, and the probe's own power-down.
- **Test 334 result: everything up to the stream, then a hang.** The ID again, then "crop 1028x772 at 306,226 and
  frame 5160 lines: taken", PLL0 locked, the PHY's own clocks moved as planned ("RCG 0x6070", "branch 0x608c",
  "branch 0x6088", all on), "CSI-PHY 3 configured (lane enable 0xd5, settle 12)", "CSID RX cfg 0x332103", the
  camera NoC, SMR69, IFE-lite, "table on: 1 writes taken". Then silence in the kernel's camera lines while the rest
  of the log ran ~30 s more (TCP closes 30 s after connections opened at that moment); the 5 s result never came,
  and the phone came back in maintenance mode (the boot-manager slots still held Test 333's run). This is the
  stalled-core signature of Tests 279-286: the probe has no session callback, so `cam_rx_up` left out the session's
  `pll600` clocks and put slow_ahb (the camera's register bus, RCG 0x7058 cfg 0x20e) on PLL0, which hung every time
  before Test 287. Fix: `cam_rx_up` uses the session clocks for any PHY other than 2 as well (kernel t336 on);
  Test 335 checks it with the t334 kernel and `cam_pll600=1`.
- **Test 335 (the t334 kernel + `cam_pll600=1`): THE FRONT SENSOR STREAMS, no hang.** "FRAMES IN 5 s: 1; middle
  line mean 26 of 255" (the write master was not re-armed: one frame), "packets 102942, ECC 0, **CRC errors
  65535**; RDI0 ... bytes 992020/992020" (exactly 1285 x 772: the sensor's crop is right), "the whole frame: 999580
  of 4194304 bytes written ... (one frame: 1296 x 772)", the teardown clean (RCGs parked, PLL0 off), iOS up. The
  rear S5K3L6 runs at RX irq 0xff / CRC 0 / PHY status 0x40; the front at RX irq 0x800ff / every payload CRC bad /
  PHY status 0x11-0x51. Headers right and payloads wrong = the link's timing: Test 336 sweeps the settle count.
- **Test 336 (kernel t336; `camfront=3`): a settle sweep, six bring-ups in one boot, no hang.** With the write master
  re-armed after each frame: **26-27 frames a second** (frame length 5160 at the 19.2 MHz MCLK). Settle 12, 20, 28,
  36, 48 all alike: ~20,900 packets a second, CRC errors = packets - 2 per frame (each frame's 772 image lines bad,
  two other packets good), ECC 0, 992020 bytes a frame, middle line ~27 of 255. Settle 6: no link (73 packets, ECC
  errors, no bytes). So not the settle count. Test 337: the sensor's solid-colour test pattern, each received byte
  checked against the known value, errors counted per lane and bit.
- **Test 337 (kernel t337; `camfront=4`): the solid-colour pattern.** 133 frames in 5 s (26.6 a second); the
  groups exactly as encoded ("aa55aa5566" for R 0x2aa / Gr 0x155 rows, "cc33cc3333" for Gb 0x333 / B 0x0cc rows:
  lanes in the right order, polarity right, no bit errors), but **150,920 of 992,020 bytes wrong, in all 772 rows,
  evenly over the four lanes and eight bits**: every line right up to byte ~962 (pixel ~770 of 1028, three
  quarters across), two wrong bytes there (0x6e 0xf6), then the rest of the line shifted by two bytes. The payload
  is two bytes off its word count: the CRC fails. A fault at a fixed place in each line is the sensor's crop or
  output, not the link. Test 338: five crops in one boot, each checked against the pattern.
- **Test 338 (kernel t338; `camfront=5`): five crops, all alike.** Digital crop at 306,226 / at the left edge 4,0 /
  the window cut in the analog array (x 1224-5351, y 920-4023, digital 2,2) / the mode's own frame length (1290
  lines: 105 frames a second): ~150,500 of 992,020 bytes wrong, all rows, all lanes. The middle row's bytes at the
  first error: "...aa 55 aa 55 66 | aa 55 66 | aa 55 aa 55 66 ..." -- a group two bytes short, the pattern clean
  after it; the first error moves (byte ~890-960); the line's last bytes are junk ("00 00 ff ff ..."). Bytes are
  lost from the second half of every line. 1024 wide (308,226) was worse (the line junk from byte ~192). res3's
  output PLL (0x030d = 3, 0x030e/f = 0x101) gives ~2 Gb/s a lane at the tables' 24 MHz (~1.6 at our 19.2): past the
  rate where D-PHY wants deskew. Test 339 sweeps that multiplier down (0x101, 200, 160, 128, 100), settle 28.
- **Test 339 (kernel t339; `camfront=6`): THE FRONT LINK IS CLEAN AT A SLOWER OUTPUT CLOCK.** The IMX616's output PLL
  multiplier (0x030e/f) swept, the solid-colour pattern checked byte by byte each time:
  - 257 (res3's own; ~1.64 Gb/s a lane at 19.2 MHz): ~150,800 bytes wrong, CRC errors on every line (as before);
  - **200 (~1.28 Gb/s) and 160 (~1.02 Gb/s): 0 of 992,020 bytes wrong, CRC 0, ECC 0, RX irq 0xff, PHY status like the
    rear sensor's**; 26-27 frames a second;
  - 128 and 100: no output at all (0 packets: too slow for the line).
  The stock rate is past what this receive path takes without deskew; **160 from now on** (~1.28 Gb/s once MCLK is the
  tables' 24 MHz), settle 28.
- **Test 340 prep (kernel t340): the front camera for iOS.**
  - `redwood_camera.c`: iOS's front slot (the camera entry with index 2) runs from the IMX616
    (`redwood_r5q_cam_front(true)` before the session; `cam_front=0` keeps the S5K3L6 for it); frames are reported
    as **RGGB** (0) for the front camera and GRBG (1) for the rear. Before this, the front slot had said BGGR, the
    POCO OV16A1Q's order, over the S5K3L6's GRBG picture.
  - `redwood_sec_ts.c`: `redwood_r5q_cam_run` takes the rear pipeline down if a switch left it up, then runs
    `frontsensor_probe(7)`. That is power, ID, init + res3, the crop, IOP 160, CSI-PHY 3 / settle 28, the receive
    path, "on", then **the same `cam_session_frames` loop** as the rear camera (the battery's lock let go), "off",
    and everything down. The front camera keeps no pipeline across switches.
  - iOS's exposure on the IMX616: lines × 919/729 (its line is ~7.29 µs against the S5K3L6's 9.19), frame lines ×
    5160/3268, coarse time ≤ frame − 48, analog gain as Sony's code 1024 − 1024/gain.
  - Kernel soak without the owner: `cam_soak_alt=1` alternates short sessions front/rear, `cam_soak_front=1` makes
    the long one the front camera's; the soak reader asks for an exposure at frame 15 (both sensors).
- **Test 340 result: THE FRONT CAMERA STREAMS THROUGH THE SESSION PATH, switching with the rear.** Six short sessions
  alternating front/rear. Each front one logs "the front camera asked for: the rear pipeline goes down first",
  "THE IMX616 ANSWERS", "output PLL multiplier 160", "SESSION: frames for iOS from the IMX616 (front)", the exposure
  "taken", "30 frames handed over, 0 misses", "the front camera goes down". Each rear one is a fresh bring-up that
  stays up. Then **20 s from the front camera: 600 frames, 464,400 packets, ECC 0, CRC 0, SMMU gfsr 0**, no IFE
  violation; "the phone is still up", iOS running. (Middle line ~20 of 255: a dim room; the rear's 15.)
- **The IMX616's colour tuning:** the vendor partition's `com.samsung.tuned.sony_imx616.bin` (15.8 MB, local only)
  goes through `camtuning_v14.py` with the new `--crop 0.1875,0.1846,0.8174,0.8154`. That resamples the lens
  shading grids over the sensor's middle 1028x772, so the corner gains (4.8x at the sensor's corners) become ~2.1x at
  the frame's. It has an identity colour matrix and ten reference lights. build_root_app.sh installs it as the front
  slot's `camera-ov16a1q.isp`.

## The main camera, the IMX586 (2026-11-19)

- **Desk work** (live DT `qcom,cam-sensor@0` under `cci@ac4a000`; the toolkit's sm8150.json for the GPIO tiles):
  - **CCI0 master 0 = GPIO17/18** (NORTH), slave 0x34 (7-bit 0x1a), **CSI-PHY 0**, rear (yaw 180);
  - **MCLK0 on GPIO13** (NORTH, function 1), camcc RCG 0x5004 / branch 0x501c (MCLK2's 0x5044 less 0x40);
    **reset GPIO92** (NORTH); "RCAM1_OIS" **GPIO133** (SOUTH);
  - supplies, mostly on a **second S2MPB03 (#1: 0x56 on GPIO94/95, NORTH)**: vdig #1 LDO1 1.1 V, vana #1 LDO6
    2.9 V, vaf #1 LDO7 2.8 V, vio L14, custom1 #2 LDO3 1.8 V, custom2 #1 LDO3 1.8 V, custom3 #1 LDO5 2.8 V;
  - module file `com.samsung.sensormodule.0_sony_imx586.bin` (local; `camera-imx586.tab`, 16,424 bytes): ID
    0x0586 at 0x16 (two bytes), 4 lanes 0x3210, RGGB, eight modes; power-up vana, custom1, vdig, vaf, vio, custom2,
    the OIS line, custom3, MCLK, reset 10 ms; **res5 = 2000x1500 at 120 fps, its output PLL at 0x138 (~2.5 Gb/s a
    lane)**.
- **Kernel t342:** the IMX616's streaming code made common (`sony_stream()` with a `struct sony_cam` per sensor:
  tables, mode, crop, frame and line length, PHY, settle, output PLL multiplier, iOS's exposure ratios); the
  exposure path takes the live sensor's ratios (`cam_live`). `mainsensor_probe()` (`cammain=N`, levels as the front
  one's): both PMICs, the power-up order above, the domain, MCLK0 from TCXO, reset, a scan of GPIO17/18 and the ID;
  then res5 cropped to the middle 1028x772 at 486,364, line length doubled (10752), frame length 5356 (~30 fps at
  24 MHz), the output PLL at 160 (~1 Gb/s a lane, the IMX616's proven setting), CSI-PHY 0, settle 28.
- **Test 342 (kernel t342; `camsensor=1 cammain=4`): THE MAIN CAMERA ANSWERS AND STREAMS CLEAN, first try.** Both
  S2MPB03s found ("#1 LDO1 0x1c LDO3 0x2c LDO5 0x0e LDO6 0x28 LDO7 0x28, #2 LDO3 0x2c"), switched on ("#1 LDO1 0xa0
  LDO3 0x80 LDO5 0xa8 LDO6 0xac LDO7 0xa8, #2 LDO3 0x80"), MCLK0 on, reset and OIS lines high; "the bus GPIO17/18
  answers at: 0x1a"; **"ID 0586: THE IMX586 ANSWERS"**; res5 + crop + line/frame length + output PLL 160 "taken";
  CSI-PHY 0 (settle 28); with the solid-colour pattern **133 frames in 5 s, 0 of 992,020 bytes wrong, CRC 0, ECC 0**;
  everything back as found; iOS up. The common `sony_stream()` made this a one-test sensor.
- **Test 343 prep (kernel t343): iOS's back camera from the IMX586.** `redwood_r5q_cam_sensor(0/1/2)` (S5K3L6 /
  IMX616 / IMX586) replaces the front flag; redwood_camera.c gives the back slot the IMX586 (`cam_main=0` keeps the
  S5K3L6), RGGB. The soak's `cam_soak_alt=2` turns through the three cameras; `cam_soak_front=2` makes the long session
  the main camera's. The IMX586's tuning (`com.samsung.tuned.sony_imx586.bin`, `--crop 0.243,0.2427,0.757,0.7573`:
  corner gains ~1.8x over the crop) installed as the back slot's `camera-s5khm2sd.isp`.
- **Test 343 (kernel t343; the three cameras in turn): ALL THREE SENSORS SERVE SESSIONS.** Six short sessions front →
  main → ultra-wide → front → main → ultra-wide, each "30 frames handed over, 0 misses"; each front or main request
  after the ultra-wide: "the S5K3L6's pipeline goes down first"; exposure "taken" on all three. Then **20 s from the
  main camera: 600 frames, 465,600 packets, ECC 0, CRC 0, SMMU gfsr 0**, no IFE violation; iOS running.
  - Middle-line means: front 20 (facing up, a dim room), ultra-wide 15 (≈ its black level: facing the desk),
    **main 0-4**: dark too, but below the 64 (16 of 255) black level the iOS side assumes. The IMX586's pedestal may
    differ: to be checked in a lit scene (the pattern was exact, so the data path is not the cause).
- **Test 345 (kernel t345; `cammain=2`, the main camera facing the desk): THE IMX586'S BLACK LEVEL IS 0.** A 10-bit
  level count per Bayer site over rows 100-671 of a dark frame: "R 0/0/79 Gr 0/0/68 Gb 0/0/77 B 0/0/69"
  (min/mean/max: everything 0 but a few hot pixels). The sensor sends its picture with the pedestal already taken
  off (noise clipped at 0), where the S5K3L6 and the IMX616 sit at 64; its tables write no pedestal register. So
  the iOS side is told black 0 for the IMX586 (`f.black`, redwood_camera.c), 64 for the others; otherwise its
  shadows would be crushed.
- **Test 350 (kernel t350; the three-camera soak): THE SONY SENSORS' WHOLE BINNED FRAMES.** iOS's sessions no longer
  get the 1028x772 crop of the IMX586 and the IMX616 but their modes' whole frames: **IMX586 res5 2000x1500** (crop
  0,0, line length tripled to 16128 so a 2500-byte line, ~4.9 µs on four lanes at ~1 Gb/s, fits its line time;
  frame length 3571, ~30 a second at 24 MHz) and **IMX616 res3 1632x1224** (its own crop 4,0, frame 5160): the
  sensors' full field of view and 2.5-3.7x the pixels for photographs. (`struct sony_cam` has the frame's
  width/height; `imx586_full` / `imx616_full` for level 7; `cam_full=0` keeps the crops.)
  - The camera block keeps 4 MB: a frame up to 1 or 2 MB alternates between two places as before. A bigger one
    (3,768,000 or 2,506,752 bytes at 16-byte strides 2512/2048) is written to one place every frame: it is copied
    out at frame-done (~1-2 ms) inside the vertical blanking.
  - redwood_camera.c: for the Sony sensors the session's mode is a copy of the S5K3L6's with their width and height
    (the header, the ring of 3, the copy). The exposure stays in the S5K3L6's terms; the kernel converts (IMX586
    line ratio now 919/1167).
  - mediaserverd needs no change: it offers iOS fixed formats (4032x3024 ...), renders them from any raw frame, and
    sizes its read from each frame's header.
  - **Result:** six short sessions front (1632x1224) → main (2000x1500) → ultra-wide (1028x772), twice, each "30
    frames handed over, 0 misses"; then **20 s from the main camera: 600 frames, 902,400 packets (1,504 a frame),
    ECC 0, CRC 0**, WM0 at one address (0xd8200000), SMMU gfsr 0; iOS running.
- **Test 351 (the t350 kernel; the standard image): iOS gets the whole frames.** mediaserverd: "frames of 2000x1500
  from the kernel (the Back Camera)", "frames of 1632x1224 from the kernel (the Front Camera)", auto-exposure and
  white balance on them, each session closed cleanly; Camera.app opened remotely (`openurl app:com.apple.camera`).
- **The missing Camera icon (owner, 2026-11-20):** Camera.app's `SBAppTags` is `still-camera`, which MobileGestalt
  takes from the loader's `isp` node, made only with `cam=5`. Test 350 (a kernel soak) booted without it, so
  SpringBoard hid the icon. `cam=5 cam_r5q=1` are now in `phone-args.txt`, the standard boot-args; a kernel-only
  camera soak drops them with `-cam -cam_r5q`.
- **Why mediaserverd was killed taking a HEIC photo (2026-11-20, Test 394; wifi/MEMORY-PLAN.md).** Not the HEIC
  encoder. With the Camera open, CMCapture's audio path makes a 39 MB CMBlockBuffer about 40 times a second
  (`CMCapture+0x37b90`). Each callback's 941 frames get 20,670,818 frames (468.7 s) added, from a field of the audio
  buffer's bookkeeping. mediaserverd reaches 2.7-3.7 GB until the session ends. Also, the iOS system image now
  leaves RAM after boot (md_flash), so the phone has about 3 GB more to start with. Open: what sets that field.
  Then retry the HEIC shutter and the lit-scene colours.
- **Fixed (Test 395):** libr5q-media.dylib sets BWAudioSourceNode's `_silenceFramesFillingDisabled`. Camera open:
  mediaserverd 75 MB. **A HEIC photo saves** (IMG_0004.HEIC, 1.56 MB), and **a video saves** (IMG_0005.MOV). The
  preview is still the purple, smeared picture (a lit scene still needs the owner).

- **Tests 396-398 (kernels t396-t398): THE MAIN CAMERA'S BLACK LEVEL, AND THE FLAT PURPLE STREAKS.**
  - New tools: `camgrab [SENSOR] [SKIP] [OUT] [LINES GAIN]` (wifi/camgrab.c, phone side) takes one raw frame from
    /dev/redwood-camera, optionally at an exposure of its own. `camraw.py FRAME` (PC side) unpacks RAW10, renders it
    half-size and prints per-line, per-column and per-row levels. Frames show the room: local only.
  - The HEIC photo (IMG_0004) and the raw frame agreed: the picture iOS makes is faithful to the sensor's data. The
    data itself was wrong.
    - A **dark frame** (4 lines, 1x) of the IMX586's full 2000x1500 mode (Test 350's: line length tripled to 16128)
      was not black: a level ramping from ~3 at the top to ~137 at the bottom, column stripes, 45% of pixels at 0.
      It scaled with gain and not with exposure, and R≈G≈B.
    - iOS is told black 0. With white balance R×2.1, B×1.8 that offset became the flat purple streaks.
    - The cropped mode (line length 10752) gives a dark frame of mean 1.2, flat.
  - **Test 397:** boot-arg `cam586_ll=N` sets the full frame's line length, with the frame length and iOS's line ratio
    scaled. At **10752** (frame 5356 lines, ratio 919/778, the same ~30 a second) the dark frame is mean 3, flat.
    The link stays clean: 1504 packets a frame, ECC 0, CRC 0. The top ~16 rows keep a small fading offset (~50→40).
  - **Test 398:** 10752 is the default (imx586_full). Standard image = r5q-install-boot-398.img (kernel t398, root
    test395). IMG_0007.HEIC saved; mediaserverd 102 MB.
  - **Test 396 (kept):** the Sony mode tables set `0x0220 = 0x62` and a short exposure of their own (`0x0224 = 500`;
    the IMX616 also a short gain, `0x0216`). The kernel now writes both exposures and both gains in one group hold.
    Measured afterwards: the exposure response (4 → 3000 lines) was about the same as before, so this was not the
    cause. It keeps the two in step.
  - Still for the owner: a lit scene, for colour (white balance and tuning). In a dark room iOS's auto-exposure sits
    at ~7 ms and 3-5x gain, and the preview is noise.

## The lit-scene look (owner, 2026-11-20): the back camera flashes white with lines, the front is clear

- **What the owner saw:** the phone stood up so both cameras see the room. The front camera's picture is fairly
  clear. The back camera's flashes white and is full of lines. Screenshots showed purple noise with horizontal
  streaks, then fully white: iOS's auto-exposure swinging.
- **Raw frames (camgrab, Camera closed; local only):**
  - The front IMX616 at 3000 lines and 4x shows the room: a dim room (at 400 lines and 1x it is black).
  - The back IMX586 at 50 / 400 / 2000 lines and 1x: means 70 / 80 / 85. A 40x longer exposure gave +15. Every
    frame has the same shape: a ramp from ~5 at the top rows to ~120 at the bottom, column stripes, no scene. At 8x
    the mean is ~230 (more than 8x the 1x level), and 3000 lines against 400 still adds only ~10.
  - The RAW10 packing is right: the fifth byte of each group averages 129, as random low bits do.
  - **New tool `torchset LEVEL`** (wifi/torchset.c): the back flashlight through /dev/redwood-torch, the device iOS's
    flashlight uses (the kernel's current cap, 30 s limit and temperature guards). With the torch on, the back
    camera's raw frame **shows the scene in front sharply** (exposure 400 lines, 1x). So the sensor, lens, link and
    unpacking work. The fault is a large offset that buries a dim room, and iOS's auto-exposure amplifies it into
    white. (The raw picture also reads mirror-wise left to right: to be checked against the Camera app once the
    picture is right; res5's `0x0101` is 0.)
- **A likely cause:** the IMX586's tables are made for a 24 MHz MCLK (`0x0136 = 0x18`; res5's pixel PLL: pre-divider 4,
  multiplier 360 = a 2160 MHz VCO). This phone feeds the 19.2 MHz crystal, so that VCO runs at 1728 MHz (80% of the
  design), and with it the sensor's pixel and ADC timing. Analog timing off its design fits an offset that grows
  with gain, a row ramp and column stripes. The front IMX616 runs at 19.2 MHz too, but its picture is clean.
- **Test 402 (kernel t402, root test401):** `struct sony_cam` gets `vt_mpy` / `exck`; `imx586_full_tune()` sets the
  IMX586's pixel PLL multiplier to **450** (VCO 2160 MHz again) and EXCK_FREQ to 19.2 MHz (`0x0136 = 0x1333`).
  Boot-arg `cam586_vt=N` picks another multiplier (300-500; 360 = the tables' as before). A line then takes 19.2/24
  as long: 6.22 us at line length 10752 (the link needs ~4.9). So iOS's line ratio becomes 919/622, and the frame
  (5356 lines) is 30 a second instead of ~24.
- **Test 402 result:** booted ("pixel PLL multiplier 450, EXCK 19.19 MHz: taken", line ratio 919/622).
  - At 1x and 2x (400 and 5300 lines) every pixel is now **exactly 64**: a pedestal like the other two sensors', but
    with no noise at all and no dim scene either. With the torch the scene stands on a clean 64.
  - From 4x the ramp and column stripes are back, above 64 (4x/5300 lines: mean 89; 8x: 142-181, values down to 0).
  - Reading: the sensor's black-level clamp now subtracts a large offset. Whatever is under the dim scene is still
    there, and below 4x it is clipped to the pedestal with the scene.
- **Next suspect, Test 403 (kernel t403):** the output (MIPI) PLL. The tables' 0x030d = 3 with multiplier 312 makes a
  2496 MHz VCO at 24 MHz. Ours (multiplier 160 at 19.2) is 1024 MHz, 41% of that, likely below its range. For the
  IMX586 now: the output system clock divider `0x030b = 2` with multiplier 390, so the VCO is 2496 MHz again and the
  lanes run at ~1.25 Gb/s (the IMX616 is clean at ~1.28). Boot-args `cam586_opdiv=1` (as before) and `cam586_iop=N`.
- **Test 403 result:** booted ("output PLL multiplier 390, divider 2"). The link stays clean (127,504 packets, ECC 0,
  CRC 0, 2,496,960 bytes a frame). The artifact roughly halved: 4x / 5300 lines mean 73 (was 89), 8x / 3000 lines 103
  (was 142-181), the same ramp and stripes. 1x and 2x still read exactly 64.
- **Next:** a shorter line. Test 397 showed the ramp grows with line length (16128 much worse than 10752). With both
  PLLs at their design VCOs the link needs ~4.0 us a line, the line at 10752 is 6.22 us. `+cam586_ll=8000` on kernel
  t403 makes it 4.63 us (frame 7199 lines, still ~30 a second).
- **`+cam586_ll=8000` on kernel t403 (line 4.63 us, frame 7199 lines, iOS's line ratio 919/462):**
  - **The ramp and column stripes are gone at every gain.** 4x / 8x / 16x: rows and columns flat at 64; the link is clean.
  - With the torch the scene is clean on 64.
  - Left: 98.8-99.9% of pixels read exactly 64, even at 16x, so the sensor's black-level clamp still takes off a little
    too much. A dim room's shadows are clipped to black. A brightly lit scene is needed to judge the colour.
  - The Camera app: the white flashing is gone. The dim room's preview is black with the hot pixels sparkling at iOS's
    16x gain. iOS was still told black 0 for the IMX586, though it now sits at 64.
- **Test 404 (kernel t404, root test401):** line length 8000 and frame 7199 are imx586_full's defaults. iOS is told
  black 64 for the IMX586 whenever the pixel-PLL fix is on (`redwood_r5q_cam586_black`, set in imx586_full_tune and
  read in redwood_camera.c); with `cam586_vt=360` it is 0 as before.
- Tool: `phone.py cycle` retries a flash refused with "a kernel is running". On every first cycle from a running iOS,
  step 2 had caught iOS's own live-mode listener before the restart. It now ends the run again, waits for the real
  restart, and flashes again (3 tries; the read-back check still guards).
- **Test 404 result:** booted. The first flash was refused and phone.py's new retry got it in by itself. Pixel PLL 450,
  line ratio 919/462; output PLL 390 with divider 2. The raw header says **black 64**.
  - 8x, 3000 lines (the dim room): mean 66.2 and noise again (min 29); rows 0-1 above black, rising to ~9 at the
    bottom tenth. That is a small leftover of the ramp, against ~200 on kernel t398.
  - The Camera app in the dim room: no white flash. iOS sits at its highest gain; the preview is dark on one side and
    noisy, streaky on the other (the sensor's bottom rows).
  - **Standard image = r5q-install-boot-404.img** (kernel t404, root test401).
  - Still to judge with the owner: a brightly lit scene (colour, white balance, whether the leftover streaks matter at
    normal gain), and whether the picture is mirrored left to right.
