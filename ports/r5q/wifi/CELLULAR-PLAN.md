# Cellular on the S10 Lite under iOS (2026-11-20)

## How iOS reaches its baseband (desk work, the 20F66 system image, local only)

- **CommCenter speaks QMI.** `System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter` (24.7 MB) links
  `libQMIParserDynamic`, `libTelephonyBasebandDynamic`, `libTelephonyIOKitDynamic`, `libARI` (Intel basebands'
  protocol), `libBBUpdaterDynamic`, AppleBasebandManager and AppleBasebandServices. Its strings name QMI services and
  messages (`QMI_NAS_REGISTERED`, `QMI_DMS_...`, `QMI_BSP_DEBUG_LOG_...`), and it is entitled to
  `com.apple.driver.AppleBasebandPCI.user-access` / `...AppleBasebandPCIControl.user-access`. The iPhone X (iPhone10,3)
  has a Qualcomm baseband on PCIe; CommCenter talks QMI to it through Apple's AppleBasebandPCI kext.
- **This phone's modem offers the standard QMI services over QRTR** (node 0, Test 351's log): WDS 0x1, DMS 0x2, NAS
  0x3, QOS 0x4, WMS 0x5 (SMS), VOICE 0x9, CAT 0xa, UIM 0xb, PBM 0xc, LOC 0x10, WDA 0x1a, PDC 0x24 and more.
- **So the protocol matches.** A bridge would put an AppleBasebandPCI-like transport under CommCenter, carrying its QMI
  to the modem's QRTR services (QMUX client IDs emulated: QRTR has no CTL service), with AppleBasebandManager's boot
  states made to say "ready". Apple-only services CommCenter uses (`QMI_BSP`) are not on this modem: how far
  CommCenter gets without them is the open question.

## Test 352 (kernel t352, `cell=2`): THE MODEM'S RADIO WORKS

A kernel probe (redwood_wifi.c `cell_probe`, QMI over QRTR; it logs states only, no identifiers):
- SIM (UIM GET_CARD_STATUS, read before the modem went online): "2 card(s); the first ERROR ... 0 application(s)".
- **Modem operating mode: "shutting down" (5)** -- nothing had asked it to be online. DMS SET_OPERATING_MODE online:
  "taken".
- **Within 5 s it found GSM cells: "network type 2, radio: GSM", signal -80..-76 dBm**, but stayed at "searching" (not
  registered) for 45 s: with no usable SIM it cannot register.

## Test 353 (kernel t353): both SIM slots -- empty (the owner: no SIM in the phone)

- Both slots, before and after the modem went online (+10, +30, +45 s): "ERROR, error no ATR, 0 application(s)". The
  modem's UIM powers each slot and gets no answer-to-reset: an empty slot (the owner confirmed: no SIM). The modem
  again found GSM cells (network type 3GPP, -77 dBm) and stayed "searching".
- **Where cellular stands:** the modem runs, goes online on request, its radio receives, and its SIM interface
  works. Registration, calls, SMS and data need a SIM in the phone.
- **Next, the iOS side** (can start without a SIM; a SIM is needed to finish): put CommCenter on this modem.
  1. Learn what CommCenter and AppleBasebandManager open (IOKit classes and methods of AppleBasebandPCI /
     AppleBasebandPCIControl, the QMI channel framing they expect) from their binaries.
  2. A kernel service that answers those, carrying QMI to the modem's QRTR services (client IDs emulated).
  3. First milestone: iOS's status bar says "No SIM" from the real modem; then, with a SIM, registration.
- `cell=1` / `cell=2` stay in the kernel for checks (not in the standard args).

## The iOS side's transport (desk work, the 20F66 shared cache; tools/ios)

- `dsc_whose.py`: "qmux_control" is in `/usr/lib/libATCommandStudioDynamic.dylib`; "AppleBasebandPCIControl" in
  **`/usr/lib/libIOABP.dylib`** (base 0x1e92df000), the IOKit transport.
