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
