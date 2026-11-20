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
