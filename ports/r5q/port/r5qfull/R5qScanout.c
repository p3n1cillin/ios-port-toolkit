//
// r5qfull (Test 54+): make the panel show the framebuffer everyone draws into.
//
// UEFI's GOP (Silicium's fixed 0x9C400000), the boot manager and iOS's framebuffer driver all draw at 0x9C400000,
// but the panel shows whatever the display pipe the stock bootloader left running fetches. That bootloader logs
// "Could not get splash memory region node" and places its own buffer, so the pipe's address can differ from
// 0x9C400000 by an amount that changes from boot to boot. The picture then appears shifted by that much: a
// constant shift with every row's start wrapped to the right edge (Test 35's photo: 3 characters; Tests 43 and
// 51: the Apple logo at the top right). BootShim's own bands, drawn at the address it scanned, were always right.
//
// Fix: every pipe fetching from DRAM is pointed at 0x9C400000 (the source address register is double-buffered:
// it is taken at the start of the next frame after a flush of the pipe's bit in its CTL path, and the panel's
// autorefresh sends frames continuously). Everything is logged first: BootShim's record, the CTL path, each pipe.
//
// Included into FirstBootManager.c (uses LogSec and ReadCntPct/ReadCntFrq).
//

#define R5Q_MDSS_BASE    0x0AE00000ULL
#define R5Q_SCAN_FB_PA   0x9C400000U
#define R5Q_BOOTSHIM_INFO 0x9E300000ULL

STATIC UINT32  mR5qScanOld;      // the address the panel's pipe fetched before (0: none found)
STATIC UINT32  mR5qScanPipes;    // pipes re-pointed
STATIC UINT32  mR5qScanTaken;    // 1: the hardware took the new address

STATIC VOID
R5qFixScanout (
  VOID
  )
{
  STATIC CONST UINT32  Pipe[8]  = { 0x05000, 0x07000, 0x09000, 0x0b000, 0x25000, 0x27000, 0x29000, 0x2b000 };
  STATIC CONST UINT32  Flush[8] = { 1u << 0, 1u << 1, 1u << 2, 1u << 18, 1u << 11, 1u << 12, 1u << 24, 1u << 25 };
  STATIC CONST CHAR8   *Name[8] = { "vig0", "vig1", "vig2", "vig3", "dma0", "dma1", "dma2", "dma3" };
  volatile UINT32      *Info = (volatile UINT32 *)(UINTN)R5Q_BOOTSHIM_INFO;
  UINT64               Ctl   = 0, Base, T0, F;
  UINT32               I, N, Addr, FlushBits = 0;
  CHAR8                L[200];

  AsciiSPrint (L, sizeof (L), "[SCAN] BootShim info: magic %08x fb %08x pipe %u stride %08x size %08x format %08x "
    "te %08x autorefresh-was %08x ctl %u/%08x\n", Info[0], Info[1], Info[2], Info[3], Info[4], Info[5], Info[6], Info[7],
    Info[8], Info[9]);
  LogSec (L);

  for (N = 0; N < 6; N++) {
    if (MmioRead32 (R5Q_MDSS_BASE + 0x2000 + 0x200 * N + 0xF4) & BIT1) {   // CTL_INTF_ACTIVE: INTF1 (the DSI panel)
      Ctl = R5Q_MDSS_BASE + 0x2000 + 0x200 * N;
      break;
    }
  }
  if (Ctl == 0) {
    N   = 0;
    Ctl = R5Q_MDSS_BASE + 0x2000;
  }
  AsciiSPrint (L, sizeof (L), "[SCAN] ctl%u: layer0 %08x top %08x flush %08x intf_active %08x fetch_active %08x\n", N,
    MmioRead32 (Ctl + 0x00), MmioRead32 (Ctl + 0x14), MmioRead32 (Ctl + 0x18), MmioRead32 (Ctl + 0xF4), MmioRead32 (Ctl + 0xFC));
  LogSec (L);

  for (I = 0; I < 8; I++) {
    Base = R5Q_MDSS_BASE + Pipe[I];
    Addr = MmioRead32 (Base + 0x14);
    if ((Addr & 0x80000000U) == 0) {                                   // DRAM starts at 0x80000000
      continue;
    }
    AsciiSPrint (L, sizeof (L), "[SCAN] %a: addr %08x src_size %08x img_size %08x src_xy %08x out_size %08x out_xy %08x "
      "stride %08x format %08x\n", Name[I], Addr, MmioRead32 (Base + 0x00), MmioRead32 (Base + 0x04), MmioRead32 (Base + 0x08),
      MmioRead32 (Base + 0x0C), MmioRead32 (Base + 0x10), MmioRead32 (Base + 0x24), MmioRead32 (Base + 0x30));
    LogSec (L);
    if (mR5qScanOld == 0) {
      mR5qScanOld = Addr;
    }
    if (Addr != R5Q_SCAN_FB_PA) {
      MmioWrite32 (Base + 0x14, R5Q_SCAN_FB_PA);
      FlushBits |= Flush[I];
      mR5qScanPipes++;
    }
  }
  if (FlushBits == 0) {
    LogSec (mR5qScanOld ? "[SCAN] the panel's pipe already fetches 0x9C400000\n" : "[SCAN] no pipe fetching DRAM found\n");
    return;
  }
  MemoryFence ();
  MmioWrite32 (Ctl + 0x18, FlushBits);                                   // CTL_FLUSH: taken at the next frame
  F  = ReadCntFrq ();
  T0 = ReadCntPct ();
  while ((MmioRead32 (Ctl + 0x18) & FlushBits) != 0 && (ReadCntPct () - T0) < F / 5) {
  }
  mR5qScanTaken = (MmioRead32 (Ctl + 0x18) & FlushBits) == 0;
  AsciiSPrint (L, sizeof (L), "[SCAN] %u pipe(s) re-pointed %08x -> %08x (flush %08x %a after %lu ms)\n", mR5qScanPipes,
    mR5qScanOld, R5Q_SCAN_FB_PA, FlushBits, mR5qScanTaken ? "taken" : "STILL PENDING",
    ((ReadCntPct () - T0) * 1000ULL) / F);
  LogSec (L);
}
