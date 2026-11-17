/** R5Q native Test 2: command-mode display refresh, persistent sec_log telemetry,
    memory-only payload handoff, 10s visual countdown, and clean PSHOLD warm reset.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Configuration/BootDevices.h>

#define PAYLOAD_BASE 0xF0000000ULL
#define PAYLOAD_MAX  0x04000000U
#define PAYLOAD_MAGIC 0x50554E58U

#define MDSS_BASE    0x0AE00000UL
#define INTF1_BASE   (MDSS_BASE + 0x6B800)
#define CTL_START    0x1C
#define INFO_BASE    0x9E300000UL
#define INFO_MAGIC   0x32513552U   /* "R5Q2" written by the Test 2 shim */

#define SEC_LOG_BASE 0xC1600000ULL
#define PSHOLD_ADDR  0x0C264000UL

typedef struct { UINT32 Magic; UINT32 Size; } PAYLOAD_HEADER;
typedef struct { CHAR8 Name[16]; UINT32 Size; UINT32 Reserved; } PAYLOAD_ENTRY;

typedef struct {
  UINT32 Magic, FrameBuffer, Pipe, Stride, SrcSize, SrcFormat;
  UINT32 TearCheckEn, AutoRefresh, CtlIndex, CtlIntfActive, Stage;
  UINT32 EntryX0, EntryX1, KickTimeouts, CtlBase;
} SHIM_INFO;

STATIC volatile SHIM_INFO *mInfo = (volatile SHIM_INFO *)(UINTN)INFO_BASE;
STATIC EFI_EVENT mKickEvent;
STATIC UINT32    mKicks;
STATIC UINT32    mBusy;

VOID LogSec (IN CONST CHAR8 *Msg)
{
  volatile UINT32 *Hdr = (volatile UINT32 *)(UINTN)SEC_LOG_BASE;
  if (Hdr[1] != 0x4D474F4C) { // "LOGM"
    return;
  }
  UINT32 WIdx = Hdr[2];
  UINT32 Off = WIdx & 0x1FFFFF;
  if (Off < 0x10 || Off >= 0x1FFF00) {
    Off = 0x10;
  }
  volatile CHAR8 *Buf = (volatile CHAR8 *)(UINTN)SEC_LOG_BASE;
  while (*Msg != '\0') {
    Buf[Off++] = *Msg++;
    if (Off >= 0x1FFF00) {
      Off = 0x10;
    }
  }
  Hdr[2] = (WIdx & ~0x1FFFFF) | Off;
  MemoryFence ();
}

STATIC UINTN CtlBase (VOID)
{
  UINT32 Base;
  if (mInfo->Magic != INFO_MAGIC) {
    return MDSS_BASE + 0x2000;
  }
  Base = mInfo->CtlBase;
  if (Base < MDSS_BASE + 0x2000 || Base > MDSS_BASE + 0x2A00 || (Base & 0x1FF) != 0) {
    return MDSS_BASE + 0x2000;
  }
  return Base;
}

/* Push one command-mode frame if the previous transfer has completed and the
   panel is not already in hardware autorefresh. */
STATIC VOID Kick (VOID)
{
  UINTN Ctl = CtlBase ();
  if ((MmioRead32 (INTF1_BASE + 0x2B4) & BIT31) != 0) {
    return;
  }
  if ((MmioRead32 (Ctl + CTL_START) & BIT0) != 0) {
    mBusy++;
    return;
  }
  MmioWrite32 (Ctl + CTL_START, 1);
  mKicks++;
}

STATIC VOID EFIAPI KickTimer (IN EFI_EVENT Event, IN VOID *Context)
{
  Kick ();
}

STATIC VOID Band (UINTN Index, UINT32 Colour)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
  UINTN Width, Height;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    return;
  }
  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution / 8;
  Pixel.Blue     = (UINT8)Colour;
  Pixel.Green    = (UINT8)(Colour >> 8);
  Pixel.Red      = (UINT8)(Colour >> 16);
  Pixel.Reserved = 0;
  Gop->Blt (Gop, &Pixel, EfiBltVideoFill, 0, 0, 0, Height * Index, Width, Height, 0);
  Kick ();
}

