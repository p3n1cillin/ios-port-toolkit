//
// r5qfull (Test 55+): put the panel back in step with the display pipeline, then prove it with a test pattern.
//
// What the measurements say (Tests 52-54): the pipe fetches exactly 0x9C400000 (1080x2400, stride 4320, no crop),
// the DSI controller inserts write_memory_start (DCS 0x2C) at each frame and the stream is 1080x2400, yet the panel
// shows our frame displaced with wraparound, by an amount that changes from boot to boot (Test 35: 3 characters;
// Tests 51/54: about half the screen). Android, which drives the same panel, renders perfectly. The frames and the
// panel's idea of where a frame starts are out of phase. Our own code made that possible: BootShim and the boot
// manager's KickDisplay fired CTL_START while the interface's autorefresh was sending frames by itself, which
// Qualcomm's driver never does (dpu_encoder_phys_cmd: autorefresh off, wait for the transfer, then kick).
//
// Resync: autorefresh off, let the transfer in flight drain, one clean frame (it begins with 0x2C), wait for it,
// autorefresh back on. From then on nobody kicks (KickDisplay only fences): autorefresh sends every frame.
//
// Included into FirstBootManager.c (uses LogSec, ReadCntPct, ReadCntFrq).
//

#define R5Q_PS_MDSS        0x0AE00000ULL
#define R5Q_PS_INTF1       (R5Q_PS_MDSS + 0x6B800)
#define R5Q_PS_DSI0        (R5Q_PS_MDSS + 0x94000)
#define R5Q_DSI_RESET      1                           // Test 56
#define R5Q_PS_PHASES      1                           // Test 57

STATIC UINT64  mR5qPsCtl;
#define R5Q_PS_FB          0x9C400000ULL
#define R5Q_PS_W           1080U
#define R5Q_PS_H           2400U

STATIC VOID
R5qPsWait (
  UINT64  Us
  )
{
  UINT64  F = ReadCntFrq (), T0 = ReadCntPct ();

  while ((ReadCntPct () - T0) < (F * Us) / 1000000ULL) {
  }
}

//
// Test 58: the panel's TE (tear effect) signal on GPIO 8 (live DT: qcom,platform-te-gpio = gpio8; pinctrl sde_te_active:
// function mdp_vsync, 2 mA, pull-down). The tear check uses it to know where the panel's scan is and so when to
// send a frame (the stock driver's AMB667UM01 config: DSITEUsingDedicatedTEPin, start position 2400). If the full
// firmware's TLMM driver left the pin in another function, the tear check runs on a guess and frames start at an
// arbitrary phase of the panel's scan. SM8150: gpio8 is in the NORTH tile (TLMM 0x03000000 + 0x900000), function 1 =
// mdp_vsync; CFG = pull [1:0], function [5:2], drive [8:6]; IN_OUT bit 0 = the pin's level.
//
#define R5Q_TE_CFG   (0x03000000ULL + 0x900000ULL + 8 * 0x1000ULL)

STATIC UINT32
R5qTeEdges (
  VOID
  )
{
  UINT32  I, Last = MmioRead32 (R5Q_TE_CFG + 4) & 1, Now, Edges = 0;

  for (I = 0; I < 2000; I++) {   // ~50 ms: three panel frames at 60 Hz
    Now = MmioRead32 (R5Q_TE_CFG + 4) & 1;
    Edges += (Now != Last);
    Last = Now;
    R5qPsWait (25);
  }
  return Edges;
}