- **libIOABP's two classes** (`dsc_strings.py ... libIOABP.dylib`):
  - **`IOABPClass`**: matches services of class **`AppleBasebandPCIInterface`** by the property
    **`ABPInterfaceProtocol`** ("IOABPClass::start for %s": one service per channel; the channel names in the cache
    are `qmux_control`, `qmux_loc`, `qmux_extra`, `qmux_logchannel`, `qmux_test`), with first-match and terminate
    notifications. Methods: `open`, `close`, `read`, `readAsync`, `write`, `writeAsync`, `sendImage`,
    `sendImageAsync` (firmware), `readRegister`, `startChannelAsync`, `abortChannelAsync`, `timeSyncAsync`.
  - **`IOABPControlClass`**: opens **`AppleBasebandPCIControl`**: `open`/`close`, notification and event ports,
    `readLogsAsync`, `logSnapshotBufferSize`, `loggerTune`, `bootstage`.
- **The frames on a QMUX channel are classic QMUX** (another library's log: "queueing qmux pdu for svc=%d client=%d
  (txid=%d msgid=0x%x)"): service, client ID, transaction, message, as Linux's qmi_wwan/QMUX. This modem over QRTR has
  no QMUX: each QRTR port is a client, and the control service (0, client IDs) has to be answered by the bridge.
- **The bridge, concretely:** (1) kernel IOKit services `AppleBasebandPCIInterface` (one per QMUX channel, with
  `ABPInterfaceProtocol`) and `AppleBasebandPCIControl`; (2) their user clients answering libIOABP's selectors
  (numbers and argument layouts from its code: next); (3) QMUX -> QRTR: strip the QMUX header, one QRTR port per
  (service, client), answer CTL (get/release client ID, version info) here, and wrap the modem's answers and
  indications back in QMUX; (4) AppleBasebandManager's boot (it would load firmware with `sendImage` and watch
  `bootstage`): answered as "booted", the modem being already up.
- **libIOABP's selectors on an `AppleBasebandPCIInterface` connection** (`dsc_sel.py`, the `mov w1, #N` before each
  IOConnectCallMethod / IOConnectCallAsyncMethod; `w3` the scalar input count):
  0 open (1 scalar); 1 close; 2 write / writeAsync (2 scalars, input struct = the bytes); 3 read / readAsync (2
  scalars, output struct = the buffer, its size in/out); 4 sendImage(Async) (2 scalars, firmware); 5 readRegister (1);
  6 abortChannelAsync, 7 startChannelAsync, 8 timeSyncAsync (1 each); 9 mapMemoryAsync; 10-12 unmapMemoryAsync. The
  control connection (`AppleBasebandPCIControl`): 0 readLogsAsync, 1 flushTraceBuffers / start, 2 dumpRegistry, 3
  event notification / loggerTune / getRegistryInfo, 4 and 5 trace and log buffers, 6 demo, 9 connect.

## Test 354 (root test354: `ALLOW="locationd CommCenter"`, `OSLOG=... com.apple.CommCenter`): the donor's crash again

- CommCenter's two plists share a label and launchd picks by the device tree (`_LimitLoadToDeviceTree: baseband` for
  the cellular one, `_LimitLoadFromDeviceTree: baseband` for the non-cellular one); mkjobs.py drops the "To" condition
  for a job allowed by name. CommCenter starts, answers its XPC services, then **crash-loops every ~15 s: 12 SIGABRT,
  3 SIGSEGV** in 4 minutes. That is the donor's split: SIGSEGV after "Could not find AppleIPAppender service",
  SIGABRT after "Unable to communicate with AppleBaseband driver". Those lines are info-level, below what
  OS_ACTIVITY_DT_MODE prints here (two NSLog lines came through: "STK sessions are created").
