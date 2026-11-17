/*
 * r5qfull (Test 73+): the firmware's log kept from the start of the driver stage on the installed S10 Lite.
 *
 * A start on battery dies in the driver stage, before the boot manager's own log saves (R5qUfsSaveLog). So this
 * driver, loaded right after ResetSystemRuntimeDxe (APRIORI) and before Qualcomm's PMIC driver:
 *   - wraps gRT->ResetSystem: a shutdown or reset asked for by any driver saves the log first;
 *   - saves the log every second until the end of the driver stage (EndOfDxe), so a watchdog reset or a power cut
 *     leaves at most a second unsaved;
 * into slot 2 of the USERDATA log area (R5qUsbRamDisk.c's slots 0 and 1 are the boot manager's), and only when the
 * iOS install record is in USERDATA: then that space is ours. USERDATA is found from the GPT of the storage unit
 * itself (partitions are not connected this early): the entry named "userdata".
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Reset.h>
#include <Guid/EventGroup.h>

#define SEC_LOG_BASE      0xC1600000ULL
#define SEC_LOG_MAGIC     0x4D474F4CU         // "LOGM"
#define RING_BYTES        0x200000U
#define BLOCK             4096U
#define RECORD_LBA        ((4ULL << 30) / BLOCK)            // in USERDATA: the iOS install record
#define EARLY_LBA         (RECORD_LBA + 256ULL + 2ULL * 1024ULL)   // slots 2 and 3 of the log area: two starts
#define EARLY_SLOT        1024ULL
#define STATE_RUNNING     1U          // the start had not reached the end of the driver stage at this save
#define STATE_DONE        2U          // ... it had (a start that died stays RUNNING)
// header: magic[16] | saves u32 @16 | why[64] @32 | start number u32 @96 | state u32 @100

STATIC CONST CHAR8  mRecordMagic[16] = { 'R', '5', 'Q', '-', 'i', 'O', 'S', '-', 'I', 'M', 'A', 'G', 'E', '/', '1', 0 };
STATIC CONST CHAR8  mEarlyMagic[16]  = { 'R', '5', 'Q', 'E', 'A', 'R', 'L', 'Y', 'L', 'O', 'G', '/', '1', 0, 0, 0 };

STATIC EFI_RESET_SYSTEM       mOriginalReset;
STATIC EFI_EVENT              mTimer;
STATIC EFI_BLOCK_IO_PROTOCOL  *mUnit;           // the storage unit holding USERDATA
STATIC EFI_LBA                mUserdata;        // its first block
STATIC BOOLEAN                mOurs, mGaveUp;
STATIC UINT32                 mLooks, mSaves;
STATIC UINT8                  *mBuf;            // a header block + the ring
STATIC UINT32                 mSlot = 0xFFFFFFFF;   // this start's slot (0 or 1 of the two)
STATIC UINT32                 mStart;           // this start's number
STATIC UINT32                 mState = STATE_RUNNING;

//
// This start's slot: never the one holding a start that died (RUNNING) when the other can be had; otherwise the
// older of the two. Read once, before the first save.
//
STATIC VOID
ChooseSlot (
  VOID
  )
{
  UINT32  Start[2] = { 0, 0 }, State[2] = { 0, 0 }, S;

  for (S = 0; S < 2; S++) {
    if (!EFI_ERROR (mUnit->ReadBlocks (mUnit, mUnit->Media->MediaId, mUserdata + EARLY_LBA + S * EARLY_SLOT, BLOCK, mBuf)) &&
        (CompareMem (mBuf, mEarlyMagic, sizeof (mEarlyMagic)) == 0))
    {
      Start[S] = ReadUnaligned32 ((UINT32 *)(mBuf + 96));
      State[S] = ReadUnaligned32 ((UINT32 *)(mBuf + 100));
    }
  }
  if ((State[0] == STATE_RUNNING) && (State[1] != STATE_RUNNING)) {
    mSlot = 1;                    // slot 0 holds a start that died: keep it
  } else if ((State[1] == STATE_RUNNING) && (State[0] != STATE_RUNNING)) {
    mSlot = 0;
  } else {
    mSlot = (Start[0] <= Start[1]) ? 0 : 1;   // the older one
  }
  mStart = MAX (Start[0], Start[1]) + 1;
}

STATIC VOID
Say (
  IN CONST CHAR8  *Msg
  )
{
  volatile UINT32  *Hdr = (volatile UINT32 *)(UINTN)SEC_LOG_BASE;
  volatile UINT8   *Buf = (volatile UINT8 *)(UINTN)(SEC_LOG_BASE + 16);
  UINT32           W;

  if (Hdr[1] != SEC_LOG_MAGIC) {
    return;
  }
  W = Hdr[2];
  for (UINTN I = 0; Msg[I] != 0; I++, W++) {
    Buf[W % (RING_BYTES - 16)] = (UINT8)Msg[I];
  }
  Hdr[2] = W;
}

//
// The unit with a GPT entry named "userdata", and the install record in it. TRUE when the log area is ours.
//
STATIC BOOLEAN
FindUserdata (
  VOID
  )
{
  EFI_HANDLE             *H = NULL;
  UINTN                  N  = 0, I, E;
  EFI_BLOCK_IO_PROTOCOL  *B;
  UINT8                  *Blk = mBuf;
  UINT64                 EntriesLba;
  UINT32                 Count, Size;
  CHAR8                  L[128];

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &N, &H)) || (H == NULL)) {
    return FALSE;
  }
  for (I = 0; (I < N) && !mOurs; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (H[I], &gEfiBlockIoProtocolGuid, (VOID **)&B)) || (B == NULL) || (B->Media == NULL) ||
        B->Media->LogicalPartition || (B->Media->BlockSize != BLOCK) || ((B->Media->LastBlock + 1) < (32ULL << 30) / BLOCK))
    {
      continue;
    }
    if (EFI_ERROR (B->ReadBlocks (B, B->Media->MediaId, 1, BLOCK, Blk)) || (CompareMem (Blk, "EFI PART", 8) != 0)) {
      continue;
    }
    EntriesLba = ReadUnaligned64 ((UINT64 *)(Blk + 72));
    Count      = ReadUnaligned32 ((UINT32 *)(Blk + 80));
    Size       = ReadUnaligned32 ((UINT32 *)(Blk + 84));
    if ((Size < 128) || (Count == 0) || (Count > 512) || ((UINT64)Count * Size > RING_BYTES) ||
        EFI_ERROR (B->ReadBlocks (B, B->Media->MediaId, EntriesLba, ALIGN_VALUE ((UINTN)Count * Size, BLOCK), Blk)))
    {
      continue;
    }
    for (E = 0; E < Count; E++) {
      UINT8          *Ent = Blk + E * Size;
      CONST CHAR16   *Name = (CONST CHAR16 *)(Ent + 56);
      STATIC CONST CHAR16  Want[] = L"userdata";
      UINTN          K;

      for (K = 0; K < 9 && ReadUnaligned16 ((UINT16 *)&Name[K]) == Want[K]; K++) {
      }
      if (K == 9) {
        mUnit     = B;
        mUserdata = ReadUnaligned64 ((UINT64 *)(Ent + 32));
        break;
      }
    }
    if ((mUnit != NULL) && !EFI_ERROR (B->ReadBlocks (B, B->Media->MediaId, mUserdata + RECORD_LBA, BLOCK, Blk)) &&
        (CompareMem (Blk, mRecordMagic, sizeof (mRecordMagic)) == 0))
    {
      mOurs = TRUE;
      AsciiSPrint (L, sizeof (L), "[EARLYLOG] USERDATA at block %lu with iOS installed: the log is kept from now on\n", mUserdata);
      Say (L);
    }
  }
  FreePool (H);
  return mOurs;
}

STATIC VOID
Save (
  IN CONST CHAR8  *Why
  )
{
  volatile UINT64  *Src = (volatile UINT64 *)(UINTN)SEC_LOG_BASE;
  CHAR8            L[128];

  if ((mBuf == NULL) || (!mOurs && (mGaveUp || !FindUserdata ()))) {
    return;
  }
  if (mSlot > 1) {
    ChooseSlot ();
  }
  mSaves++;
  AsciiSPrint (L, sizeof (L), "[EARLYLOG] start %u, save %u (slot %u): %a\n", mStart, mSaves, mSlot, Why);
  Say (L);
  ZeroMem (mBuf, BLOCK);
  CopyMem (mBuf, mEarlyMagic, sizeof (mEarlyMagic));
  WriteUnaligned32 ((UINT32 *)(mBuf + 16), mSaves);
  AsciiStrnCpyS ((CHAR8 *)(mBuf + 32), 64, Why, 63);
  WriteUnaligned32 ((UINT32 *)(mBuf + 96), mStart);
  WriteUnaligned32 ((UINT32 *)(mBuf + 100), mState);
  for (UINTN I = 0; I < RING_BYTES / 8; I++) {
    ((UINT64 *)(mBuf + BLOCK))[I] = Src[I];
  }
  if (!EFI_ERROR (mUnit->WriteBlocks (mUnit, mUnit->Media->MediaId, mUserdata + EARLY_LBA + mSlot * EARLY_SLOT, BLOCK + RING_BYTES, mBuf))) {
    mUnit->FlushBlocks (mUnit);
  }
}

STATIC VOID EFIAPI
Tick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mLooks++;
  if (!mOurs && (mLooks > 60)) {
    mGaveUp = TRUE;               // (a minute without finding iOS installed: not this phone's layout)
    return;
  }
  Save ("every second in the driver stage");
}

STATIC VOID EFIAPI
EndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mTimer != NULL) {
    gBS->SetTimer (mTimer, TimerCancel, 0);
    gBS->CloseEvent (mTimer);
    mTimer = NULL;
  }
  mState = STATE_DONE;            // this start got through the driver stage: its slot may be reused
  Save ("end of the driver stage");
  gBS->CloseEvent (Event);
}

//
// Does the last part of the RAM log say this? (What the driver that asks for a reset printed just before.)
//
STATIC BOOLEAN
RingTailSays (
  IN CONST CHAR8  *Text
  )
{
  volatile UINT32  *Hdr = (volatile UINT32 *)(UINTN)SEC_LOG_BASE;
  volatile UINT8   *Buf = (volatile UINT8 *)(UINTN)(SEC_LOG_BASE + 16);
  CHAR8            Tail[257];
  UINT32           W, I, Cap = RING_BYTES - 16;
  UINTN            N = AsciiStrLen (Text);

  if (Hdr[1] != SEC_LOG_MAGIC) {
    return FALSE;
  }
  W = Hdr[2];
  for (I = 0; I < 256; I++) {
    Tail[I] = (W >= 256 - I) ? (CHAR8)Buf[(W - (256 - I)) % Cap] : ' ';
  }
  Tail[256] = 0;
  for (I = 0; I + N <= 256; I++) {
    if (CompareMem (Tail + I, Text, N) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC VOID EFIAPI
WrappedReset (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  CHAR8  Why[64];

  AsciiSPrint (Why, sizeof (Why), "ResetSystem(%a) asked for", ResetType == EfiResetShutdown ? "SHUTDOWN" :
    ResetType == EfiResetCold ? "cold" : ResetType == EfiResetWarm ? "warm" : "platform");
  if (EfiGetCurrentTpl () <= TPL_CALLBACK) {
    Save (Why);
  } else {
    Say ("[EARLYLOG] ResetSystem at a high TPL: not saved\n");
  }
  //
  // Test 76: Qualcomm's PmicDxe, started on a power-key start, measures how long the key was held and turns the phone
  // off as an accidental press ("Invalid pwrkey press. Shutting down!") -- it runs seconds after the press here (after
  // Samsung's bootloader, which has already accepted the press, BootShim and SEC), so the key is always up by then
  // and every start on battery ended. That one shutdown, in the driver stage, is declined: PmicDxe carries on
  // (its helper only returns a status). Every other shutdown or reset goes through.
  //
  // (Test 78: ButtonsDxe makes the same check after PmicDxe -- "PowerKeyPressed [ms] 0 / Power off device" -- and
  // likewise only returns a status after asking: declined too.)
  if ((ResetType == EfiResetShutdown) && (mState == STATE_RUNNING) &&
      (RingTailSays ("Invalid pwrkey press") || RingTailSays ("Power off device")))
  {
    Say ("[EARLYLOG] a driver's power-key shutdown DECLINED: Samsung's bootloader already accepted the press\n");
    return;
  }
  mOriginalReset (ResetType, ResetStatus, DataSize, ResetData);
}

STATIC VOID
Hook (
  VOID
  )
{
  if ((mOriginalReset != NULL) || (gRT->ResetSystem == NULL)) {
    return;
  }
  mOriginalReset   = gRT->ResetSystem;
  gRT->ResetSystem = WrappedReset;
  gRT->Hdr.CRC32   = 0;
  gBS->CalculateCrc32 (gRT, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);
  Say ("[EARLYLOG] ResetSystem wrapped: a shutdown or reset saves the log first\n");
}

STATIC VOID EFIAPI
ResetArchReady (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  VOID  *P;

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiResetArchProtocolGuid, NULL, &P))) {
    Hook ();
    gBS->CloseEvent (Event);
  }
}

EFI_STATUS
EFIAPI
R5qEarlyLogEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Ev;
  VOID       *Reg, *P;

  mBuf = AllocatePages (EFI_SIZE_TO_PAGES (BLOCK + RING_BYTES));
  Say ("[EARLYLOG] driver loaded\n");
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiResetArchProtocolGuid, NULL, &P))) {
    Hook ();
  } else if (!EFI_ERROR (gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ResetArchReady, NULL, &Ev))) {
    gBS->RegisterProtocolNotify (&gEfiResetArchProtocolGuid, Ev, &Reg);
  }
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Tick, NULL, &mTimer))) {
    gBS->SetTimer (mTimer, TimerPeriodic, 10000000ULL);    // every second (100 ns units)
  }
  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EndOfDxe, NULL, &gEfiEndOfDxeEventGroupGuid, &Ev);
  Save ("driver loaded");
  return EFI_SUCCESS;
}