STATIC EFI_STATUS StartTestLoader (VOID)
{
  CONST PAYLOAD_HEADER *Header = (CONST PAYLOAD_HEADER *)(UINTN)PAYLOAD_BASE;
  CONST PAYLOAD_ENTRY  *Entry = (CONST PAYLOAD_ENTRY *)(Header + 1);
  CONST PAYLOAD_ENTRY  *End;
  EFI_HANDLE Handle;
  EFI_STATUS Status;
  UINTN Padded;
  CHAR8 Msg[128];
  STATIC CONST CHAR8 LoaderName[16] = "loader.efi";
  STATIC CONST UINT8 EmptyEntry[sizeof (PAYLOAD_ENTRY)] = {0};

  LogSec ("[TEST2] Checking XNUP payload at 0xF0000000...\n");

  if (Header->Magic != PAYLOAD_MAGIC || Header->Size < 48 ||
      Header->Size > PAYLOAD_MAX - sizeof (*Header)) {
    LogSec ("[TEST2] FAIL: Payload magic or size invalid\n");
    return EFI_COMPROMISED_DATA;
  }
  if (CompareMem (Entry->Name, LoaderName, sizeof (LoaderName)) != 0 ||
      Entry->Reserved != 0 || Entry->Size < 256 || Entry->Size > SIZE_1MB) {
    LogSec ("[TEST2] FAIL: Entry header mismatch\n");
    return EFI_COMPROMISED_DATA;
  }
  Padded = ALIGN_VALUE ((UINTN)Entry->Size, 16);
  if (Header->Size != 2 * sizeof (*Entry) + Padded) {
    LogSec ("[TEST2] FAIL: Header size check failed\n");
    return EFI_COMPROMISED_DATA;
  }
  End = (CONST PAYLOAD_ENTRY *)((CONST UINT8 *)(Entry + 1) + Padded);
  if (CompareMem (End, EmptyEntry, sizeof (*End)) != 0) {
    LogSec ("[TEST2] FAIL: Terminator check failed\n");
    return EFI_COMPROMISED_DATA;
  }

  AsciiSPrint (Msg, sizeof (Msg), "[TEST2] PAYLOAD OK (%u bytes). Loading EFI Image...\n", Entry->Size);
  LogSec (Msg);
  Print (L"T2 PAYLOAD OK (%u bytes)\n", Entry->Size);

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, (VOID *)(Entry + 1), Entry->Size, &Handle);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Msg, sizeof (Msg), "[TEST2] LoadImage FAILED: %r\n", Status);
    LogSec (Msg);
    return Status;
  }

  LogSec ("[TEST2] EFI Image Loaded successfully. Starting Image...\n");
  Print (L"T2 EFI IMAGE LOADED\n");

  Status = gBS->StartImage (Handle, NULL, NULL);
  AsciiSPrint (Msg, sizeof (Msg), "[TEST2] StartImage finished: %r\n", Status);
  LogSec (Msg);

  gBS->UnloadImage (Handle);
  return Status;
}

STATIC VOID DumpDisplay (VOID)
{
  STATIC CONST UINT32 Pipes[8] = { 0x5000, 0x7000, 0x9000, 0xB000, 0x25000, 0x27000, 0x29000, 0x2B000 };
  STATIC CONST CHAR16 *Names[8] = { L"VIG0", L"VIG1", L"VIG2", L"VIG3", L"DMA0", L"DMA1", L"DMA2", L"DMA3" };
  UINTN Index, Base;
  CHAR8 Msg[256];

  if (mInfo->Magic == INFO_MAGIC) {
    AsciiSPrint (Msg, sizeof (Msg), "[TEST2] SHIM: fb=%08x pipe=%u stride=%u size=%08x stage=%u ctl=%x\n",
                 mInfo->FrameBuffer, mInfo->Pipe, mInfo->Stride, mInfo->SrcSize, mInfo->Stage, mInfo->CtlIndex);
    LogSec (Msg);
    Print (L"SHIM fb=%08x pipe=%u stride=%u size=%08x fmt=%08x stage=%u\n",
           mInfo->FrameBuffer, mInfo->Pipe, mInfo->Stride, mInfo->SrcSize, mInfo->SrcFormat, mInfo->Stage);
    Print (L"SHIM te=%08x autoref=%08x ctl=%x intfact=%08x kto=%u x0=%08x x1=%08x\n",
           mInfo->TearCheckEn, mInfo->AutoRefresh, mInfo->CtlIndex, mInfo->CtlIntfActive,
           mInfo->KickTimeouts, mInfo->EntryX0, mInfo->EntryX1);
  } else {
    LogSec ("[TEST2] SHIM INFO MISSING!\n");
    Print (L"SHIM INFO MISSING (%08x)\n", mInfo->Magic);
  }

  for (Index = 0; Index < 8; Index++) {
    Base = MDSS_BASE + Pipes[Index];
    Print (L"%s addr=%08x ystr=%08x size=%08x fmt=%08x op=%08x\n", Names[Index],
           MmioRead32 (Base + 0x14), MmioRead32 (Base + 0x24), MmioRead32 (Base + 0x00),
           MmioRead32 (Base + 0x30), MmioRead32 (Base + 0x38));
  }
  for (Index = 0; Index < 6; Index++) {
    Base = MDSS_BASE + 0x2000 + Index * 0x200;
    Print (L"CTL%u top=%08x intf=%08x pipes=%08x start=%08x\n", Index,
           MmioRead32 (Base + 0x14), MmioRead32 (Base + 0xF4), MmioRead32 (Base + 0xFC),
           MmioRead32 (Base + CTL_START));
  }
  Print (L"INTF1 sel=%08x te=%08x vsync=%08x h=%08x\n",
         MmioRead32 (INTF1_BASE + 0x280), MmioRead32 (INTF1_BASE + 0x284),
         MmioRead32 (INTF1_BASE + 0x288), MmioRead32 (INTF1_BASE + 0x28C));
  Print (L"INTF1 start=%08x rdptr=%08x outline=%08x line=%08x autoref=%08x\n",
         MmioRead32 (INTF1_BASE + 0x2A0), MmioRead32 (INTF1_BASE + 0x2A4),
         MmioRead32 (INTF1_BASE + 0x2AC), MmioRead32 (INTF1_BASE + 0x2B0),
         MmioRead32 (INTF1_BASE + 0x2B4));
  Print (L"KICKS ok=%u busy=%u\n", mKicks, mBusy);
}