- The second message is in **`libTelephonyBasebandDynamic.dylib`** (`TelephonyBasebandController`): it looks for the
  IOService **`AppleBaseband`** ("AppleBaseband IOService is not found. Finding it via matching service"), opens it,
  "self-identifies", then GetBasebandState / SetBasebandState, Get/SetBasebandProperty, ResetModem, PollGPIO,
  GetCoredumpReadyStatus, ReportThermalData, ResetSPMI, through a generic call wrapper
  (`_TelephonyBasebandControllerCallMethodGeneric`).

## Test 355 (kernel t355, `bbstub=1`): listening stubs

`iokit/Kernel/RedwoodBaseband.cpp` publishes, 5 s into the boot, services under the class names iOS matches --
`AppleBaseband`, `AppleIPAppender`, `AppleBasebandPCIControl`, and `AppleBasebandPCIInterface` for `qmux_control`,
`qmux_loc`, `qmux_extra`, `qmux_logchannel`, `qmux_test` (property `ABPInterfaceProtocol`) -- whose user client
answers every call with success and zeroed outputs and logs it (process, selector, scalars, structure sizes, first
bytes). The log then shows what CommCenter asks, in order, before anything is made to answer for real.
- **Test 355 result: COMMCENTER STOPS CRASHING.** With the stubs published, CommCenter runs as its first instance
  (pid 29, with CommCenterMobileHelper) and no "exited due to" in the whole run, where Test 354 had a crash every
  ~15 s. The phone's kernel buffer shows connections to `AppleBaseband` opened and closed. The stubs logged with
  IOLog, which reaches only the phone's message buffer (66 KB through `klog`), not the USB follower, so most of the
  call log was lost; Test 356 logs through printf.

## Test 357 (kernel t357): what iOS asks of the baseband services (the stubs' log, through the loader's ring)

- Published 5 s in: AppleBaseband, AppleIPAppender, AppleBasebandPCIControl, five AppleBasebandPCIInterface
  (qmux_control, qmux_loc, qmux_extra, qmux_logchannel, qmux_test).
- **CommCenter[28]**, in order:
  - AppleIPAppender: selector 4 (structure 8 in: `c4 09 00 00 00 00 00 00`).
  - AppleBaseband: selector 15 with a 64-byte name ("CommCenter: TelephonyBas..." = the self-identify), then selector
    32 (scalar 3).
  - AppleIPAppender: selector 8 (scalar -2), selector 16 x5 (scalars [n, 1], n = 0..4: per-interface), selector 6 (8
    out), 17 (1 in, 1 out), 9.
  - AppleBaseband: selector 3 (1 scalar out: **the baseband state**; the stub answers 0), 23 (1 out: a property), 22
    (1 in: set property). It reopens AppleBaseband and self-identifies (selector 15) every few seconds.