STATIC VOID
R5qTePin (
  VOID
  )
{
  UINT32  Cfg = MmioRead32 (R5Q_TE_CFG), Edges = R5qTeEdges ();
  CHAR8   L[160];

  AsciiSPrint (L, sizeof (L), "[TE] gpio8 cfg %08x (function %u, pull %u, drive %u) edges in 50 ms: %u; intf1 vsync_sel %x\n",
    Cfg, (Cfg >> 2) & 0xF, Cfg & 3, (Cfg >> 6) & 7, Edges, MmioRead32 (R5Q_PS_INTF1 + 0x280));
  LogSec (L);
  if (((Cfg >> 2) & 0xF) != 1) {
    MmioWrite32 (R5Q_TE_CFG, (Cfg & ~0x1FFU) | (1U << 2) | 1U);   // mdp_vsync, pull-down, 2 mA
    MemoryFence ();
    R5qPsWait (20000);
    Cfg   = MmioRead32 (R5Q_TE_CFG);
    Edges = R5qTeEdges ();
    AsciiSPrint (L, sizeof (L), "[TE] gpio8 set to mdp_vsync: cfg %08x, edges in 50 ms: %u\n", Cfg, Edges);
    LogSec (L);
  }
}

//
// Test 60: the panel itself starts each frame's write partway in (Test 59: the ruler from row ~1200 at the panel's
// top, black below, no magenta: the fetch is right; Test 56: ~580 rows; a different amount every boot). The panel's
// write window (DCS 2A column / 2B page address, or partial mode) is panel state that controller resets do not
// touch; a garbled transfer early in our boot can leave it anywhere. Send what the stock bootloader sends at init
// (its log: 2A 00 00 04 37, 2B 00 00 09 5F) plus 13 (normal display mode: no partial area), through the DSI
// controller's command DMA, with Linux msm's in-memory packet format (dsi_cmd_dma_add): bytes header[1], header[2],
// header[0], flags (bit 7 last, bit 6 long), then the payload padded with 0xFF to 4 bytes.
// DSI 6G registers as in this phone's dump: CMD_DMA_CTRL 0x3C (0x14000000: from memory, low power, kept),
// DMA_BASE 0x48, DMA_LEN 0x4C, TRIG_DMA 0x90, INTR_CTRL 0x110 (bit 0: CMD_DMA_DONE, write 1 to clear).
//
#define R5Q_PANEL_WINDOW   1
#define R5Q_DSI_CMDBUF_ABL 0xFF30A000ULL       // the stock bootloader's command buffer (its DMA_BASE, still set)
#define R5Q_DSI_CMDBUF_ALT 0x9E200000ULL       // fallback: Display Reserved, below BootShim's record at 0x9E300000
STATIC UINT64  mR5qDsiBuf = R5Q_DSI_CMDBUF_ALT;

STATIC BOOLEAN
R5qDsiSend (
  IN CONST UINT8  *Pkt,
  IN UINT32       Len,
  IN CONST CHAR8  *Name
  )
{
  volatile UINT8  *Buf = (volatile UINT8 *)(UINTN)mR5qDsiBuf;
  UINT64          F    = ReadCntFrq (), T0;
  UINT32          I, Intr;
  BOOLEAN         Done;
  CHAR8           L[128];

  for (I = 0; I < Len; I++) {
    Buf[I] = Pkt[I];
  }
  WriteBackDataCacheRange ((VOID *)(UINTN)mR5qDsiBuf, 64);
  MemoryFence ();
  Intr = MmioRead32 (R5Q_PS_DSI0 + 0x110);
  MmioWrite32 (R5Q_PS_DSI0 + 0x110, (Intr & 0xAAAAAAAAU) | BIT1 | BIT0);   // keep the masks, enable MASK_CMD_DMA_DONE, clear an old DONE
  MmioWrite32 (R5Q_PS_DSI0 + 0x48, (UINT32)mR5qDsiBuf);
  MmioWrite32 (R5Q_PS_DSI0 + 0x4C, Len);
  MemoryFence ();
  MmioWrite32 (R5Q_PS_DSI0 + 0x90, 1);                 // TRIG_DMA
  MemoryFence ();
  T0 = ReadCntPct ();
  while (!(MmioRead32 (R5Q_PS_DSI0 + 0x110) & BIT0) && (ReadCntPct () - T0) < F / 50) {
  }
  Done = (MmioRead32 (R5Q_PS_DSI0 + 0x110) & BIT0) != 0;
  MmioWrite32 (R5Q_PS_DSI0 + 0x110, (MmioRead32 (R5Q_PS_DSI0 + 0x110) & 0xAAAAAAAAU) | BIT0);
  AsciiSPrint (L, sizeof (L), "[PANEL] %a: %a (intr %08x, ack_err %08x)\n", Name, Done ? "sent" : "NO DMA DONE in 20 ms",
    MmioRead32 (R5Q_PS_DSI0 + 0x110), MmioRead32 (R5Q_PS_DSI0 + 0x68));
  LogSec (L);
  return Done;
}

