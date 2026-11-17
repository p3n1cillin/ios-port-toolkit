// Test 3 Boot Manager: direct framebuffer visual checkpoints, 3x bitmap console,
// fixed sec_log ring buffer, safe CTL_START kick, xnuloader handoff, and PSHOLD reset.
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>

#define FB_BASE           0x9C400000ULL
#define FB_WIDTH          1080U
#define FB_HEIGHT         2400U
#define FB_STRIDE_PIXELS  1080U
#define FB_SIZE_BYTES     (FB_WIDTH * FB_HEIGHT * 4U)

#define SEC_LOG_BASE      0xC1600000ULL
#define SEC_LOG_CAP       2097136U  // 0x200000 - 16
#define PAYLOAD_BASE      0xF0000000ULL
#define PAYLOAD_MAX       0x04000000U
#define XNUP_MAGIC        0x50554E58U // "XNUP"

#define CTL0_BASE         0x0AE02000ULL
#define CTL1_BASE         0x0AE03000ULL
#define CTL_START_OFF     0x01CU
#define INTF1_BASE        0x0AE6B000ULL
#define PSHOLD_ADDR       0x0C264000ULL

#define FONT_SCALE        3U
#define CHAR_ADV_X        (8U * FONT_SCALE + 3U)
#define CHAR_ADV_Y        (8U * FONT_SCALE + 10U)
#define DASH_TOP_Y        140U

STATIC BOOLEAN    mBootPhaseReady = FALSE;
STATIC UINT32     mTextRow   = 0;
STATIC UINT32     mTimerTicks = 0;