- **abm-helper[154]** (AppleBasebandManager's helper, so the cellular CommCenter path is running): opens
  AppleBaseband, self-identifies, asks the state (selector 3).
- **locationd** opens AppleBaseband too.
- **No AppleBasebandPCI* connection yet:** with the state 0, AppleBasebandManager does not go on to the boot stage
  and the QMUX channels. Next: which state value means "up" (AppleBasebandManager's boot states: BasebandBootStateIsReady,
  ...Booted, ...WillReset, ...DidReset). Selector 3's and 23's answers made writable sysctls, to be tried live.

## Test 358 (kernel t358): the stubs' answers set live; the state is not the gate

- `kern.redwood.bb_answer = selector << 16 | value` (redwood_wifi.c) sets the first scalar output the stubs return
  for a selector; `wifi/r5q_sysctl.c` now writes an integer (`r5q_sysctl NAME VALUE`; rebuild the phone tools with
  `pipeline/build_phone_tools.sh` before `phone.py tools`, which only uploads). CommCenter reads the state only when
  it starts, so each value was tried by restarting it (`r5qd_client.py kill CommCenter`; launchd respawns it).
- **AppleBaseband selector 3 answered 1, 2, 3, 4, 5, 6, 8: no AppleBasebandPCI* connection with any of them.** Each
  time: CommCenter opens AppleBaseband, self-identifies (15), selector 32 (3), 23, 22 (1), asks the state (3);
  abm-helper asks the state (3). On its way down CommCenter calls selector 7 and selector 22 (0). So AppleBasebandManager
  waits on something else before it starts the QMUX transport: an interest notification (the stubs accept a
  notification port and send nothing), a boot stage on AppleBasebandPCIControl, or a registry property. Next: read
  AppleBasebandManager's boot sequence (the framework and abm-helper) for what it waits on.
- **Seen once (after ~30 min with CommCenter on the stubs): Wi-Fi traffic stalled** (frames sent/received stopped,
  CE4 "wr 0 read 0"), r5qd silent, iOS otherwise up; a restart cleared it. Not yet known whether CommCenter (its
  AppleIPAppender interfaces) caused it. `bbstub` stays out of the standard args until that is understood.

## Where AppleBasebandManager decides (desk work, after Test 358) -- the next layer

- abm-helper (`ABMHelper.framework/Support/abm-helper`, 85 KB) is entitled to the user-client classes
  `AppleBasebandUserClient`, `AppleBasebandPCIUserClient`, `AppleBasebandPCIControlUserClient`,
  `AppleBasebandPCITraceUserClient` and Intel's `IOAppleConvergedIPCICEBB(Control)UserClient` (our stubs' class,
  RedwoodBasebandUserClient, was not refused: the sandbox's user-client class check is not enforced on this port).
- ABMHelper.framework is the trace/diagnostics side. The boot states are AppleBasebandManager's
  (`BasebandBootStateIsReady`, `...Booted`, `...WillReset`, `...DidReset`, `...WillShutdown`, `...HasShutdown`,
  `...Dead`, `...Unknown`; `EventTransportIsReady`); its server starts in `AppleBasebandManager.framework`
  (`AppleBasebandManagerStartServer`).
- The image carries the basebands' firmware: `usr/standalone/update/Mav17.Release.bbfw` (Qualcomm, iPhone10,3) and
  `ICE17.Release.bbfw` (Intel). `libBasebandCapabilities` maps radio types (Mav10..Mav30, ICE17..19, IBIS18) to
  firmware folders.
- **Open:** how AppleBasebandManager picks the radio type and when it starts the transport. The likely gate is the
  device tree's `baseband` node (which launchd already uses to choose CommCenter's variant, and which our loader does
  not make) or MobileGestalt's baseband answers. That decides between continuing with stubs (an AppleBasebandPCIControl
  that reports a boot stage and a QMUX transport that answers) and a loader change (a `baseband` node).

## Test 364 (loader `bbnode=1`: a "baseband" node in the device tree, compatible "baseband,mav17"; with the stubs)

- MobileGestalt reads `IODeviceTree:/baseband` (libMobileGestalt, two places; property names behind CFString constants,
  e.g. `baseband-board-snum` among its strings), and launchd picks CommCenter's variant by the node. The loader now
  makes the node with `bbnode=1` (port/xnuloader/loader.c, beside the camera's `isp` node).
- **Result: no change.** The same calls (CommCenter: AppleIPAppender 4/8/16x5/6/17/9/11, AppleBaseband 15/32/3/23/22;
  abm-helper: AppleBaseband 3) and no AppleBasebandPCI* connection. The node's presence is not the gate either.
- Next: who calls `TelephonyBasebandControllerGetBasebandState` and what it compares the answer with (the static way
  to the value, or the condition, that makes AppleBasebandManager go on to boot).
- `TelephonyBasebandControllerGetBasebandState` (libTelephonyBasebandDynamic +0x42ec) calls the library's generic
  wrapper (+0xca0) with method code 26 ("get baseband state"; 42 is "get coredump ready status", 25 the one before it),
  which the wrapper turns into the AppleBaseband selector seen on the wire (3). The value's meaning, and the condition
  for booting, are in AppleBasebandManager's server (AppleBasebandManager.framework, started by
  `AppleBasebandManagerStartServer`), the next thing to read; its client constants name the boot states but not
  their numbers.