STATIC VOID
R5qPanelWindow (
  VOID
  )
{
  STATIC CONST UINT8  Normal[4]  = { 0x13, 0x00, 0x05, 0x80 };                                     // DCS 13, short
  STATIC CONST UINT8  Column[12] = { 0x05, 0x00, 0x39, 0xC0, 0x2A, 0x00, 0x00, 0x04, 0x37, 0xFF, 0xFF, 0xFF };
  STATIC CONST UINT8  Page[12]   = { 0x05, 0x00, 0x39, 0xC0, 0x2B, 0x00, 0x00, 0x09, 0x5F, 0xFF, 0xFF, 0xFF };
  CHAR8               L[128];
  EFI_PHYSICAL_ADDRESS  A = R5Q_DSI_CMDBUF_ABL;

  // the bootloader's buffer: the address its command DMA is known to reach (claimed while we use it)
  mR5qDsiBuf = EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, 1, &A)) ? R5Q_DSI_CMDBUF_ALT : A;
  AsciiSPrint (L, sizeof (L), "[PANEL] command buffer %lx\n", mR5qDsiBuf);
  LogSec (L);
  AsciiSPrint (L, sizeof (L), "[PANEL] dma_ctrl %08x dma_base %08x dma_len %x trig_ctrl %08x intr %08x\n",
    MmioRead32 (R5Q_PS_DSI0 + 0x3C), MmioRead32 (R5Q_PS_DSI0 + 0x48), MmioRead32 (R5Q_PS_DSI0 + 0x4C),
    MmioRead32 (R5Q_PS_DSI0 + 0x84), MmioRead32 (R5Q_PS_DSI0 + 0x110));
  LogSec (L);
  if (R5qDsiSend (Normal, sizeof (Normal), "13 normal display mode")) {
    R5qDsiSend (Column, sizeof (Column), "2A columns 0-1079");
    R5qDsiSend (Page, sizeof (Page), "2B rows 0-2399");
  }
  R5qPsWait (2000);
  // Test 63: give the bootloader's command page back: it lies inside the iOS system image's resident range
  // (0xFC000000-0x1908C0000; Test 62: "no memory ... at the resident address")
  if (mR5qDsiBuf == R5Q_DSI_CMDBUF_ABL) {
    gBS->FreePages (mR5qDsiBuf, 1);
    LogSec ("[PANEL] command page released\n");
  }
}

