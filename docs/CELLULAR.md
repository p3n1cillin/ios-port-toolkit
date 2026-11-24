# Cellular: iOS's telephony stack on a Qualcomm Android phone's modem

The S10 Lite port got this far (November 20, 2026, Tests 352-388): CommCenter and its baseband manager run on the
phone's own Qualcomm modem. The manager reports the modem booted and ready, and CommCenter's QMI clients exchange real
QMI with the modem's services. The SIM state, IMEI and modem firmware version come from the modem. The full record is
`ports/r5q/wifi/CELLULAR-PLAN.md`. This page is the recipe for another Qualcomm phone.

## The shape of it

```
CommCenter (QMI clients, libATCommandStudio QMux)  ->  libBasebandManager (router, "MAV_ROUTER_QMI_0/1")
   -> libPCITransport / libIOABP  ->  IOKit AppleBasebandPCIInterface (protocol "qmux_test")   [kernel stub]
   -> kernel: QMUX frames in, QMI CTL answered locally, every other service's QMI to the modem over QRTR
   -> the modem's own QMI servers (NAS, DMS, UIM, WDS, WMS, Voice, ...), node 0
```

On an iPhone, Apple's baseband sits behind PCIe and speaks QMUX. Here, kernel stubs publish the IOKit services iOS
matches by class name:
- `AppleBaseband`
- `AppleIPAppender`
- `AppleBasebandPCIControl`
- `AppleBasebandPCIPDPManagerBase`
- `AppleBasebandPCIInterface` (one per `ABPInterfaceProtocol`)

## Steps, in the order they unblocked things

1. **Firmware and boot.** The manager wants Apple's baseband firmware package and a per-device `bbticket.der` to flash
   the modem with BBUpdater. Don't forge a ticket. The modem is already running, so take the manager's own
   **fake-boot** path instead: libBasebandManager's bootup branch (`cbz w0` after a libBasebandCapabilities call that
   always returns 1), patched at read time behind a boot-arg. See docs/SYSTEM-PATCHING.md.
2. **PCI event listener.** It needs `AppleBasebandPCIControl` and `AppleBasebandPCIPDPManagerBase` services.
3. **Engage.** After a fake boot nothing engages the IPC router. A small library injected into CommCenter finds the
   QMITransportService object by its vtables and calls engage. (Open: after a reset, the manager's own engage fails.)
4. **Async transport.** Implement these on the interface:
   - `startChannelAsync` (selector 7) and `abortChannelAsync` (selector 6): complete them with
     `sendAsyncResult64(ref, kIOReturnSuccess)`.
   - Async read (selector 3: user buffer, size): hold it until there's data.
   - Async write (selector 2: user buffer, length): copy the bytes in with
     `IOMemoryDescriptor::withAddressRange(addr, len, dir, task)`.

   Completions carry two arguments, `{bytes, 0}`.
5. **QMUX framing.** Each frame is `01 <len u16 = frame - 1> <flags: 00 out / 80 in> <service> <client>` followed by
   the QMI message (`flags, txid u16, msgid u16, len u16, TLVs`). That message is byte for byte what QRTR QMI carries,
   so the bridge passes it through unchanged.
   - Answer QMI CTL (service 0) in the kernel: SYNC 0x27, 0x26, 0x2a, GET_CLIENT_ID 0x22 (TLV 01 = service, client).
   - Give each (connection, service, client) its own QRTR port. The kernel needs room for many: about 60 clients.
   - Several frames may go in one read.
6. **Things this modem must not get, or must not be asked:**
   - Apple's vendor services (0xe1, 0xe2, 0xe4, 0xe7, 0xe8 ...): reply with a QMI error, with an exact frame length.
     A one-byte error there misaligns the whole stream.
   - COEX (0x22): it knocked the phone's Wi-Fi off (the Wi-Fi firmware runs beside the modem).
   - PDS (6): locationd asks for a baseband reset on any answer it doesn't like. Refuse PDS client IDs.

## Seeing what happens

Everything above was found from iOS's own log, read on the PC:
- `tools/ios/tracev3.py`, with logging preferences that turn on private data and Debug level per subsystem
  (`com.apple.telephony.abm`, `com.apple.CommCenter`, `com.apple.telephony.bb`);
- the kernel stubs' call log.

The `qmux` category logs every QMI frame CommCenter sends and parses (`QMI: Svc=... Req/Resp MsgId=... Bin=[...]`),
which makes framing bugs easy to see.

## Keeping the modem alive: GLINK intents and servreg acknowledgements (Tests 451-452)

Everything on the modem's IPCRTR channel -- CommCenter's QMI, GPS, the Wi-Fi firmware's WLFW, servreg -- needs a
free *intent* of the modem's (a receive buffer it advertised) for each message sent. Under load the modem can hold
its intents for seconds. Two rules:

- **When no intent is free, ask for one (GLINK INTENT_REQUEST), as Linux's `__qcom_glink_send()` does.** The S10
  Lite's sender asked only when none was big enough and otherwise waited; under a large Wi-Fi transfer every QMI send
  failed with ENOBUFS for ~20 s.
- **Treat servreg acknowledgements as fatal if lost.** The modem's service registry waits for the AP to acknowledge
  each domain state change (e.g. `msm/modem/wlan_pd` going DOWN when the Wi-Fi firmware restarts). If the
  acknowledgement cannot be sent, the modem kills itself: SMP2P `err-fatal`, SMEM item 421
  "servreg_monitor.c:502:Servreg ack timedout". Check the send's result and retry; log it.

Test the starved path on purpose rather than waiting for it: a switch that makes the sender use only intents it
asked for (r5q: `glink_starve=1`), together with a forced Wi-Fi firmware crash (`wifi_fw_crash=S`), exercises both
rules in one boot.

## Without the injected helper, and the UI (Tests 389-391)

- **Engage from boot.** The manager engages its router after a baseband reset, but not after its first fake boot.
  The kernel sends one PCI event (an IOGeneralInterest message `0xe3ff8100`, argument `{u32 event; u64; u64}`) on
  AppleBasebandPCIControl about 20 s after CommCenter starts (boot-arg `bbkick`). The manager resets and engages by
  itself. For that to work, abortChannelAsync must complete the reads still posted (kIOReturnAborted); otherwise
  every engage after a reset fails.
- **Settings' Cellular section.** MobileGestalt's `cellular-data` is `HasBaseband`, which means an IODeviceTree entry
  named `baseband` exists. Give the device tree a node like the real phone's (the IPSW's DeviceTree, with identifiers
  left zero). Then delete MobileGestalt's cache (`systemgroup.com.apple.mobilegestaltcache/.../com.apple.MobileGestalt.plist`)
  so it recomputes. `mgq` and `dtq` (tiny phone tools) show the answers and the device-tree entries.