## The gate, found (2026-11-20): "Baseband Firmware Path Not Found"

- **Where the manager's code is:** CommCenter hosts it (`abm::InitServer` / `abm::StartServer`, imported from
  AppleBasebandManager.framework). That framework is only a loader ("Failed on loading BasebandManager library for
  initializing server"): it opens **`/usr/lib/libBasebandManager.dylib`** (Qualcomm; `libBasebandManagerICE.dylib`
  for Intel). Its logs ("#I boot state changing %s -> %s", "Faking bootup", "Failed on preparation for booting!:
  Reason: %s", "Listening for PCI events", ...) are os_log formats, in the `__oslogstring` section;
  `tools/ios/dsc_strings.py` now lists that section too. abm-helper only hosts `abm::HelperServer` (logs, traces).
- **Its own word, on the phone:** `/private/var/wireless/Library/Logs/log-bb-live-stats.txt` (read with the new
  `r5qfs cat PATH`), one line per boot attempt:
  `reset: reason=modem boot up failure [Baseband Firmware Path Not Found]` ("Failed to find the firmware in %s").
  **That is why it never booted, whatever the stubs answered.** The path is fixed in libBasebandCapabilities:
  `/usr/local/standalone/firmware/Baseband/Mav17` (no override found), which an iPhone's restore fills from
  `usr/standalone/update/Mav17.Release.bbfw` (a zip, 34 MB: acdb, apps, mba, qdsp6sw (59 MB), rpm, sbl1, tz,
  restoresbl1, bbcfg .mbn, Info.plist, Options.plist; Qualcomm MDM9655 firmware) and which this port's system image
  never had.
- **Test 365:** the folder provided without rebuilding the 2.5 GB system image. The firmware tree (local only, never
  published; `qdsp6sw.mbn` a 4 KB placeholder, since the stubs ignore what is sent and the full one would not fit
  the 64 MB boot image) goes into the root as `/redwood/share/usr-local` (`EXTRA_SHARE`). A second `r5q_bind` in the
  init script (`rc.r5q.bbfw`, chained after the ringtones', still as process 1) shows it on `/usr/local`.
- **Test 365 result:** the bind works -- `/usr/local/standalone/firmware/Baseband/Mav17` lists the firmware files on
  the phone -- but the manager still wrote "Baseband Firmware Path Not Found" for this boot (5:26, the cycle began
  5:23).
- **Test 366:** the package added under the name an iPhone has there, `Mav17-<version>.Release.bbfw`
  (`Mav17-8.50.04.Release.bbfw`; the version from its Info.plist: 8.50.04, ChipId 0x0003B0E1), rebuilt with the 4 KB
  `qdsp6sw.mbn` placeholder (2.4 MB). **Still "Firmware Path Not Found"** (5:33). libBBUpdaterDynamic asserts "Failed
  finding the bbfw path." and wants an `apTicket` and `/bbupdate_profile.plist` further on.
- **What is not known yet:** whether the manager resolves this phone's radio type to Mav17 at all. The stats line
  reads `AP=20F66;BB=;Machine=iPhone10,3` (no baseband identity). The type comes from libBasebandCapabilities, likely
  through MobileGestalt (whose `baseband` node reads are `function-bb_ap_time_sync` and a hashed key,
  "5dyhCh3dm1vSOaNK+US1Qw"). The manager's own messages ("Failed to find the firmware in "%s"" with the path it
  tried) are info-level or go through CommCenter's logger, and do not reach the forwarded os_log here. Next: get
  those lines (a logging profile or the logger's preference), which would name the path it looks in.
