# Camera feasibility: the S10 Lite's cameras in iOS 16.5 (20F66)

Read-only research, 2026-11-19. The phone was not touched. Paths are relative to the project root unless they are
WSL paths. Cache addresses are unslid VM addresses in `20F66__iPhone10,3_6/dyld_shared_cache_arm64`.

## TL;DR

* **iOS side: already solved by the donor.** The donor tree (`work/ios-r5q/donor-linux`, the POCO X5 Pro "redwood"
  port) has a working replacement for Apple's ISP plugin: `userland/src/camera_device.c` + `camera_isp.c`, built into
  `libredwood-media.dylib`. The r5q pipeline already injects that library into mediaserverd
  (`ios-port-toolkit/ports/r5q/pipeline/build_root_app.sh`, the `INJECT` line). It reads RAW10 frames from a kernel
  character device, `/dev/redwood-camera` (ABI in `xnu/files/bsd/sys/redwood_camera.h`), runs a small software ISP, and
  hands CMCapture a `FigCaptureDevice` with a back and a front `FigCaptureStream`. The donor's comments say its frames
  reach Camera.app's preview, the shutter, zoom and tap-to-focus. I could not check that on hardware from here.
* **The work is on the kernel side.** We need an r5q back end for `/dev/redwood-camera` that has the same frame
  header. That means porting the donor's `bsd/dev/redwood_camera.c` (Titan 165, SM7325) to the S10 Lite's Titan 175
  (SM8150). Several block addresses are the same: CSID-lite0 0xacc8000, IFE-lite0 0xacc4000, camcc 0xad00000, apps
  SMMU 0x15000000. The rest differs: pins, the PMICs (two S2MPB03 on bit-banged I2C instead of a PM8008), CSIPHY (v1.1
  at 0xac65000+), the PLL type, the RPMh rails (MMCX instead of CX) and the SMMU stream IDs.
* **Imitating Apple's ISP is not worth it.** `H10ISP.mediacapture` speaks a firmware command protocol to
  `AppleH10CamIn`: about 94 user-client selectors, and through selector 10 hundreds of `CISP_CMD_*` firmware commands.
  Emulating that means writing an ISP firmware stand-in.
* **First milestone:** `cam=5 cam_fake=1` on the S10 Lite. The kernel makes a synthetic RAW10 colour chart, and
  Camera.app shows it through the real iOS capture graph. No camera hardware is touched. The details and the
  preconditions are in section (d).

---

## (a) The camera hardware, from the live device tree (`work/ios-r5q/live.dts`)

### Sensors

The DT gives no sensor part names. The phone's public spec is: rear 48 MP wide with OIS, 12 MP ultra-wide, 5 MP
macro; front 32 MP. The "likely role" column is inferred and must be confirmed by reading the chip ID.
`sensor-position-yaw` 180 means rear, 0 means front.