STATIC VOID
R5qPanelResync (
  VOID
  )
{
  UINT64  Ctl = 0, F = ReadCntFrq (), T0;
  UINT32  N, Orig, Line0, Line1, Waited = 0, Done;
  CHAR8   L[180];

  for (N = 0; N < 6; N++) {
    if (MmioRead32 (R5Q_PS_MDSS + 0x2000 + 0x200 * N + 0xF4) & BIT1) {   // the CTL path driving INTF1 (the panel)
      Ctl = R5Q_PS_MDSS + 0x2000 + 0x200 * N;
      break;
    }
  }
  if (Ctl == 0) {
    LogSec ("[SYNC] no CTL path drives INTF1; panel left alone\n");
    return;
  }
  Orig = MmioRead32 (R5Q_PS_INTF1 + 0x2B4);
  AsciiSPrint (L, sizeof (L), "[SYNC] ctl%u start %x; intf1 autorefresh %08x tear_en %x line %x int_count %x\n", N,
    MmioRead32 (Ctl + 0x1C), Orig, MmioRead32 (R5Q_PS_INTF1 + 0x284), MmioRead32 (R5Q_PS_INTF1 + 0x2B0),
    MmioRead32 (R5Q_PS_INTF1 + 0x298));
  LogSec (L);

  // 1. autorefresh off, then wait until no transfer is in flight (the write pointer stops moving) and start is idle
  MmioWrite32 (R5Q_PS_INTF1 + 0x2B4, 0);
  MemoryFence ();
  T0 = ReadCntPct ();
  do {
    Line0 = MmioRead32 (R5Q_PS_INTF1 + 0x2B0);
    R5qPsWait (2000);
    Line1 = MmioRead32 (R5Q_PS_INTF1 + 0x2B0);
    Waited++;
  } while ((Line0 != Line1 || (MmioRead32 (Ctl + 0x1C) & BIT0)) && (ReadCntPct () - T0) < F / 5);
  R5qPsWait (20000);   // and one more frame time for the panel side
  AsciiSPrint (L, sizeof (L), "[SYNC] autorefresh off; idle after %u checks (line %x, start %x)\n", Waited, Line1,
    MmioRead32 (Ctl + 0x1C));
  LogSec (L);

#ifdef R5Q_DSI_RESET
  //
  // 1b. Test 56: the DSI controller's command-stream counter is out of phase with the frames (Test 55's photo: the
  // panel's write_memory_start lands ~620 rows + 90 px into every frame; the rows before it are lost, the panel's
  // last rows keep the stock splash), and a clean frame does not reset it. Linux's msm DSI driver recovers its
  // stream errors with dsi_sw_reset_restore(): controller disabled, RESET pulsed for 20 ms, CTRL restored, with the
  // clocks running and the link up. Done here while nothing is in flight.
  //
  {
    UINT32  Ctrl = MmioRead32 (R5Q_PS_DSI0 + 0x004);

    AsciiSPrint (L, sizeof (L), "[SYNC] dsi0 ctrl %08x status %08x fifo %08x err %08x before reset\n", Ctrl,
      MmioRead32 (R5Q_PS_DSI0 + 0x008), MmioRead32 (R5Q_PS_DSI0 + 0x00C), MmioRead32 (R5Q_PS_DSI0 + 0x10C));
    LogSec (L);
    MmioWrite32 (R5Q_PS_DSI0 + 0x004, Ctrl & ~BIT0);   // DSI_CTRL_ENABLE off
    MemoryFence ();
    MmioWrite32 (R5Q_PS_DSI0 + 0x118, 1);              // DSI_RESET (6G: dsi.xml 0x114 + DSI_6G_REG_SHIFT 4; Tests 56-60 wrote 0x114)
    MemoryFence ();
    R5qPsWait (20000);
    MmioWrite32 (R5Q_PS_DSI0 + 0x118, 0);
    MemoryFence ();
    MmioWrite32 (R5Q_PS_DSI0 + 0x004, Ctrl);
    MemoryFence ();
    R5qPsWait (2000);
    AsciiSPrint (L, sizeof (L), "[SYNC] dsi0 reset: ctrl %08x status %08x fifo %08x err %08x\n",
      MmioRead32 (R5Q_PS_DSI0 + 0x004), MmioRead32 (R5Q_PS_DSI0 + 0x008), MmioRead32 (R5Q_PS_DSI0 + 0x00C),
      MmioRead32 (R5Q_PS_DSI0 + 0x10C));
    LogSec (L);
  }
#endif

#ifdef R5Q_PANEL_WINDOW
  // 1c. Test 60: the panel's own write window, as the stock bootloader sets it at init
  R5qPanelWindow ();
#endif

  // 2. one clean frame: it begins with write_memory_start
  MmioWrite32 (Ctl + 0x1C, 1);
  MemoryFence ();
  T0 = ReadCntPct ();
  while ((MmioRead32 (Ctl + 0x1C) & BIT0) && (ReadCntPct () - T0) < F / 5) {
  }
  Done = (MmioRead32 (Ctl + 0x1C) & BIT0) == 0;
  R5qPsWait (20000);

#ifdef R5Q_PS_PHASES
  // Test 57: autorefresh stays off here; R5qPatternPhases() decides
  mR5qPsCtl = Ctl;
  AsciiSPrint (L, sizeof (L), "[SYNC] clean frame %a; autorefresh left off (was %08x)\n", Done ? "sent" : "STILL BUSY", Orig);
  LogSec (L);
#else
  // 3. autorefresh back on (the bootloader/BootShim setting: every TE pulse)
  MmioWrite32 (R5Q_PS_INTF1 + 0x2B4, (Orig & BIT31) ? Orig : 0x80000001U);
  MemoryFence ();
  AsciiSPrint (L, sizeof (L), "[SYNC] clean frame %a; autorefresh back %08x\n", Done ? "sent" : "STILL BUSY",
    MmioRead32 (R5Q_PS_INTF1 + 0x2B4));
  LogSec (L);
#endif
}

