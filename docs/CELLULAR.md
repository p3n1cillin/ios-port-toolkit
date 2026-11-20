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