| DT node (cell-index) | Likely role | Control bus | Slave addr (8-bit / 7-bit) | CSIPHY | MCLK | Reset / other GPIOs | Supplies (V) | Extras |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `qcom,cci@ac4a000/qcom,cam-sensor@0` (0) | rear wide 48 MP (OIS) | CCI0, master 0 (GPIO 17 SDA / 18 SCL) | 0x34 / 0x1A (Sony-typical) | 0 | MCLK0, GPIO 13, **24 MHz** (camcc clk 0x48) | reset GPIO 92; RCAM1_OIS GPIO 133 (shared with OIS, `cam-res-mgr` shared-gpios 13/92/133) | vdig 1.1 (S2MPB03#1 LDO1), vana 2.9 (#1 LDO6), vaf 2.8 (#1 LDO7), vio 1.8 (PM8150 L14), custom1 1.8 (#2 LDO3), custom2 1.8 (#1 LDO3), custom3 2.8 (#1 LDO5); cam_clk = titan_top_gdsc | actuator 0x18/0x0C, EEPROM 0xA0/0x50, OIS 0x48/0x24, all on CCI1 master 1 (GPIO 33/34) per the DT; flash `camera-flash@0` |
| `qcom,cci@ac4b000/qcom,cam-sensor@2` (2) | rear ultra-wide (likely) | CCI1, master 0 (GPIO 31/32) | 0x5A / 0x2D (Samsung S5K-typical) | 2 | MCLK1, GPIO 14, **19.2 MHz** (camcc 0x4a) | reset GPIO 22 | vdig 1.05 (#2 LDO2), vana 2.8 (#2 LDO6), vio 1.8 (L14) | EEPROM 0xA8/0x54 on GENI I2C `i2c@a80000`; flash `camera-flash@1` |
| `i2c@a88000/qcom,cam-sensor@20` (4) | rear macro (likely) | **GENI I2C** SE at 0xa88000 (not CCI) | `reg`/slave-addr 0x20 (8- vs 7-bit unclear) | 1 | MCLK3, GPIO 16, **19.2 MHz** (camcc 0x4e) | reset GPIO 21 | vdig 1.2 (#2 LDO4), vana 2.8 (#2 LDO7), vio 1.8 (L14) | EEPROM 0xA4/0x52 on `i2c@a80000` |
| `qcom,cci@ac4a000/qcom,cam-sensor@1` (1) and `@8` (8) | front 32 MP | CCI0, master 1 (GPIO 19/20) | 0x34 / 0x1A (Sony-typical) | 3 | MCLK2, GPIO 15, **24 MHz** (camcc 0x4c) | reset GPIO 82 | vio 1.8 (L14), vdig 1.05 (#2 LDO1), vana 2.9 (#2 LDO5) | EEPROM 0xA2/0x51 on `i2c@a84000` (shared with NFC). `@8` is a duplicate entry with the same pins and address, probably a second module source or a Samsung alias |

* **Camera PMICs:** two Samsung S2MPB03 chips, both at 7-bit address 0x56, each on its own bit-banged `i2c-gpio` bus.
  * #1 is `i2c@1` on GPIO 94/95: LDO1 1.1 V, LDO3 1.8, LDO5 2.8, LDO6/LDO7 2.8–2.9.
  * #2 is `i2c@2` on GPIO 47/48: LDO1/LDO2 1.05, LDO3 1.8, LDO4 1.2, LDO5 2.8–2.9, LDO6 2.8, LDO7 (2.8–2.9).
  * The NFC node `nq@28` also names GPIO 47 as its IRQ. Check the pin mux before driving bus #2.
* **Shared rails:**
  * The sensor I/O rail (VIO) is `pm8150_l14` (1.8 V), an RPMh VRM resource `ldoa14`.
  * The CSIPHY rail is `mipi-csi-vdd` = `pm8150l_l3`, 1.2 V (`csi-vdd-voltage` 1200000).
* **The data lane count is not in this DT.** CAMX takes it from the userspace sensor module XML. It must come from the
  vendor sensor module files (see the unknowns).
* **CCI timings** for a 37.5 MHz `cci_clk` are in the DT:
  * 400 kHz fast mode: thigh 0x26, tlow 0x38, tsu_sto 0x28, tsu_sta 0x28, thd_dat 0x16, thd_sta 0x23, tbuf 0x3e,
    trdhld 6, tsp 3;
  * 1 MHz fast-plus mode: thigh 0x10, tlow 0x16, and so on.

### The Spectra (Titan 175) camera subsystem blocks

Interrupt numbers are GIC SPI numbers; add 32 for the INTID. All blocks are powered by `titan_top_gdsc`
(camcc 0xad0c1bc) unless noted.

| Block | DT compatible | Base | IRQ (SPI) | Notes |
| --- | --- | --- | --- | --- |
| CPAS top + CAMNOC | `qcom,cam-cpas` | 0xac40000 (+0x1000), camnoc 0xac42000 (+0x5000) | 0x1cb | AXI ports cam_hf_1/2, cam_sf_1. Bus votes via msm-bus vectors (cam_hf_*_mnoc, *_camnoc) |
| CPAS CDM | `qcom,cam170-cpas-cdm0` | 0xac48000 | 0x1cd | not needed for direct register programming |
| CCI0 / CCI1 | `qcom,cci` | 0xac4a000 / 0xac4b000 | 0x1cc / 0x10f | cci_clk 37.5 MHz |
| CSIPHY0–3 | `qcom,csiphy-v1.1` | 0xac65000, 0xac66000, 0xac67000, 0xac68000 | 0x1dd, 0x1de, 0x1df, 0x1c0 | cphy_rx 400 MHz, csiNphytimer 300 MHz |
| CSID0 / CSID1 | `qcom,csid175` | 0xacb3000 / 0xacba000 | 0x1d0 / 0x1d2 | needs ife_0_gdsc (0xad0a004) / ife_1_gdsc (0xad0b004) |
| IFE0 / IFE1 | `qcom,vfe175` | 0xacaf000 / 0xacb6000 (+0x4000) | 0x1d1 / 0x1d3 | full pixel pipeline, also RDI |
| **CSID-lite0 / lite1** | `qcom,csid-lite175` | **0xacc8000** / 0xaccf000 | 0x1d4 / 0x167 | **titan_top_gdsc only**: the simplest raw path |
| **IFE-lite0 / lite1** | `qcom,vfe-lite175` | **0xacc4000** / 0xaccb000 (+0x4000) | 0x1d5 / 0x168 | RDI-only write masters; clocks 320–600 MHz |
| ICP (A5) | `qcom,cam-a5` | 0xac00000, 0xac10000, 0xac18000 | 0x1cf | firmware `CAMERA_ICP.elf`, **not needed for raw capture** |
| IPE0 / IPE1 / BPS | `qcom,cam-ipe` / `cam-bps` | 0xac87000 / 0xac91000 / 0xac6f000 | – | offline processing behind the ICP; not needed |
| JPEG enc / DMA, FD, LRME | – | 0xac4e000, 0xac52000, 0xac5a000, 0xac6b000 | – | not needed |
| camcc | `qcom,camcc-sm8150-v2` | 0xad00000 (+0x10000) | – | rails: `vdd_mm` = **mmcx.lvl**, `vdd_mx` = mx.lvl (RPMh ARC) |

* **SMMU:** the apps SMMU is at 0x15000000. The IFE context bank `msm_cam_smmu_ife` covers streams 0x840, 0x860,
  0xa40, 0xa60, 0xc40, 0xc60, 0xe40 and 0xe60, all with mask 0x620. The IFE IOVA pool starts at 0x7400000 and is
  0xd8c00000 long.
* **Reserved memory:** `camera_mem_region` is 0xc2000000 + 0x14000000. It is an ION pool. The UEFI memory map already
  keeps it out.

---

## (b) How iOS 16.5 reaches its camera (with evidence)

### Process and launchd jobs (`rootfs/System/Library/LaunchDaemons`)

| Job | Binary | Role |
| --- | --- | --- |
| `com.apple.mediaserverd` | `/usr/sbin/mediaserverd` | **Owns capture.** KeepAlive, user mobile. Mach services `com.apple.coremedia.capturesession`, `.capturesource`, `.cameraviewfinder`, `.flashlight`, `.figvirtualcapturecard.xpc`. Entitlements include `com.apple.camera.iokit-user-access`, `com.apple.runningboard.cameracapture` and `kTCCServiceCamera`. CMCapture is loaded inside it through MediaToolbox/Celestial. |
| `com.apple.applecamerad` | `/usr/sbin/applecamerad` | `H10ISPServicesAssistant`: ISP firmware loading, calibration and Pearl/FaceID analytics. Launched when IOResources gets the property `applecamerad = true`, which Apple's camera kext publishes. Not in the frame path. |
| `com.apple.dietapplecamerad`, `com.apple.dietappleh13camerad` | (the first is DarwinOS only; the second's binary is absent here) | not relevant |
| `com.apple.ptpcamerad`, `com.apple.corecaptured` | | PTP photo import, and CoreCapture diagnostics (Wi-Fi logs). **Not** the camera |

**There is no `cameracaptured` in iOS 16.5.** That daemon belongs to later releases. In 16.5, capture is inside
mediaserverd.

### How the plugin is chosen (CMCapture)

* `FigCaptureGetCaptureDeviceCreateFunction` is at 0x1a20a3a94. Its error string is
  `"<<<< FigCaptureCommon >>>> %s: Failed to load CaptureDeviceCreate function"`.
* It walks a 4-entry table at 0x1d7bb3e70, made of `{IOKit class, plugin path, create symbol}` triples:
  * `AppleH13CamIn`, `/System/Library/MediaCapture/H13ISP.mediacapture`, `H13ISPCaptureDeviceCreate`
  * **`AppleH10CamIn`, `/System/Library/MediaCapture/H10ISP.mediacapture`, `H10ISPCaptureDeviceCreate`** (iPhone X)
  * `AppleH9CamIn`, H9ISP...; `AppleH6CamIn`, H6ISP...
* For each entry, the helper at 0x1a20a3bb4 calls `IOServiceGetMatchingService(kIOMainPortDefault,
  IOServiceMatching(class))`. If the service exists, it calls `dlopen(path)` and then `dlsym(symbol)`.
* The plugin `H10ISP.mediacapture` lives only in the cache, at 0x1f049c000. There is no
  `/System/Library/MediaCapture` directory on the rootfs.
* `H10ISPCaptureDeviceCreate` (0x1f04cb284) builds the device with `CMDerivedObjectCreate(alloc, vtable 0x20ce687c0,
  FigCaptureDeviceGetClassID(), &dev)`. In that vtable:
  * the base class is version 1, storage 0x100, with invalidate at 0x1f04d47dc, finalize at 0x1f04f87e0,
    copyProperty at 0x1f04d3438 and setProperty at 0x1f04caea0;
  * the device class is version 3, starting with `H10ISPProcessingSessionCreate`, followed by request/relinquish
    streams.
  * The donor's `camera_device.c` uses this same layout: device = version 3 {processing_session, request_streams,
    relinquish_streams, pearl}; stream = version 2 {start, stop, add_attachments}.
* **Property surface:** CMCaptureCore exports 250 `kFigCaptureStreamProperty_*` keys and 12
  `kFigCaptureDeviceProperty_*` keys. Examples: `StreamArray`, `Clock`, `UnitInfo`, `SupportedFormatsArray`,
  `FormatIndex`, `PortType`, `VideoOutputConfigurations`, `VideoOutputHandlers`, `StillImageCaptureNow`,
  `TorchLevel`. A replacement only has to answer the subset that the capture graph actually asks for. The donor's
  device logs every unanswered key once.
* **Per-model data:** CMCapture reads `CMCapture.framework/D22/{AVCaptureSession.plist, CameraSetup.plist}` (D22 is
  the iPhone X).
  * Default sensor IDs: back 0x0343, front 0x0354, tele 0x0353, front IR 0x0d56.
  * AVCaptureDevices listed: Back, Back Telephoto, Back Dual, Front, Front Infrared, Front TrueDepth, audio.
  * The donor's streams name their formats so that they match this plist's back and front entries.
* **MobileGestalt** decides whether cameras exist from `IODeviceTree:/arm-io/isp` and `/product/camera`.
  libMobileGestalt contains those strings, plus `AppleH1..H13CamIn`.
  * The r5q loader (`ios-port-toolkit/ports/r5q/port/xnuloader/loader.c`) already adds those nodes when booted with
    `cam=5` or `torch=N`. Camera.app's `UIRequiredDeviceCapabilities` is only `arm64`.

### The `AppleH10CamIn` user-client protocol (what design C would have to imitate)

* **Opening:** `IOServiceOpen(AppleH10CamIn, task, type 42, &conn)` at 0x1f054d7d0 / 0x1f054d7d4. It is then followed
  by selector 3 with one scalar input, in `H10ISPDeviceOpen`.
* **Selectors:** the plugin exports C++ symbols, so each `IOConnectCall*` wrapper can be named. Below is the selector
  map, found by scanning the plugin's `bl` sites to IOKit and reading the `mov w1, #sel` before each one. Selectors
  that appear inside large functions (such as `ISP_PowerOnCamera`) are inlined wrappers; they are left out here.

| Sel | Call | Wrapper | | Sel | Call | Wrapper |
| --- | --- | --- | --- | --- | --- | --- |
| 0x3 | Scalar | H10ISPDeviceOpen | | 0x27 | Scalar | ISP_GetNumChannels |
| 0x4 | Scalar | H10ISPDeviceClose | | 0x28 | Scalar | ISP_LoadDataFile |
| 0x5 | Scalar | ISP_IsPowered | | 0x29 / 0x2a | Async | ISP_GeneralProcess / ...Generic |
| **0x6** | **Async** | **ISP_SendBuffers** (frame buffers to firmware) | | 0x2b–0x2e, 0x32 | Async | Firmware work-processor register / get / complete / release |
| 0x7 | Scalar | LoadFirmwareBinary | | 0x33 | Scalar | **ISP_InstallFakeSensor** |
| 0x9 | Scalar | ISP_ForgetFirmware | | 0x34 / 0x35 | Scalar | ISP_ReserveChannel / ReleaseChannel |
| **0xa** | **Scalar(4)** | **ISP_SendCommand(buf*, len, outlen, flags)**: 574 call sites | | 0x3a / 0x3b | Scalar | StartCamera / StopCamera(channel) |
| 0xc | Scalar | ISP_PowerOffCamera | | 0x3c | Scalar | ISP_CopySetfile |
| 0xd | Struct | ISP_GetCameraTime | | 0x3d | Scalar | ISP_IsFirmwareLoaded |
| 0xe | Method | ISP_GetModuleParams | | 0x3f / 0x40 | Scalar | Select/SetBestPixClockFrequencyIndex |
| 0xf | Struct | ISP_GetCameraStatus | | 0x41 / 0x57 / 0x5a | Struct | Pearl / Power / Jasper statistics |
| 0x10 / 0x24 | Scalar | Set/GetDriverLoggingFlags | | 0x43–0x45 / 0x0–0x2 | Scalar | Yonkers / Savage (secure-element) fusing |
| 0x11 | Scalar | ISP_ShowSharedMemoryAllocations | | 0x46 | Scalar | ISP_EnablePhotometer |
| 0x13 | Scalar | ISP_ProcessRawImageGo | | 0x47 | Scalar | ISP_EnableS2R |
| 0x14 | Scalar | ISP_GetSetfileSize | | 0x48–0x4b | Scalar | ISP_Spmi{Reg,Device}{Read,Write} |
| 0x15 | Scalar(3) | ISP_EnableSensorPower(ch, on, ...) | | 0x4c | Scalar | ISP_SetFirmwareBootArg |
| 0x16 | Scalar | ISP_SetFullStatsMode | | 0x50 | Scalar | ISP_UpdateMinion |
| 0x19 / 0x20 | Scalar | Select/SetBestMIPIFrequencyIndex | | 0x51–0x56, 0x58 | Scalar/Method | Jasper (ToF projector) clocks/params/analytics |
| 0x1a | Scalar | ISP_LoadFirmware | | 0x59 | Scalar | ISP_SetDriverProperty |
| 0x1b | Scalar | ISP_DriverInitiatedManualFocus | | 0x5b | Scalar | ISP_SetMemoryPoolID |
| 0x1c | Scalar | ISP_SetDARTCacheTTL | | 0x5c | Scalar | ISP_CalculateSerialNumberMismatch |
| 0x1d | Scalar | ISP_EnableMIPIClockAdjustments | | 0x5d | Struct | ISP_SetFrontCameraOffsetFromDisplayCenter |
| 0x1e | Scalar | ISP_OverrideSensorClockFrequency | | 0x1f | Method | ISP_GetSensorConfigInfo(ch, i, sCIspCameraConfig*) |
| 0x21 / 0x22 | Scalar | Create/TerminateClientLoggingSession | | 0x23 | Scalar | ISP_EnableFastISPClock |
| 0x26 | Scalar | ISP_GetVersion | | 0x2f | Scalar | ISP_EnableFastMemory |
| 0x30 / 0x31 | Scalar | Select/SetBestChargePumpFrequencyIndex | | 0x36 | Scalar | ISP_EnableFirmwareRecovery |
| 0x37 | Scalar | ISP_PearlCamProjectorSafe | | 0x38 / 0x39 | Scalar | ISP_PPMAdmissionCheck / PPMActivityStopped |

* **Example wrapper:** `ISP_SendCommand` is at 0x1f04abcac. It passes {pointer, length, out length, flags} as 4
  scalars to selector 10, between kdebug codes 0x31a02009 and 0x31a0200a. The kernel copies the command packet in from
  user memory.
* **Firmware commands:** on top of selector 10 the plugin sends firmware commands. Strings name many of them:
  `CISP_CMD_CH_CAMERA_CONFIG_SELECT`, `CISP_CMD_CH_BUFFER_POOL_CONFIG_GET`, `CISP_CMD_CH_MULTI_CAM_ENABLE`,
  `CISP_CMD_CH_FACE_DETECTION_CONFIG_GET`, AE/AF/AWB/LTM/OIS commands, `WritePropertyValue(ch, 0x140D/0x202/0x52A/...)`
  and others. It also expects per-sensor setfiles and calibration (`/usr/local/share/firmware/isp/cam%dsetfile.dat`,
  `h10_isp_fw.bin`, none of them on the rootfs), buffer pools (`CISP_POOL_META`, `CISP_POOL_RENDERED`, `CISP_POOL_FD`)
  and frame-done messages delivered asynchronously (`MyH10ISPFrameReceiverMessageProc`).

### Simpler capture paths that iOS 16.5 already has? None I found.

* **No UVC or external-camera path on the iPhone in 16.5.** No `UVC`/`USBVideo` image or string turned up in CMCapture,
  CMCaptureCore, AVFCapture or MediaToolbox, and no UVC dext is in `DriverExtensions`. External UVC cameras arrived in
  later iPadOS releases.
* **`AVVirtualCaptureCard` / `figvirtualcapturecard` is an output path**, not a camera source. It is tied to the asset
  writer and SharePlay/AirPlay (`kFigAssetWriterOption_ShouldUseVirtualCaptureCard`, `isVirtualCaptureCardSupported`).
* **`CMCapture.framework/T8110SIM`, `T8112SIM`, `T8120SIM`** are tuning sets for Apple's ISP simulation platforms. They
  still go through an `AppleH*CamIn` plugin, so they do not help us.
* **`ISP_InstallFakeSensor` (selector 0x33)** is a fake sensor inside Apple's ISP firmware. It is only reachable if
  you already emulate `AppleH10CamIn`.
* **The real simple seam** is the plugin create function itself: `BWFigCaptureDeviceVendor
  -initWithDeviceCreateFunction:`. That is where the donor plugs in its own device, without any `AppleH10CamIn`
  service.

---

## (c) Candidate designs, ranked by effort (lowest first)

All three designs need the same kernel work to get pixels off the sensor (CCI, CSIPHY, CSID-lite, IFE-lite RDI to
memory). They differ in how the pixels reach iOS. The hardware recipe after the designs applies to all of them.

### A. Reuse the donor's userland plugin replacement; write an r5q back end for `/dev/redwood-camera` (recommended)

* **iOS side, already built and already injected on r5q:**
  * `libredwood-media.dylib` (`inject_media.c`) runs inside mediaserverd. When `/dev/redwood-camera` exists, it makes
    `BWFigCaptureDeviceVendor` with `redwood_camera_device_create`.
  * `camera_device.c` implements the `FigCaptureDevice` and its two `FigCaptureStream`s (properties,
    `VideoOutputHandlers` blocks, per-frame metadata so that the still-capture state machine advances, focus, torch).
  * `camera_isp.c` is the software ISP: black level, lens shading, white balance and AE gains, colour matrix, sRGB
    gamma; 2x2-cell preview and bilinear demosaic for stills.
  * It refuses video stabilization and the ISP processing session.
  * Per-r5q changes:
    * the `cameras[]` table (focal length, f-number, pixel size, field of view, the `turned` flag, the sensor index);
    * `load_tuning()` names (`/redwood/fw/camera-<sensor>.isp`, made by `tools/camtuning.py` from
      `com.qti.tuned.*.bin`);
    * the description strings.
  * Keep the frame-header ABI and the sensor numbering (0 macro, 1 wide, 2 front, 3 ultra-wide). Then nothing else
    changes.
* **Kernel side, new:** an r5q `redwood_camera.c` with the same `/dev/redwood-camera` read/write ABI (`RWFRAME1`
  header, commands EXPOSURE, SENSOR, FOCUS, KEEP). Port it from the donor's file and change these:
  * Pins: TLMM is at 0x03000000 on SM8150, not the donor's 0x0f100000. MCLK GPIOs 13–16, CCI GPIOs 17–20 and 31–34,
    resets 92/82/22/21. The macro sensor's control bus is a GENI I2C SE (0xa88000). The project already bit-bangs GENI
    I2C elsewhere.
  * Supplies: drop the PM8008i and SPMI GPIO code. Use the two S2MPB03 chips over bit-banged I2C (GPIO 94/95 and 47/48,
    address 0x56), plus RPMh VRM votes for `ldoa14` (VIO 1.8 V) and `ldoc3` (CSIPHY 1.2 V).
  * RPMh rails: **MMCX** (`mmcx.lvl`) and MX instead of the donor's CX and MX. Use the existing `redwood_wifi.c` RPMh
    helper.
  * camcc: titan_top_gdsc is at offset 0xc1bc (same layout). SM8150's camcc PLLs are Trion type, not the donor's
    Lucid. 19.2 MHz MCLK can come straight from XO; that suits sensors @2 and @20. 24 MHz (wide, front) needs
    cam_cc_pll2 or similar. Take RCG and branch offsets from `camcc-sm8150.c` (Qualcomm msm-4.14, or mainline if
    present).
  * GCC: camera_ahb, camera_hf_axi, camera_sf_axi (from `gcc-sm8150.c`).
  * CSIPHY: v1.1 at 0xac65000 + 0x1000·n. Use the `cam_csiphy_1_1_hwreg.h` lane tables. The donor's D-PHY code is
    for a newer PHY revision.
  * CSID-lite0 (0xacc8000) and IFE-lite0 (0xacc4000) have **the same base addresses as on the donor**. Register
    layout: `cam_ife_csid_lite17x.h`, and for IFE-lite `cam_vfe_lite17x.h` with bus ver2. The donor used
    `cam_vfe_lite16x.h` with bus ver2, so diff the two headers.
  * SMMU: streams 0x840 etc. with mask 0x620 (the donor's were 0x800 / 0x4e0). Follow the r5q SMR slot rules in
    `toolkit/docs/SENSORS.md`: Wi-Fi takes the lowest free slots, FastRPC the highest, the microphone a third range.
    The camera needs its own slot, and must copy the S2CR of a bank that already runs with translation off.
  * Sensor tables: make `camera-<sensor>.tab` (RWCTAB02) with `tools/camsensor.py` from the S10 Lite's own vendor
    sensor module files. These are vendor data: generate them locally and never publish them.
* **Effort:** the iOS side is small (hours to a day). The kernel side is the bulk: comparable to the donor's 4.3 k
  lines, but mostly re-targeting. Expect the same test ladder the donor used: cam=1 look, cam=2 chip ID, cam=3 CSID test
  pattern, cam=4 sensor frames, cam=5 iOS.
* **Biggest unknowns:**
  * Whether Samsung's SM-G770F vendor partition has CAMX `com.qti.sensormodule.*.bin` files that `camsensor.py` can
    parse (needed for the init, mode and power tables and the lane count), or Samsung-specific equivalents.
  * Whether MMNOC/CAMNOC bandwidth votes (RPMh BCM) are needed for IFE writes to land on SM8150. The donor got away
    with CX/MX only.
  * Whether the hypervisor allows the SMMU writes for the IFE streams.
  * The front and wide sensors' PLL-sourced 24 MHz MCLK.
  * CPU cost of the software ISP at preview sizes. The donor's preview uses one pixel per 2x2 cell, so that should be
    fine on the SM8150's big cores.

### B. Inject frames above the capture stack (per-app AVFoundation hooks)

* **What it is:** swizzle `AVCaptureSession`, `AVCaptureVideoPreviewLayer` and `AVCapturePhotoOutput` inside
  Camera.app (and each other app) to show and return frames read from the kernel device.
* **Kernel side:** the same as A.
* **iOS side:** new, fragile, per app, and it bypasses CMCapture. Other apps (FaceTime, Scanner, third-party apps) need
  their own hooks. Photo metadata, HEIC encoding and video recording each have to be faked separately.
* **Effort:** more than A, because A already exists. Only worth it as a fallback if mediaserverd injection broke.
* **Unknowns:** Camera.app's private CameraKit/CameraUI paths. They talk to mediaserverd over XPC
  (`com.apple.coremedia.capturesession`), not just through public AVFoundation.

### C. Imitate Apple's ISP: an `AppleH10CamIn` IOService and user client in the kernel

* **iOS side:** nothing to write. Stock `H10ISP.mediacapture` and CMCapture run unmodified.
* **Kernel side:**
  * An IOService named `AppleH10CamIn` (the r5q overlay already has `redwood_publish_named_service`) with a user client
    of type 42.
  * The roughly 94 selectors listed above.
  * Most of all, a stand-in for the ISP firmware behind selector 10: hundreds of `CISP_CMD_*` commands, each with
    exact packet layouts and replies; channel and setfile handling; buffer-pool management through selector 6;
    frame-done notifications through async ports; AE/AF/AWB statistics and metadata dictionaries; still and bracketing
    flows; plus Pearl, Jasper and Savage/Yonkers calls answered convincingly.
  * The ISP processing itself (Bayer to YCbCr, scaling) must also happen, on the CPU or in Qualcomm's full IFE pixel
    pipeline.
* **Effort:** very large, many months. Nothing public documents the CISP packet formats; every command would have to
  be reverse engineered from the plugin and from what CMCapture expects.
* **Unknowns:** essentially all of the protocol. Not recommended.

### The hardware recipe shared by all designs: streaming raw Bayer without the ICP

* **What is not needed:** the ICP / A5 (`CAMERA_ICP.elf`), IPE, BPS and CDM. The ICP only runs offline IPE/BPS
  processing. Raw capture is direct register programming of CSIPHY, CSID and IFE. That is how Qualcomm's own
  `cam_isp` RDI path works, and how mainline camss works on SDM845 (Titan 170, RDI only).
* **Mainline references (Linux):**
  * `drivers/media/platform/qcom/camss/`: `camss-csiphy-3ph-1-0.c`, `camss-csid-gen2`/csid-170 code, `camss-vfe-170.c`
    (the VFE and VFE-lite RDI write masters);
  * `drivers/i2c/busses/i2c-qcom-cci.c`;
  * `drivers/clk/qcom/camcc-sm8150.c` (check it is upstream in your tree) and `gcc-sm8150.c`.
  * Downstream `techpack/camera` for SM8150 (msm-4.14): `cam_csiphy_1_1_hwreg.h`, `cam_ife_csid_lite17x.h`,
    `cam_vfe_lite17x.h`, `cam_vfe_bus_ver2.c`.
* **Bring-up order** (the donor's, which worked on SM7325):
  1. RPMh votes: MMCX and MX levels, and the VIO and CSIPHY LDOs.
  2. GCC camera AXI/AHB clocks.
  3. titan_top_gdsc.
  4. camcc PLL (if 24 MHz is needed), then the RCGs and branches: slow_ahb, camnoc_axi, cpas_ahb, cphy_rx,
     csiNphytimer, csiphyN, ife_lite_N_csid, ife_lite_N_cphy_rx, ife_lite_N, mclkN.
  5. The SMMU SMR and S2CR (bypass, or a stage-1 context of our own as the donor made).
  6. The sensor's power sequence (from its module file), MCLK, reset; chip ID over CCI (or the CCI pins bit-banged,
     as the donor did).
  7. CSID-lite TPG to RDI0 to IFE-lite WM0 to memory, with no sensor involved. **This proves clocks, SMMU and bus.**
  8. CSIPHY lane config (settle count from the data rate), the sensor's init, mode and stream-on tables, CSID RX
     config (lanes, PHY select, VC 0 / DT 0x2B RAW10), RDI0 into IFE-lite WM0 ping-pong buffers.
  9. A ring of frames exported to `/dev/redwood-camera`.

---

## (d) Recommended first milestone

### M0: `cam=5 cam_fake=1` end to end on the S10 Lite, with no camera hardware touched

In the donor's `redwood_camera.c`, `cam=5 cam_fake=1` takes this path:

* `camera_thread()` checks `fake` and calls `serve_ios()` **before any `ml_io_map`**. The log line is "nothing of the
  phone is touched".
* `fake_session()` writes a RAW10 colour chart, at the brightness the requested exposure implies, into the frame
  ring.
* The rest is real: `/dev/redwood-camera` → libredwood-media in mediaserverd → `camera_device.c` → the CMCapture graph
  → Camera.app's preview and shutter.

Use `RC_JOBS=rc.jobs.camera`: `camera_avtest` lists the AVCaptureDevices, then a scripted tap opens Camera.app. The
loader's `/arm-io/isp` node (r5q loader, `cam=5`) gives MobileGestalt its video-camera answer and the icon.

Preconditions to check before running. They are build-side and read-only; no phone needed.

1. **That the r5q kernel still contains the donor's `redwood_camera.c` and its init call.** The build applies the
   donor's `redwood.patch` and files. The call is `redwood_camera_init()` from the donor's `redwood_audio.c`. Check
   that the r5q build compiles that file.
2. **A table file named after the donor's wide sensor.** `ios_select()` still calls `ctab_load()` in fake mode.
   Provide `camera-s5khm2sd.tab`, the name `sensors[1].tab` uses, through whatever `redwood_wifi_file()` serves on r5q.
   It can be a tiny synthetic RWCTAB02: a header plus one mode such as 4000x3000 RAW10 (DT 0x2B), 4 D-PHY lanes, 30
   fps, frame length > 0. Zero power or register tables are needed in fake mode. Alternatively, rename the entry in an
   r5q port.
3. **Do not use any other `cam=` level on r5q with the donor driver.** `cam=1..4`, and `cam=5` without `cam_fake`, map
   POCO X5 Pro addresses: TLMM 0x0f100000, PMIC arbiter 0x0c440000, CSIPHY 0x0ace0000+. On SM8150 those are wrong
   blocks.
4. Memory: about 3 × 15 MB of RAW10 ring, plus a 24 MB keep buffer by default (`cam_keep_mb`). This is fine.

**M0 passes when** Camera.app shows the colour bars and grey ramp and a photo can be taken. That proves the whole iOS
half on this phone and this build, and fixes the kernel ABI that the real driver must meet.

### After M0, the hardware ladder (one sensor first)

Start with the **rear sensor @2** (likely the ultra-wide): CCI1 master 0 (GPIO 31/32), slave 0x5A, CSIPHY2, MCLK1 at
19.2 MHz from XO (no camcc PLL), supplies from S2MPB03#2 (LDO2 1.05 V, LDO6 2.8 V) plus L14.

| Step | Boot-arg (r5q driver) | What it proves |
| --- | --- | --- |
| M1 | `cam=1` | Read-only: camcc / GDSC / pin state, both S2MPB03 chips answer at 0x56, SMMU SMRs free and the IFE streams uncovered |
| M2 | `cam=2` | Power-up sequence plus the chip ID over CCI. This identifies the part, which the DT does not name |
| M3 | `cam=3` | CSID-lite0 test pattern → RDI0 → IFE-lite0 WM0 → memory, pulled to the host. This proves rails, clocks, SMMU and bus with no sensor involved |
| M4 | `cam=4` | The sensor's RAW10 frames through CSIPHY2 into memory |
| M5 | `cam=5` | Real frames in Camera.app (M0's path with real data). Then the wide sensor (needs the 24 MHz PLL, plus OIS, actuator and EEPROM on CCI1 m1) and the front sensor |

## What I could not determine

* **The exact sensor part numbers.** The DT has no names. The roles in the table are inferred from slave addresses,
  OIS/actuator presence and cell-index order.
* **Lane counts and data rates per mode.** They are not in the DT. They must come from the vendor sensor module
  files, and I could not check whether Samsung ships parseable CAMX `com.qti.sensormodule.*.bin` files. No vendor
  camera files are on this PC.
* **Whether `camcc-sm8150.c` is in the mainline tree you would read**, and the exact Titan 175 vs 165 register deltas
  for IFE-lite. I named the downstream headers to diff, but did not diff them.
* **Whether CAMNOC/MMNOC bandwidth votes are mandatory on SM8150** for IFE writes.
* **The user-client class name of `AppleH10CamIn`** (not in the plugin's strings), and the CISP packet formats. Only
  design C needs them.
* **Whether CMCapture copes with an iPhone X profile that lacks the tele, dual and TrueDepth devices** beyond what the
  donor saw. The donor ran with back and front streams only.
* **Whether the r5q kernel currently compiles the donor's camera driver.** It is very likely, given that the build
  copies the donor files and applies `redwood.patch`, but I did not open the build log.

## Evidence index

* **DT:** `work/ios-r5q/live.dts`, lines 3570–4126 (CSIPHY, CCI, sensors, actuator/EEPROM/OIS), 4127–4780 (SMMU,
  CPAS, CSID/IFE, ICP/IPE/BPS, JPEG/FD/LRME), 5498 (camcc), 25614–25780 (GENI I2C EEPROMs and sensor@20), 29280–29410
  (S2MPB03 chips), 30075–30095 (flash), 30499 (camera_mem_region).
* **iOS rootfs** (WSL `/var/tmp/codex-ios-r5q/work/linux-source/ios165/rootfs`): `System/Library/LaunchDaemons/`
  `com.apple.{mediaserverd,applecamerad,dietapplecamerad,dietappleh13camerad,ptpcamerad,corecaptured}.plist`;
  `usr/sbin/mediaserverd` entitlements; `usr/sbin/applecamerad` strings;
  `System/Library/PrivateFrameworks/CMCapture.framework/{D22,D221,T81xxSIM}/`.
* **dyld cache:** CMCapture 0x1a1e96000 (selector function 0x1a20a3a94, table 0x1d7bb3e70, helper 0x1a20a3bb4);
  H10ISP.mediacapture 0x1f049c000 (create 0x1f04cb284, device vtable 0x20ce687c0, `ISP_SendCommand` 0x1f04abcac,
  `IOServiceOpen` type 42 at 0x1f054d7d4); IOKit 0x18e537000; libMobileGestalt camera strings.
* **Donor:** `work/ios-r5q/donor-linux/xnu/files/bsd/dev/redwood_camera.c`, `xnu/files/bsd/sys/redwood_camera.h`,
  `userland/src/{inject_media.c,camera_device.c,camera_isp.c}`, `userland/rc.jobs.camera`,
  `tools/{camsensor.py,camtuning.py,camraw.py}`.
* **r5q:** `ios-port-toolkit/ports/r5q/pipeline/build_root_app.sh` (libredwood-media injected into mediaserverd),
  `ios-port-toolkit/ports/r5q/port/xnuloader/loader.c` (`/arm-io/isp` with `cam=5`),
  `ios-port-toolkit/ports/r5q/kernel-build/build-xnu-sandbox.sh` (donor files plus the r5q overlay).