STATIC VOID
R5qPsRect (
  UINT32  X,
  UINT32  Y,
  UINT32  W,
  UINT32  H,
  UINT32  C
  )
{
  volatile UINT32  *Fb = (volatile UINT32 *)(UINTN)R5Q_PS_FB;
  UINT32           R, Cl;

  for (R = Y; R < Y + H && R < R5Q_PS_H; R++) {
    for (Cl = X; Cl < X + W && Cl < R5Q_PS_W; Cl++) {
      Fb[(UINTN)R * R5Q_PS_W + Cl] = C;
    }
  }
}

//
// A frame whose every edge and corner is unmistakable in a photo: white border on the panel's edges, red (top
// left), green (top right), blue (bottom left) and yellow (bottom right) corner squares, a white centre cross.
//
STATIC VOID
R5qCalibrationPattern (
  UINT32  HoldS
  )
{
  CHAR8  L[64];

  R5qPsRect (0, 0, R5Q_PS_W, R5Q_PS_H, 0xFF000000U);
  R5qPsRect (0, 0, R5Q_PS_W, 8, 0xFFFFFFFFU);
  R5qPsRect (0, R5Q_PS_H - 8, R5Q_PS_W, 8, 0xFFFFFFFFU);
  R5qPsRect (0, 0, 8, R5Q_PS_H, 0xFFFFFFFFU);
  R5qPsRect (R5Q_PS_W - 8, 0, 8, R5Q_PS_H, 0xFFFFFFFFU);
  R5qPsRect (8, 8, 160, 160, 0xFFFF2020U);
  R5qPsRect (R5Q_PS_W - 168, 8, 160, 160, 0xFF20E020U);
  R5qPsRect (8, R5Q_PS_H - 168, 160, 160, 0xFF2040FFU);
  R5qPsRect (R5Q_PS_W - 168, R5Q_PS_H - 168, 160, 160, 0xFFFFE020U);
  R5qPsRect (R5Q_PS_W / 2 - 3, R5Q_PS_H / 2 - 150, 6, 300, 0xFFFFFFFFU);
  R5qPsRect (R5Q_PS_W / 2 - 150, R5Q_PS_H / 2 - 3, 300, 6, 0xFFFFFFFFU);
  WriteBackDataCacheRange ((VOID *)(UINTN)R5Q_PS_FB, R5Q_PS_W * R5Q_PS_H * 4);
  MemoryFence ();
  AsciiSPrint (L, sizeof (L), "[SYNC] calibration pattern on screen for %u s\n", HoldS);
  LogSec (L);
  R5qPsWait ((UINT64)HoldS * 1000000ULL);
}