VOID EFIAPI PlatformBootManagerBeforeConsole (VOID)
{
  LogSec ("[TEST2] PlatformBootManagerBeforeConsole: initializing display...\n");
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Enable INTF1 hardware autorefresh every TE pulse
  MmioWrite32 (INTF1_BASE + 0x2B4, 0x80000001);

  // Periodic software kick timer as backup (~30 fps)
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, KickTimer, NULL, &mKickEvent))) {
    gBS->SetTimer (mKickEvent, TimerPeriodic, 330000);
  }
  Kick ();
}

VOID EFIAPI PlatformBootManagerAfterConsole (VOID)
{
  EFI_STATUS Status;
  UINT32 Stage = (mInfo->Magic == INFO_MAGIC) ? mInfo->Stage : 0;
  CHAR8 Msg[128];

  LogSec ("[TEST2] PlatformBootManagerAfterConsole: starting UI...\n");
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  gST->ConOut->ClearScreen (gST->ConOut);

  // Paint full rainbow test bands across screen
  Band (0, 0xFF0000); // Red
  Band (1, 0xFF8000); // Orange
  Band (2, 0xFFFF00); // Yellow
  Band (3, 0x00FF00); // Green
  if (Stage >= 1) Band (4, 0x00FF00); // Green: DisplaySetup OK
  if (Stage >= 2) Band (5, 0x0000FF); // Blue: FD OK
  if (Stage == 3) Band (6, 0xFFFFFF); // White: Payload OK
  if (Stage == 4) Band (6, 0xFF0000); // Red: Payload rejected
  Band (7, 0x00FFFF); // Cyan: BDS reached

  Print (L"========================================\n");
  Print (L"   SAMSUNG GALAXY S10 LITE (SM-G770F)\n");
  Print (L"    NATIVE UEFI BOOT TEST 2 SUCCESS!\n");
  Print (L"========================================\n");
  DumpDisplay ();

  Status = StartTestLoader ();
  AsciiSPrint (Msg, sizeof (Msg), "[TEST2] Loader execution finished: %r\n", Status);
  LogSec (Msg);
  Print (L"T2 LOADER RESULT: %r\n", Status);
  Band (7, EFI_ERROR (Status) ? 0xFF00FF : 0x00FF00); // Green on success, Magenta on error

  LogSec ("[TEST2] Waiting 10 seconds before automatic PSHOLD warm reset...\n");
  Print (L"\nReturning to Android in 10 seconds (via hardware PSHOLD warm reset)...\n");

  for (INTN i = 10; i > 0; i--) {
    Print (L"Reset in %d seconds...\r", i);
    Kick ();
    gBS->Stall (1000000);
  }

  LogSec ("[TEST2] Triggering Qualcomm PSHOLD warm reset (0x0C264000 = 0)...\n");
  Print (L"\nResetting now!\n");
  Kick ();

  // Warm reset via Qualcomm PSHOLD (preserves RAM / sec_log, drops PS_HOLD)
  MmioWrite32 (PSHOLD_ADDR, 0);

  // Fallback to UEFI ResetSystem if PSHOLD is delayed
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

VOID EFIAPI PlatformBootManagerOnDemandConInConnect (VOID) {}
VOID EFIAPI PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption) {}
VOID EFIAPI PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext) {}
VOID EFIAPI PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain) {}
VOID EFIAPI PlatformBootManagerUnableToBoot (VOID) { CpuDeadLoop (); }
VOID EFIAPI PlatformBootManagerBdsEntry (VOID) {}