// 8x8 ASCII font for 0x20..0x5F (lowercase mapped to uppercase for compactness)
STATIC CONST UINT8  mFont8x8[64][8] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' 0x20
  {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
  {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
  {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
  {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // '$'
  {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
  {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
  {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '''
  {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
  {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
  {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
  {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
  {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
  {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // '0' 0x30
  {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
  {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // '2'
  {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
  {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00}, // '4'
  {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
  {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
  {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
  {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
  {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
  {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
  {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '='
  {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // '>'
  {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?'
  {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // '@' 0x40
  {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00}, // 'A'
  {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 'B'
  {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 'C'
  {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 'D'
  {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 'E'
  {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 'F'
  {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 'G'
  {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'H'
  {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
  {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J'
  {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 'K'
  {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 'L'
  {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 'M'
  {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'N'
  {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'O'
  {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 'P' 0x50
  {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 'Q'
  {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 'R'
  {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 'S'
  {0xFF,0xDB,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'T'
  {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'U'
  {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'V'
  {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, // 'W'
  {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, // 'X'
  {0xC3,0x66,0x3C,0x18,0x18,0x18,0x3C,0x00}, // 'Y'
  {0xFE,0x86,0x0C,0x18,0x30,0x62,0xFE,0x00}, // 'Z'
  {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
  {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
  {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
  {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
};

STATIC
VOID
LogSec (
  IN CONST CHAR8  *Msg
  )
{
  volatile UINT32  *Hdr;
  volatile UINT8   *Buf;
  UINT32           WIdx;
  UINT32           Pos;
  UINTN            Idx;

  Hdr = (volatile UINT32 *)(UINTN)SEC_LOG_BASE;
  if (Hdr[1] != 0x4D474F4CU) {
    return;
  }
  Buf  = (volatile UINT8 *)(UINTN)(SEC_LOG_BASE + 0x10ULL);
  WIdx = Hdr[2];
  for (Idx = 0; Msg[Idx] != '\0'; Idx++) {
    Pos      = WIdx % SEC_LOG_CAP;
    Buf[Pos] = (UINT8)Msg[Idx];
    WIdx++;
  }
  Hdr[2] = WIdx;
  MemoryFence ();
}

STATIC
VOID
KickDisplay (
  VOID
  )
{
  MemoryFence ();
  MmioWrite32 (INTF1_BASE + 0x2B4U, 0x80000001U);
  if ((MmioRead32 (CTL0_BASE + CTL_START_OFF) & BIT0) == 0U) {
    MmioWrite32 (CTL0_BASE + CTL_START_OFF, 1U);
  }
  MemoryFence ();
}

STATIC
VOID
FbFillRect (
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  W,
  IN UINT32  H,
  IN UINT32  Color
  )
{
  volatile UINT32  *Fb;
  UINT32           Row;
  UINT32           Col;

  if ((X >= FB_WIDTH) || (Y >= FB_HEIGHT)) {
    return;
  }
  if (X + W > FB_WIDTH) {
    W = FB_WIDTH - X;
  }
  if (Y + H > FB_HEIGHT) {
    H = FB_HEIGHT - Y;
  }
  Fb = (volatile UINT32 *)(UINTN)FB_BASE;
  for (Row = 0; Row < H; Row++) {
    UINTN  Base = (UINTN)(Y + Row) * FB_STRIDE_PIXELS + X;
    for (Col = 0; Col < W; Col++) {
      Fb[Base + Col] = Color;
    }
  }
}

STATIC
VOID
FbDrawChar (
  IN UINT32  X,
  IN UINT32  Y,
  IN CHAR8   Ch,
  IN UINT32  Fg,
  IN UINT32  Bg
  )
{
  volatile UINT32  *Fb;
  UINT8            Idx;
  UINT32           Row;
  UINT32           Col;
  UINT32           Sy;
  UINT32           Sx;

  if ((Ch >= 'a') && (Ch <= 'z')) {
    Ch = (CHAR8)(Ch - 'a' + 'A');
  }
  if ((Ch < 0x20) || (Ch > 0x5F)) {
    Ch = '?';
  }
  Idx = (UINT8)(Ch - 0x20);
  Fb  = (volatile UINT32 *)(UINTN)FB_BASE;

  for (Row = 0; Row < 8U; Row++) {
    UINT8  Bits = mFont8x8[Idx][Row];
    for (Sy = 0; Sy < FONT_SCALE; Sy++) {
      UINT32  Py = Y + Row * FONT_SCALE + Sy;
      if (Py >= FB_HEIGHT) {
        continue;
      }
      for (Col = 0; Col < 8U; Col++) {
        UINT32  Color = ((Bits & (0x80U >> Col)) != 0U) ? Fg : Bg;
        for (Sx = 0; Sx < FONT_SCALE; Sx++) {
          UINT32  Px = X + Col * FONT_SCALE + Sx;
          if (Px < FB_WIDTH) {
            Fb[(UINTN)Py * FB_STRIDE_PIXELS + Px] = Color;
          }
        }
      }
    }
  }
}

STATIC
VOID
FbDrawTextAt (
  IN UINT32       X,
  IN UINT32       Y,
  IN CONST CHAR8  *Text,
  IN UINT32       Fg,
  IN UINT32       Bg
  )
{
  UINTN  Idx;

  for (Idx = 0; Text[Idx] != '\0'; Idx++) {
    if (X + CHAR_ADV_X >= FB_WIDTH) {
      break;
    }
    FbDrawChar (X, Y, Text[Idx], Fg, Bg);
    X += CHAR_ADV_X;
  }
}

STATIC
VOID
FbPrintLine (
  IN CONST CHAR8  *Text,
  IN UINT32       Fg
  )
{
  UINT32  Y;

  Y = DASH_TOP_Y + mTextRow * CHAR_ADV_Y;
  if (Y + CHAR_ADV_Y >= FB_HEIGHT - 120U) {
    FbFillRect (0, DASH_TOP_Y, FB_WIDTH, FB_HEIGHT - DASH_TOP_Y - 120U, 0xFF0A1428U);
    mTextRow = 0;
    Y        = DASH_TOP_Y;
  }
  FbFillRect (0, Y, FB_WIDTH, CHAR_ADV_Y, 0xFF0A1428U);
  FbDrawTextAt (24U, Y + 4U, Text, Fg, 0xFF0A1428U);
  mTextRow++;
  KickDisplay ();
}

STATIC
VOID
SetCheckpoint (
  IN UINT32  Index,
  IN UINT32  Color
  )
{
  if (Index < 6U) {
    FbFillRect (Index * 180U, 10U, 176U, 96U, Color);
    KickDisplay ();
  }
}

STATIC
VOID
EFIAPI
KickTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mTimerTicks++;
}

STATIC
UINT64
ReadCntPct (
  VOID
  )
{
  UINT64  Val;
  __asm__ __volatile__ ("mrs %0, cntpct_el0" : "=r" (Val));
  return Val;
}

STATIC
UINT64
ReadCntFrq (
  VOID
  )
{
  UINT64  Val;
  __asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (Val));
  return (Val != 0ULL) ? Val : 19200000ULL;
}

STATIC
EFI_STATUS
RunTestLoader (
  OUT CHAR8   *StatusLine,
  IN  UINTN   LineSize
  )
{
  volatile UINT32  *Hdr;
  UINT32           BodySize;
  UINT8            *Ptr;
  UINT8            *End;
  UINT32           EntrySize;
  UINT32           Flags;
  VOID             *EfiBuf;
  UINTN            EfiSize;
  EFI_HANDLE       ImgHandle;
  EFI_STATUS       Status;
  struct {
    MEMMAP_DEVICE_PATH        Mem;
    EFI_DEVICE_PATH_PROTOCOL  End;
  } DevPath;

  Hdr = (volatile UINT32 *)(UINTN)PAYLOAD_BASE;
  if (Hdr[0] != XNUP_MAGIC) {
    AsciiSPrint (StatusLine, LineSize, "[5] XNUP BAD MAGIC: 0x%08X", Hdr[0]);
    return EFI_NOT_FOUND;
  }
  BodySize = Hdr[1];
  if ((BodySize == 0U) || (BodySize > (PAYLOAD_MAX - 8U))) {
    AsciiSPrint (StatusLine, LineSize, "[5] XNUP BAD SIZE: 0x%08X", BodySize);
    return EFI_BAD_BUFFER_SIZE;
  }

  Ptr     = (UINT8 *)(UINTN)(PAYLOAD_BASE + 8ULL);
  End     = Ptr + BodySize;
  EfiBuf  = NULL;
  EfiSize = 0;

  while ((UINTN)(End - Ptr) >= 24U) {
    if (Ptr[0] == 0U) {
      break;
    }
    EntrySize = *(UINT32 *)(Ptr + 16);
    Flags     = *(UINT32 *)(Ptr + 20);
    (VOID)Flags;
    if ((EntrySize == 0U) || ((UINTN)(End - Ptr) < (24U + EntrySize))) {
      break;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)Ptr, "loader.efi") == 0) {
      EfiBuf  = Ptr + 24;
      EfiSize = EntrySize;
      break;
    }
    Ptr += 24U + ((EntrySize + 15U) & ~15U);
  }

  if (EfiBuf == NULL) {
    AsciiSPrint (StatusLine, LineSize, "[5] loader.efi NOT FOUND IN XNUP");
    return EFI_NOT_FOUND;
  }

  DevPath.Mem.Header.Type    = HARDWARE_DEVICE_PATH;
  DevPath.Mem.Header.SubType = HW_MEMMAP_DP;
  SetDevicePathNodeLength (&DevPath.Mem.Header, sizeof (MEMMAP_DEVICE_PATH));
  DevPath.Mem.MemoryType      = EfiLoaderCode;
  DevPath.Mem.StartingAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)EfiBuf;
  DevPath.Mem.EndingAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN)EfiBuf + EfiSize;
  SetDevicePathEndNode (&DevPath.End);

  ImgHandle = NULL;
  Status    = gBS->LoadImage (
                     FALSE,
                     gImageHandle,
                     (EFI_DEVICE_PATH_PROTOCOL *)&DevPath,
                     EfiBuf,
                     EfiSize,
                     &ImgHandle
                     );
  if (EFI_ERROR (Status)) {
    AsciiSPrint (StatusLine, LineSize, "[5] LoadImage ERR: 0x%lx", (UINT64)Status);
    return Status;
  }

  Status = gBS->StartImage (ImgHandle, NULL, NULL);
  AsciiSPrint (StatusLine, LineSize, "[5] loader.efi OK RET=0x%lx", (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
ClearSamsungParamFlag (
  OUT CHAR8  *OutLine,
  IN  UINTN  LineSize
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Idx;
  EFI_PARTITION_INFO_PROTOCOL  *PartInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  UINT8                        *Buf;
  UINT32                       OldVal;
  UINT32                       ShdnMagic;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    for (Idx = 0; Idx < Count; Idx++) {
      gBS->ConnectController (Handles[Idx], NULL, NULL, TRUE);
    }
    FreePool (Handles);
  }

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    AsciiSPrint (OutLine, LineSize, "[6] UFS NO PARTITIONS: 0x%lx", (UINT64)Status);
    return EFI_NOT_FOUND;
  }

  for (Idx = 0; Idx < Count; Idx++) {
    PartInfo = NULL;
    Status   = gBS->HandleProtocol (Handles[Idx], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo == NULL) || (PartInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }
    if (StrCmp (PartInfo->Info.Gpt.PartitionName, L"param") != 0) {
      continue;
    }
    BlockIo = NULL;
    Status  = gBS->HandleProtocol (Handles[Idx], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL) || (BlockIo->Media->BlockSize != 4096U)) {
      AsciiSPrint (OutLine, LineSize, "[6] PARAM BLKIO ERR: 0x%lx", (UINT64)Status);
      FreePool (Handles);
      return EFI_DEVICE_ERROR;
    }

    Buf = AllocateZeroPool (4096U);
    if (Buf == NULL) {
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }
    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0ULL, 4096U, Buf);
    if (EFI_ERROR (Status)) {
      AsciiSPrint (OutLine, LineSize, "[6] PARAM READ ERR: 0x%lx", (UINT64)Status);
      FreePool (Buf);
      FreePool (Handles);
      return Status;
    }

    OldVal    = *(UINT32 *)(Buf + 0x00U);
    ShdnMagic = *(UINT32 *)(Buf + 0x68U);
    if (ShdnMagic != 0x4E444853U) { // "SHDN"
      AsciiSPrint (OutLine, LineSize, "[6] PARAM MAGIC MISMATCH: 0x%08X", ShdnMagic);
      FreePool (Buf);
      FreePool (Handles);
      return EFI_COMPROMISED_DATA;
    }

    if (OldVal != 1U && OldVal != 2U) {
      AsciiSPrint (OutLine, LineSize, "[6] PARAM UNEXPECTED VALUE 0x%X", OldVal);
      FreePool (Buf);
      FreePool (Handles);
      return EFI_COMPROMISED_DATA;
    }
    if (OldVal != 1U) {
      *(UINT32 *)(Buf + 0x00U) = 1U;
      Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0ULL, 4096U, Buf);
      if (!EFI_ERROR (Status)) {
        BlockIo->FlushBlocks (BlockIo);
      }
      AsciiSPrint (OutLine, LineSize, "[6] PARAM CLEARED 0x%X->1 (0x%lx)", OldVal, (UINT64)Status);
    } else {
      AsciiSPrint (OutLine, LineSize, "[6] PARAM ALREADY 1 (%u PARTS)", (UINT32)Count);
      Status = EFI_SUCCESS;
    }
    FreePool (Buf);
    FreePool (Handles);
    return Status;
  }

  AsciiSPrint (OutLine, LineSize, "[6] PARAM NOT FOUND (%u PARTS)", (UINT32)Count);
  FreePool (Handles);
  return EFI_NOT_FOUND;
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  CHAR8  Line[64];

  EFI_STATUS PhaseStatus;
  EFI_HANDLE LockHandle = NULL;
  VOID *LockInterface = NULL;

  // Standard firmware lifecycle: permit third-party EFI images only after DXE.
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  PhaseStatus = gBS->LocateProtocol (&gEfiDxeSmmReadyToLockProtocolGuid, NULL, &LockInterface);
  if (EFI_ERROR (PhaseStatus)) {
    PhaseStatus = gBS->InstallProtocolInterface (&LockHandle,
      &gEfiDxeSmmReadyToLockProtocolGuid, EFI_NATIVE_INTERFACE, NULL);
  }
  AsciiSPrint (Line, sizeof (Line), "[TEST5] DXE PHASE READY=0x%lx\n", (UINT64)PhaseStatus);
  LogSec (Line);
  mBootPhaseReady = !EFI_ERROR (PhaseStatus);
  LogSec ("[TEST5] PlatformBootManagerBeforeConsole\n");
  FbFillRect (0, 0, FB_WIDTH, FB_HEIGHT, 0xFF0A1428U);
  LogSec ("[TEST5] BC1:FbFillRect\n");
  mTextRow = 0;
  SetCheckpoint (0U, 0xFF00D8FFU); // Cyan
  LogSec ("[TEST5] BC2:Checkpoint0\n");
  FbPrintLine ("R5Q UEFI TEST 5 - DIAGNOSTIC DASHBOARD", 0xFF00D8FFU);
  LogSec ("[TEST5] BC3:Print1\n");
  AsciiSPrint (
    Line,
    sizeof (Line),
    "[1] BEFORE CONSOLE CTL0=0x%08X",
    MmioRead32 (CTL0_BASE + CTL_START_OFF)
    );
  FbPrintLine (Line, 0xFFF0F4F8U);
  KickDisplay ();
  LogSec ("[TEST5] BeforeConsole_END\n");
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_EVENT                     TickEvent;
  UINT64                        Freq;
  UINT64                        T0;
  UINT64                        T1;
  UINT32                        Sec;
  CHAR8                         Line[64];

  LogSec ("[TEST5] PlatformBootManagerAfterConsole\n");
  FbFillRect (0, 0, FB_WIDTH, FB_HEIGHT, 0xFF0A1428U);
  mTextRow = 0;
  SetCheckpoint (0U, 0xFF00D8FFU); // Cyan
  SetCheckpoint (1U, 0xFFFFE000U); // Yellow

  TickEvent = NULL;
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, KickTimer, NULL, &TickEvent))) {
    gBS->SetTimer (TickEvent, TimerPeriodic, 330000ULL);
  }

  FbPrintLine ("R5Q UEFI TEST 5 - AFTER CONSOLE", 0xFFFFE000U);

  Gop    = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && (Gop != NULL) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL)) {
    AsciiSPrint (
      Line,
      sizeof (Line),
      "[2] GOP %ux%u FB=0x%08lX",
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution,
      (UINT64)Gop->Mode->FrameBufferBase
      );
  } else {
    AsciiSPrint (Line, sizeof (Line), "[2] GOP STATUS=0x%lx", (UINT64)Status);
  }
  FbPrintLine (Line, 0xFFF0F4F8U);
  SetCheckpoint (2U, 0xFFFF8C00U); // Orange

  Freq = ReadCntFrq ();
  T0   = ReadCntPct ();
  gBS->Stall (100000U);
  T1 = ReadCntPct ();
  SetCheckpoint (3U, 0xFFB040FFU); // Purple
  AsciiSPrint (
    Line,
    sizeof (Line),
    "[3] TIMER FREQ=%lu DT=%lu IRQ=%u",
    Freq,
    (T1 - T0),
    mTimerTicks
    );
  FbPrintLine (Line, 0xFFF0F4F8U);

  SetCheckpoint (4U, 0xFF2080FFU); // Blue
  FbPrintLine ("[4] LAUNCHING XNULOADER (LOADER.EFI)...", 0xFF80C0FFU);
  // Clear the recovery-entry flag before executing code that might fault.
  Status = ClearSamsungParamFlag (Line, sizeof (Line));
  LogSec (Line);
  LogSec ("\n");
  FbPrintLine (Line, EFI_ERROR (Status) ? 0xFFFF8C00U : 0xFF40FF60U);

  if (!mBootPhaseReady || EFI_ERROR (Status)) {
    LogSec ("[TEST5] APP SKIPPED: phase/return guard failed\n");
    MmioWrite32 (PSHOLD_ADDR, 0U);
    CpuDeadLoop ();
  }
  Status = RunTestLoader (Line, sizeof (Line));
  LogSec (Line);
  LogSec ("\n");

  // Re-establish clean dashboard in case loader.efi cleared screen
  FbFillRect (0, DASH_TOP_Y + mTextRow * CHAR_ADV_Y, FB_WIDTH, CHAR_ADV_Y, 0xFF0A1428U);
  FbPrintLine (Line, EFI_ERROR (Status) ? 0xFFFF5050U : 0xFF40FF60U);
  if (!EFI_ERROR (Status)) {
    SetCheckpoint (5U, 0xFF20E040U); // Bright Green
    FbPrintLine ("[6] LOADER CHECK PASSED", 0xFF40FF60U);
  } else {
    SetCheckpoint (5U, 0xFFFF2020U); // Red
    FbPrintLine ("[6] LOADER CHECK RETURNED ERROR", 0xFFFF5050U);
  }


  // Live 8-second countdown with progress bar (under HW Wdog timeout)
  for (Sec = 8U; Sec > 0U; Sec--) {
    UINT64  WaitStart = ReadCntPct ();
    UINT32  BarW      = ((9U - Sec) * (FB_WIDTH - 48U)) / 8U;
    AsciiSPrint (Line, sizeof (Line), "AUTO RESET IN %2u S  (IRQ TICKS=%u)", Sec, mTimerTicks);
    FbFillRect (0, FB_HEIGHT - 110U, FB_WIDTH, 110U, 0xFF0A1428U);
    FbDrawTextAt (24U, FB_HEIGHT - 96U, Line, 0xFFF0F4F8U, 0xFF0A1428U);
    FbFillRect (24U, FB_HEIGHT - 50U, FB_WIDTH - 48U, 24U, 0xFF1E2D4AU);
    FbFillRect (24U, FB_HEIGHT - 50U, BarW, 24U, EFI_ERROR (Status) ? 0xFFFF4040U : 0xFF20E040U);
    KickDisplay ();
    while ((ReadCntPct () - WaitStart) < Freq) {
      CpuPause ();
    }
  }
  if (TickEvent != NULL) {
    gBS->SetTimer (TickEvent, TimerCancel, 0ULL);
    gBS->CloseEvent (TickEvent);
  }

  LogSec ("[TEST5] Resetting via PSHOLD...\n");
  MmioWrite32 (PSHOLD_ADDR, 0U);
  MemoryFence ();
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  for (;;) {
    CpuPause ();
  }
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  SetCheckpoint (5U, 0xFFFF2020U);
  FbPrintLine ("BDS UNABLE TO BOOT - RESETTING", 0xFFFF5050U);
  MmioWrite32 (PSHOLD_ADDR, 0U);
  CpuDeadLoop ();
}

VOID
EFIAPI
PlatformBootManagerBdsEntry (
  VOID
  )
{
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (
  IN OUT UINT16  **BootNext
  )
{
}

VOID
EFIAPI
PlatformBootManagerDeadloop (
  VOID
  )
{
  SetCheckpoint (5U, 0xFFFF2020U);
  FbPrintLine ("BDS DEADLOOP - RESETTING", 0xFFFF5050U);
  MmioWrite32 (PSHOLD_ADDR, 0U);
  CpuDeadLoop ();
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect (
  VOID
  )
{
}