//
// Test 59: where do the panel's rows come from? Test 58: the TE pin is right (mdp_vsync, 3 pulses per 50 ms). Two
// explanations remain for "the frame's first ~580 rows are missing and the panel's bottom keeps the splash":
// (a) the panel drops the first rows of each transfer; (b) the pipe's fetch is translated (SMMU) past our buffer,
// so the panel's bottom shows memory after the framebuffer, where the bootloader's splash may be. A ruler in the
// framebuffer (a white line every 100 rows, k*36 px long for row 100*(k-1)) and magenta in the 10 MB after it
// (inside Display Reserved, below BootShim's record at 0x9E300000) tell them apart in one photo.
//
STATIC VOID
R5qRulerPattern (
  UINT32  HoldS
  )
{
  volatile UINT32  *After = (volatile UINT32 *)(UINTN)(R5Q_PS_FB + 0x9E4000ULL);
  UINTN            I;
  UINT32           K;
  CHAR8            L[96];

  R5qCalibrationPattern (0);
  for (K = 0; K < 24; K++) {
    R5qPsRect (180, K * 100 + (K == 0 ? 12 : 0), 36 * (K + 1), 8, 0xFFFFFFFFU);
  }
  for (I = 0; I < (0xA00000 / 4); I++) {
    After[I] = 0xFFFF00FFU;
  }
  WriteBackDataCacheRange ((VOID *)(UINTN)R5Q_PS_FB, 0x9E4000 + 0xA00000);
  MemoryFence ();
  MmioWrite32 (R5Q_PS_INTF1 + 0x2B4, 0x80000002U);   // the stock setting: every 2nd TE pulse
  MemoryFence ();
  AsciiSPrint (L, sizeof (L), "[SYNC] ruler + magenta after the framebuffer, for %u s\n", HoldS);
  LogSec (L);
  R5qPsWait ((UINT64)HoldS * 1000000ULL);
}

#ifdef R5Q_PS_PHASES_RUN
//
// Test 57: the same pattern in three refresh modes, 8 s each, numbered by 1-3 small white squares left of the
// centre cross: (1) autorefresh off, one clean frame sent and waited for; (2) autorefresh every 2nd TE pulse
// (Qualcomm DisplayDxe's AutoRefreshFrameNumDiv 2); (3) every TE pulse (what BootShim set). Mode 2 stays on after.
//
STATIC VOID
R5qPatternPhases (
  VOID
  )
{
  STATIC CONST UINT32  Auto[4] = { 0, 0, 0x80000002U, 0x80000001U };
  UINT32               Phase, K;
  UINT64               F = ReadCntFrq (), T0;
  CHAR8                L[128];

  for (Phase = 1; Phase <= 3; Phase++) {
    R5qCalibrationPattern (0);
    for (K = 0; K < Phase; K++) {
      R5qPsRect (R5Q_PS_W / 2 - 260 + K * 60, R5Q_PS_H / 2 - 20, 40, 40, 0xFFFFFFFFU);
    }
    WriteBackDataCacheRange ((VOID *)(UINTN)R5Q_PS_FB, R5Q_PS_W * R5Q_PS_H * 4);
    MemoryFence ();
    if (Phase == 1) {
      if (mR5qPsCtl != 0) {
        MmioWrite32 (mR5qPsCtl + 0x1C, 1);     // one frame, autorefresh off
        MemoryFence ();
        T0 = ReadCntPct ();
        while ((MmioRead32 (mR5qPsCtl + 0x1C) & BIT0) && (ReadCntPct () - T0) < F / 5) {
        }
      }
    } else {
      MmioWrite32 (R5Q_PS_INTF1 + 0x2B4, Auto[Phase]);
      MemoryFence ();
    }
    AsciiSPrint (L, sizeof (L), "[SYNC] phase %u: autorefresh %08x, start %x, line %x\n", Phase,
      MmioRead32 (R5Q_PS_INTF1 + 0x2B4), mR5qPsCtl ? MmioRead32 (mR5qPsCtl + 0x1C) : 0xFFFFFFFFU,
      MmioRead32 (R5Q_PS_INTF1 + 0x2B0));
    LogSec (L);
    R5qPsWait (8000000ULL);
  }
  MmioWrite32 (R5Q_PS_INTF1 + 0x2B4, Auto[2]);
  MemoryFence ();
  LogSec ("[SYNC] phases done; autorefresh every 2nd TE pulse from here on\n");
}
#endif
