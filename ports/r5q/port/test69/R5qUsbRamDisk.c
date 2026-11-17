//
// r5qfull (Test 46+): the phone shows up on the PC as a USB drive backed only by phone RAM. Qualcomm's UsbMsdDxe
// (EFI_USB_MSD_PROTOCOL, as in QcomPkg's MassStorage application) serves a RAM-backed Block IO that this code
// formats as FAT32 (MBR, one partition, label R5QRAM). Windows' inbox mass-storage driver needs no install and no
// vendor requests. The PC copies files onto the drive, then DONE.TXT ("<name> <size> <crc32 hex>" lines). The
// firmware then finds each file by walking the FAT, rebuilds it contiguously in RAM, checks its size and CRC32, and
// logs the result. No UFS/Block IO device of the phone is ever assigned to USB: only the RAM disk.
//
// Included into FirstBootManager.c (uses LogSec, FbPrintLine, R5qPersistLog, ReadCntPct and ReadCntFrq).
//

#define R5Q_MSD_GUID  { 0xC8591FAF, 0xDBCC, 0x479E, { 0x9E, 0xF2, 0xFD, 0x08, 0x5B, 0xC3, 0x7B, 0xC7 } }

typedef struct _R5Q_USB_MSD R5Q_USB_MSD;
struct _R5Q_USB_MSD {
  UINT32        Revision;
  EFI_STATUS    (EFIAPI *AssignBlkIoHandle)(R5Q_USB_MSD *This, EFI_BLOCK_IO_PROTOCOL *BlkIo, UINT32 Lun);
  EFI_STATUS    (EFIAPI *QueryMaxLun)(R5Q_USB_MSD *This, UINT8 *Count);
  EFI_STATUS    (EFIAPI *EventHandler)(R5Q_USB_MSD *This);
  EFI_STATUS    (EFIAPI *StartDevice)(R5Q_USB_MSD *This);
  EFI_STATUS    (EFIAPI *StopDevice)(R5Q_USB_MSD *This);
  VOID          *GetDeviceSpeed;
  VOID          *UnmountHandle;
  VOID          *MountHandle;
  VOID          *FindPartitions;
};

#define R5Q_RD_BYTES      0xC0000000ULL     // 3 GiB RAM disk
#define R5Q_RD_SECTOR     512U
#define R5Q_RD_PART_LBA   2048U
#define R5Q_FAT_RSVD      32U
#define R5Q_FAT_SPC       64U               // 32 KiB clusters

STATIC UINT8                  *mRd;
STATIC EFI_BLOCK_IO_MEDIA     mRdMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mRdBio;
STATIC UINT32                 mRdFatSz;
STATIC UINT64                 mRdReads;
STATIC UINT64                 mRdWrites;

STATIC EFI_STATUS EFIAPI
RdReset (IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN Ext)
{
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
RdRead (IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba, IN UINTN Size, OUT VOID *Buf)
{
  if ((Size % R5Q_RD_SECTOR) != 0 || (Lba * R5Q_RD_SECTOR + Size) > R5Q_RD_BYTES || (Buf == NULL && Size != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  CopyMem (Buf, mRd + Lba * R5Q_RD_SECTOR, Size);
  mRdReads += Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
RdWrite (IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba, IN UINTN Size, IN VOID *Buf)
{
  if ((Size % R5Q_RD_SECTOR) != 0 || (Lba * R5Q_RD_SECTOR + Size) > R5Q_RD_BYTES || (Buf == NULL && Size != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  CopyMem (mRd + Lba * R5Q_RD_SECTOR, Buf, Size);
  mRdWrites += Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
RdFlush (IN EFI_BLOCK_IO_PROTOCOL *This)
{
  return EFI_SUCCESS;
}

STATIC VOID
Put16 (UINT8 *P, UINT16 V)
{
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8);
}

STATIC VOID
Put32 (UINT8 *P, UINT32 V)
{
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); P[2] = (UINT8)(V >> 16); P[3] = (UINT8)(V >> 24);
}

STATIC UINT32
Get32 (CONST UINT8 *P)
{
  return P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC UINT8 *
RdPart (VOID)
{
  return mRd + (UINT64)R5Q_RD_PART_LBA * R5Q_RD_SECTOR;
}

// MBR + FAT32 (reserved 32, two FATs, root at cluster 2, label R5QRAM)
STATIC VOID
RdFormat (VOID)
{
  UINT32  Part = (UINT32)(R5Q_RD_BYTES / R5Q_RD_SECTOR) - R5Q_RD_PART_LBA;
  UINT32  Clusters;
  UINT8   *M = mRd;
  UINT8   *B = RdPart ();
  UINT8   *Fat;
  UINT32  I;

  Clusters  = (Part - R5Q_FAT_RSVD) / R5Q_FAT_SPC;
  mRdFatSz  = ((Clusters + 2) * 4 + R5Q_RD_SECTOR - 1) / R5Q_RD_SECTOR;
  ZeroMem (M, (UINT64)R5Q_RD_PART_LBA * R5Q_RD_SECTOR);
  ZeroMem (B, (UINT64)(R5Q_FAT_RSVD + 2 * mRdFatSz + R5Q_FAT_SPC) * R5Q_RD_SECTOR);

  Put32 (M + 440, 0x52355152U);                // disk signature
  M[446 + 0] = 0x00; M[446 + 1] = 0xFE; M[446 + 2] = 0xFF; M[446 + 3] = 0xFF;
  M[446 + 4] = 0x0C;                           // FAT32 LBA
  M[446 + 5] = 0xFE; M[446 + 6] = 0xFF; M[446 + 7] = 0xFF;
  Put32 (M + 446 + 8, R5Q_RD_PART_LBA);
  Put32 (M + 446 + 12, Part);
  M[510] = 0x55; M[511] = 0xAA;

  B[0] = 0xEB; B[1] = 0x58; B[2] = 0x90;
  CopyMem (B + 3, "MSWIN4.1", 8);
  Put16 (B + 11, R5Q_RD_SECTOR);
  B[13] = R5Q_FAT_SPC;
  Put16 (B + 14, R5Q_FAT_RSVD);
  B[16] = 2;
  B[21] = 0xF8;
  Put16 (B + 24, 63);
  Put16 (B + 26, 255);
  Put32 (B + 28, R5Q_RD_PART_LBA);
  Put32 (B + 32, Part);
  Put32 (B + 36, mRdFatSz);
  Put32 (B + 44, 2);                           // root cluster
  Put16 (B + 48, 1);                           // FSInfo
  Put16 (B + 50, 6);                           // backup boot sector
  B[64] = 0x80;
  B[66] = 0x29;
  Put32 (B + 67, 0x52355152U);
  CopyMem (B + 71, "R5QRAM     ", 11);
  CopyMem (B + 82, "FAT32   ", 8);
  B[510] = 0x55; B[511] = 0xAA;
  // FSInfo
  Put32 (B + 512 + 0, 0x41615252U);
  Put32 (B + 512 + 484, 0x61417272U);
  Put32 (B + 512 + 488, 0xFFFFFFFFU);
  Put32 (B + 512 + 492, 3);
  Put32 (B + 512 + 508, 0xAA550000U);
  CopyMem (B + 6 * 512, B, 1024);              // backup boot sector + FSInfo
  for (I = 0; I < 2; I++) {
    Fat = B + (UINT64)(R5Q_FAT_RSVD + I * mRdFatSz) * R5Q_RD_SECTOR;
    Put32 (Fat + 0, 0x0FFFFFF8U);
    Put32 (Fat + 4, 0x0FFFFFFFU);
    Put32 (Fat + 8, 0x0FFFFFFFU);              // root directory: one cluster
  }
  // root directory: volume label entry
  Fat = B + (UINT64)(R5Q_FAT_RSVD + 2 * mRdFatSz) * R5Q_RD_SECTOR;
  CopyMem (Fat, "R5QRAM     ", 11);
  Fat[11] = 0x08;
}

STATIC UINT8 *
RdCluster (UINT32 C)
{
  return RdPart () + ((UINT64)R5Q_FAT_RSVD + 2ULL * mRdFatSz + (UINT64)(C - 2) * R5Q_FAT_SPC) * R5Q_RD_SECTOR;
}

STATIC UINT32
RdNext (UINT32 C)
{
  return Get32 (RdPart () + (UINT64)R5Q_FAT_RSVD * R5Q_RD_SECTOR + (UINT64)C * 4) & 0x0FFFFFFFU;
}

STATIC UINT32
RdMaxCluster (VOID)
{
  return mRdFatSz * (R5Q_RD_SECTOR / 4);
}

//
// Finds an 8.3 name ("DONE    TXT") in the root directory. Returns FALSE if absent.
//
STATIC BOOLEAN
RdFind (
  IN  CONST CHAR8  *Name11,
  OUT UINT32       *First,
  OUT UINT32       *Size
  )
{
  UINT32  C = 2, Hops = 0;
  UINT32  I;
  UINT8   *E;

  while (C >= 2 && C < 0x0FFFFFF8U && C < RdMaxCluster () && Hops++ < 4096) {
    for (I = 0; I < R5Q_FAT_SPC * R5Q_RD_SECTOR; I += 32) {
      E = RdCluster (C) + I;
      if (E[0] == 0x00) {
        return FALSE;
      }
      if (E[0] == 0xE5 || E[11] == 0x0F || (E[11] & 0x08)) {
        continue;
      }
      if (CompareMem (E, Name11, 11) == 0) {
        *First = ((UINT32)E[20] | ((UINT32)E[21] << 8)) << 16 | E[26] | ((UINT32)E[27] << 8);
        *Size  = Get32 (E + 28);
        return TRUE;
      }
    }
    C = RdNext (C);
  }
  return FALSE;
}

//
// Copies a file's cluster chain into Dest (Size bytes). Returns the number of fragments, 0 on a broken chain.
//
STATIC UINT32
RdGather (
  IN  UINT32  First,
  IN  UINT32  Size,
  OUT UINT8   *Dest
  )
{
  UINT32  C = First, Prev = 0, Frags = 0;
  UINT64  Done = 0, N;
  UINT64  Csz = (UINT64)R5Q_FAT_SPC * R5Q_RD_SECTOR;

  while (Done < Size) {
    if (C < 2 || C >= RdMaxCluster () || C >= 0x0FFFFFF8U) {
      return 0;
    }
    if (C != Prev + 1) {
      Frags++;
    }
    N = MIN (Csz, (UINT64)Size - Done);
    CopyMem (Dest + Done, RdCluster (C), N);
    Done += N;
    Prev  = C;
    C     = RdNext (C);
  }
  return Frags;
}

STATIC VOID
RdName11 (
  IN  CONST CHAR8  *Name,
  OUT CHAR8        *Out11
  )
{
  UINTN  I = 0, J = 0;

  SetMem (Out11, 11, ' ');
  while (Name[I] != '\0' && Name[I] != '.' && J < 8) {
    Out11[J++] = Name[I++];
  }
  while (Name[I] != '\0' && Name[I] != '.') {
    I++;
  }
  if (Name[I] == '.') {
    I++;
    for (J = 8; Name[I] != '\0' && J < 11; I++, J++) {
      Out11[J] = Name[I];
    }
  }
}

#define R5Q_RESIDENT_PA  0xFC000000ULL     // the loader's resident image address (window 0xF4000000 + 0x08000000)
#define R5Q_RD_PA        0x1A0000000ULL    // RAM disk: above the resident image, below the top-of-RAM allocations

STATIC EFI_PHYSICAL_ADDRESS  mR5qResidentPa;
STATIC UINTN                 mR5qResidentPages;
STATIC UINT32                mR5qResidentSize;

//
// DONE.TXT lines: "<NAME.EXT> <size> <crc32 hex>". Each file is gathered and checked.
//
STATIC UINT32
RdCheckFiles (
  IN UINT32  DoneFirst,
  IN UINT32  DoneSize
  )
{
  CHAR8                 Text[1025];
  CHAR8                 *P, *Name, *SizeS, *CrcS;
  CHAR8                 Name11[11];
  UINT32                First, Size, Frags, Crc, Want, WantSize, Ok = 0;
  EFI_PHYSICAL_ADDRESS  A;
  CHAR8                 L[96];
  BOOLEAN               IsSystem;

  if (DoneSize > 1024 || RdGather (DoneFirst, DoneSize, (UINT8 *)Text) == 0) {
    LogSec ("[MSD] DONE.TXT unreadable\n");
    return 0;
  }
  Text[DoneSize] = '\0';
  P = Text;
  while (*P != '\0') {
    while (*P == '\r' || *P == '\n' || *P == ' ') {
      P++;
    }
    if (*P == '\0') {
      break;
    }
    Name = P;
    while (*P != ' ' && *P != '\0') P++;
    if (*P) *P++ = '\0';
    SizeS = P;
    while (*P != ' ' && *P != '\0') P++;
    if (*P) *P++ = '\0';
    CrcS = P;
    while (*P != '\r' && *P != '\n' && *P != '\0') P++;
    if (*P) *P++ = '\0';
    WantSize = (UINT32)AsciiStrDecimalToUintn (SizeS);
    Want     = (UINT32)AsciiStrHexToUintn (CrcS);
    RdName11 (Name, Name11);
    if (!RdFind (Name11, &First, &Size)) {
      AsciiSPrint (L, sizeof (L), "[MSD] %a: not found\n", Name);
      LogSec (L);
      continue;
    }
    // Test 51: SYSTEM.APF (the iOS system volume) goes to the loader's resident address: window base 0xF4000000
    // + RESIDENT_RD_OFFSET 0x08000000. Released (not cleared: PcdDebugPropertyMask 0x27) before the loader runs.
    IsSystem = (CompareMem (Name11, "SYSTEM  APF", 11) == 0);
    A        = IsSystem ? R5Q_RESIDENT_PA : 0x27F000000ULL;
    if (EFI_ERROR (gBS->AllocatePages (IsSystem ? AllocateAddress : AllocateMaxAddress, EfiLoaderData, EFI_SIZE_TO_PAGES (Size), &A))) {
      AsciiSPrint (L, sizeof (L), "[MSD] %a: no memory for %u bytes%a\n", Name, Size, IsSystem ? " at the resident address" : "");
      LogSec (L);
      continue;
    }
    Frags = RdGather (First, Size, (UINT8 *)(UINTN)A);
    Crc   = 0;
    gBS->CalculateCrc32 ((VOID *)(UINTN)A, Size, &Crc);
    AsciiSPrint (L, sizeof (L), "[MSD] %a: %u bytes in %u fragments at 0x%lx crc32 %08x: %a\n", Name, Size, Frags, (UINT64)A, Crc,
      (Size == WantSize && Crc == Want && Frags != 0) ? "MATCH" : "MISMATCH");
    LogSec (L);
    if (Size == WantSize && Crc == Want && Frags != 0) {
      Ok++;
      if (IsSystem) {
        mR5qResidentPa    = A;
        mR5qResidentPages = EFI_SIZE_TO_PAGES (Size);
        mR5qResidentSize  = Size;
      }
      AsciiSPrint (L, sizeof (L), "[M] %a OK, %u MB", Name, Size >> 20);
      FbPrintLine (L, 0xFF40FF60U);
    } else {
      if (IsSystem) {
        gBS->FreePages (A, EFI_SIZE_TO_PAGES (Size));
      }
      AsciiSPrint (L, sizeof (L), "[M] %a BAD", Name);
      FbPrintLine (L, 0xFFFF5050U);
    }
  }
  return Ok;
}

#ifndef R5Q_HOST_TEST
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

STATIC UINT64  mR5qEpoch;          // Test 66: the PC's clock from EPOCH.TXT (0: none)
STATIC UINT64  mR5qEpochCnt;       // CNTPCT when it was read

//
// Test 69: the time without a PC -- the PMIC's real-time clock, through the firmware's runtime service (Android keeps
// it in UTC). Days from the civil date (Howard Hinnant's algorithm), for EFI_TIME's year/month/day.
//
STATIC UINT64
R5qEpochFromEfiTime (
  IN CONST EFI_TIME  *T
  )
{
  INT64   Y   = (INT64)T->Year - (T->Month <= 2 ? 1 : 0);
  INT64   Era = (Y >= 0 ? Y : Y - 399) / 400;
  UINT64  Yoe = (UINT64)(Y - Era * 400);
  UINT64  Mp  = (T->Month + 9) % 12;
  UINT64  Doy = (153 * Mp + 2) / 5 + T->Day - 1;
  UINT64  Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  INT64   Days = Era * 146097 + (INT64)Doe - 719468;

  return (UINT64)Days * 86400ULL + T->Hour * 3600ULL + T->Minute * 60ULL + T->Second;
}

STATIC VOID
R5qEpochFromRtc (
  VOID
  )
{
  EFI_TIME    T;
  EFI_STATUS  S;
  CHAR8       L[128];

  ZeroMem (&T, sizeof (T));
  S = gRT->GetTime (&T, NULL);
  AsciiSPrint (L, sizeof (L), "[TIME] RTC: 0x%lx, %04u-%02u-%02u %02u:%02u:%02u zone %d\n", (UINT64)S, T.Year, T.Month, T.Day,
    T.Hour, T.Minute, T.Second, T.TimeZone);
  LogSec (L);
  if (!EFI_ERROR (S) && (T.Year >= 2024) && (T.Year < 2100) && (T.Month >= 1) && (T.Month <= 12) && (T.Day >= 1)) {
    mR5qEpoch = R5qEpochFromEfiTime (&T);
    if ((T.TimeZone != EFI_UNSPECIFIED_TIMEZONE) && (T.TimeZone >= -1440) && (T.TimeZone <= 1440)) {
      mR5qEpoch += (INT64)T.TimeZone * 60;     // (EFI: local = UTC - TimeZone minutes)
    }
    mR5qEpochCnt = ReadCntPct ();
  }
}

//
// Test 69: the system image from a file system the firmware reads by itself -- a microSD card (Qualcomm SdccDxe, FAT) or
// any FAT volume -- instead of the PC over USB: \SYSTEM.APF read whole to the resident address, its CRC32 checked against
// \SYSTEM.CRC (eight hex digits, written by the PC with the image). Reads only. TRUE: the image is in place.
//
#define R5Q_FS_CHUNK  (64U << 20)

STATIC BOOLEAN
R5qFileSystemLoad (
  VOID
  )
{
  EFI_HANDLE                       *H = NULL;
  UINTN                            N  = 0, I;
  EFI_STATUS                       S;
  CHAR8                            L[160];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root, *F, *C;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize, Len;
  UINT64                           Size, Done, T0, Frq;
  EFI_PHYSICAL_ADDRESS             A;
  UINT32                           Crc, Want;
  CHAR8                            Text[16];

  EfiBootManagerConnectAll ();     // (SdccDxe and UFSDxe give block devices; PartitionDxe and Fat give file systems)
  S = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &N, &H);
  AsciiSPrint (L, sizeof (L), "[FS] file systems the firmware can read: %u (0x%lx)\n", (UINT32)N, (UINT64)S);
  LogSec (L);
  for (I = 0; !EFI_ERROR (S) && (I < N); I++) {
    EFI_DEVICE_PATH_PROTOCOL  *Dp = DevicePathFromHandle (H[I]);
    CHAR16                    *DpText = (Dp != NULL) ? ConvertDevicePathToText (Dp, TRUE, TRUE) : NULL;

    AsciiSPrint (L, sizeof (L), "[FS] %u: %s\n", (UINT32)I, (DpText != NULL) ? DpText : L"(no path)");
    LogSec (L);
    if (DpText != NULL) {
      FreePool (DpText);
    }
    if (EFI_ERROR (gBS->HandleProtocol (H[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    if (EFI_ERROR (Root->Open (Root, &F, L"\\SYSTEM.APF", EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root);
      continue;
    }
    InfoSize = SIZE_OF_EFI_FILE_INFO + 64;
    Info     = AllocateZeroPool (InfoSize);
    Size     = 0;
    if ((Info != NULL) && !EFI_ERROR (F->GetInfo (F, &gEfiFileInfoGuid, &InfoSize, Info))) {
      Size = Info->FileSize;
    }
    if (Info != NULL) {
      FreePool (Info);
    }
    Want = 0;
    if (!EFI_ERROR (Root->Open (Root, &C, L"\\SYSTEM.CRC", EFI_FILE_MODE_READ, 0))) {
      Len = sizeof (Text) - 1;
      ZeroMem (Text, sizeof (Text));
      if (!EFI_ERROR (C->Read (C, &Len, Text))) {
        Want = (UINT32)AsciiStrHexToUintn (Text);
      }
      C->Close (C);
    }
    AsciiSPrint (L, sizeof (L), "[FS] %u: SYSTEM.APF, %lu bytes; SYSTEM.CRC %08x\n", (UINT32)I, Size, Want);
    LogSec (L);
    // the resident image must end below the RAM disk's address and the loader's window
    if ((Size < (1ULL << 30)) || (Size > (R5Q_RD_PA - R5Q_RESIDENT_PA)) || (Want == 0)) {
      LogSec ("[FS] not taken: size out of range or no SYSTEM.CRC\n");
      F->Close (F);
      Root->Close (Root);
      continue;
    }
    A = R5Q_RESIDENT_PA;
    if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiLoaderData, EFI_SIZE_TO_PAGES (Size), &A))) {
      LogSec ("[FS] no memory at the resident address\n");
      F->Close (F);
      Root->Close (Root);
      break;
    }
    FbPrintLine ("[M] LOADING iOS FROM THE CARD", 0xFF80C0FFU);
    Frq  = ReadCntFrq ();
    T0   = ReadCntPct ();
    Done = 0;
    Crc  = 0;
    while (Done < Size) {
      Len = (UINTN)MIN ((UINT64)R5Q_FS_CHUNK, Size - Done);
      S   = F->Read (F, &Len, (VOID *)(UINTN)(A + Done));
      if (EFI_ERROR (S) || (Len == 0)) {
        break;
      }
      Done += Len;
      if ((Done % (512ULL << 20)) == 0) {
        AsciiSPrint (L, sizeof (L), "[FS] %lu MB in %lu ms\n", Done >> 20, ((ReadCntPct () - T0) * 1000ULL) / Frq);
        LogSec (L);
      }
    }
    F->Close (F);
    Root->Close (Root);
    if (Done == Size) {
      gBS->CalculateCrc32 ((VOID *)(UINTN)A, (UINTN)Size, &Crc);
    }
    AsciiSPrint (L, sizeof (L), "[FS] read %lu of %lu bytes in %lu ms; crc32 %08x: %a\n", Done, Size,
      ((ReadCntPct () - T0) * 1000ULL) / Frq, Crc, (Done == Size && Crc == Want) ? "MATCH" : "MISMATCH");
    LogSec (L);
    if ((Done == Size) && (Crc == Want)) {
      mR5qResidentPa    = A;
      mR5qResidentPages = EFI_SIZE_TO_PAGES (Size);
      mR5qResidentSize  = (UINT32)Size;
      FbPrintLine ("[M] iOS LOADED FROM THE CARD", 0xFF40FF60U);
      FreePool (H);
      return TRUE;
    }
    gBS->FreePages (A, EFI_SIZE_TO_PAGES (Size));
    FbPrintLine ("[M] CARD IMAGE BAD: USING USB", 0xFFFF8C00U);
  }
  if (H != NULL) {
    FreePool (H);
  }
  return FALSE;
}

STATIC VOID EFIAPI
R5qNoopNotify (IN EFI_EVENT Event, IN VOID *Context)
{
}

//
// One session: expose the RAM disk, wait up to WaitS seconds for DONE.TXT, check the files, stop the device.
//
STATIC VOID
R5qUsbRamDiskSession (
  IN UINT32  WaitS
  )
{
  STATIC EFI_GUID       MsdGuid = R5Q_MSD_GUID;
  R5Q_USB_MSD           *Msd = NULL;
  EFI_STATUS            S;
  EFI_PHYSICAL_ADDRESS  A = R5Q_RD_PA;
  UINT8                 MaxLun = 0;
  UINT64                F, T0, LastLog;
  UINT32                DoneFirst = 0, DoneSize = 0, Ok;
  CHAR8                 L[96];
  BOOLEAN               Found = FALSE;

  // UsbMsd (and our own UsbfnIo client) need EFI_USBFN_IO_PROTOCOL. UsbConfigDxe starts the USB cores only when the
  // gUsbControllerInitGuid event group is signalled (Qualcomm's DeviceBootManagerOnDemandConInConnect does that; our
  // r5qfull build uses the Null device boot manager), then UsbfnDwc3Dxe binds to the core handle on connect.
  // Test 47: connect-all alone left 0 UsbfnIo handles. Test 48: signal the group first, then connect, then poll.
  {
    STATIC EFI_GUID  FnGuid      = EFI_USBFN_IO_PROTOCOL_GUID;
    STATIC EFI_GUID  UsbInitGuid = { 0x1C0CFFCE, 0xFC8D, 0x4E44, { 0x8C, 0x78, 0x9C, 0x9E, 0x5B, 0x53, 0x0D, 0x36 } };
    EFI_HANDLE       *H     = NULL;
    UINTN            Before = 0, After = 0, Tries;
    UINT64           Tc     = ReadCntPct ();
    EFI_EVENT        Ev     = NULL;

    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &FnGuid, NULL, &Before, &H)) && (H != NULL)) {
      FreePool (H);
      H = NULL;
    }
    S = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, R5qNoopNotify, NULL, &UsbInitGuid, &Ev);
    if (!EFI_ERROR (S)) {
      gBS->SignalEvent (Ev);
      gBS->CloseEvent (Ev);
    }
    AsciiSPrint (L, sizeof (L), "[MSD] USB controller init event signalled (0x%lx)\n", (UINT64)S);
    LogSec (L);
    for (Tries = 0; Tries < 20 && After == 0; Tries++) {
      EfiBootManagerConnectAll ();
      if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &FnGuid, NULL, &After, &H)) && (H != NULL)) {
        FreePool (H);
        H = NULL;
      }
      if (After == 0) {
        gBS->Stall (250000);
      }
    }
    AsciiSPrint (L, sizeof (L), "[MSD] connect all: UsbfnIo handles %u -> %u (%lu ms)\n", (UINT32)Before, (UINT32)After,
      ((ReadCntPct () - Tc) * 1000ULL) / ReadCntFrq ());
    LogSec (L);
    R5qPersistLog ("after connect all");
  }

  S = gBS->LocateProtocol (&MsdGuid, NULL, (VOID **)&Msd);
  AsciiSPrint (L, sizeof (L), "[MSD] UsbMsd 0x%lx rev 0x%x\n", (UINT64)S, (Msd != NULL) ? Msd->Revision : 0);
  LogSec (L);
  if (EFI_ERROR (S) || (Msd == NULL)) {
    FbPrintLine ("[M] USB MSD MISSING", 0xFFFF5050U);
    return;
  }
  S = gBS->AllocatePages (AllocateAddress, EfiLoaderData, EFI_SIZE_TO_PAGES (R5Q_RD_BYTES), &A);
  if (EFI_ERROR (S)) {
    AsciiSPrint (L, sizeof (L), "[MSD] RAM disk allocation failed 0x%lx\n", (UINT64)S);
    LogSec (L);
    return;
  }
  mRd = (UINT8 *)(UINTN)A;
  RdFormat ();
  ZeroMem (&mRdMedia, sizeof (mRdMedia));
  mRdMedia.MediaId          = 0x5235;
  mRdMedia.RemovableMedia   = TRUE;
  mRdMedia.MediaPresent     = TRUE;
  mRdMedia.LogicalPartition = FALSE;
  mRdMedia.ReadOnly         = FALSE;
  mRdMedia.WriteCaching     = FALSE;
  mRdMedia.BlockSize        = R5Q_RD_SECTOR;
  mRdMedia.IoAlign          = 0;
  mRdMedia.LastBlock        = R5Q_RD_BYTES / R5Q_RD_SECTOR - 1;
  ZeroMem (&mRdBio, sizeof (mRdBio));
  mRdBio.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mRdBio.Media       = &mRdMedia;
  mRdBio.Reset       = RdReset;
  mRdBio.ReadBlocks  = RdRead;
  mRdBio.WriteBlocks = RdWrite;
  mRdBio.FlushBlocks = RdFlush;
  AsciiSPrint (L, sizeof (L), "[MSD] RAM disk 3 GiB at 0x%lx, FAT32 with FAT size %u sectors\n", (UINT64)A, mRdFatSz);
  LogSec (L);

  S = Msd->QueryMaxLun (Msd, &MaxLun);
  AsciiSPrint (L, sizeof (L), "[MSD] max LUN count %u (0x%lx)\n", MaxLun, (UINT64)S);
  LogSec (L);
  S = Msd->AssignBlkIoHandle (Msd, &mRdBio, 0);
  AsciiSPrint (L, sizeof (L), "[MSD] assign RAM disk to LUN 0: 0x%lx\n", (UINT64)S);
  LogSec (L);
  if (EFI_ERROR (S)) {
    FbPrintLine ("[M] MSD ASSIGN FAILED", 0xFFFF5050U);
    return;
  }
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  S = Msd->StartDevice (Msd);
  AsciiSPrint (L, sizeof (L), "[MSD] StartDevice 0x%lx\n", (UINT64)S);
  LogSec (L);
  if (EFI_ERROR (S)) {
    FbPrintLine ("[M] MSD START FAILED", 0xFFFF5050U);
    Msd->AssignBlkIoHandle (Msd, NULL, 0);
    return;
  }
  FbPrintLine ("[M] USB DRIVE R5QRAM: WAITING FOR THE PC", 0xFF80C0FFU);
  R5qPersistLog ("msd waiting");

  F       = ReadCntFrq ();
  T0      = ReadCntPct ();
  LastLog = T0;
  while ((ReadCntPct () - T0) < (UINT64)WaitS * F) {
    Msd->EventHandler (Msd);
    if ((ReadCntPct () - LastLog) > F / 2) {
      LastLog = ReadCntPct ();
      if (RdFind ("DONE    TXT", &DoneFirst, &DoneSize) && DoneSize != 0) {
        Found = TRUE;
        break;
      }
    }
  }
  // keep serving briefly so the PC's last requests complete before the device goes away
  T0 = ReadCntPct ();
  while ((ReadCntPct () - T0) < 2 * F) {
    Msd->EventHandler (Msd);
  }
  S = Msd->StopDevice (Msd);
  Msd->AssignBlkIoHandle (Msd, NULL, 0);
  AsciiSPrint (L, sizeof (L), "[MSD] StopDevice 0x%lx; PC read %lu MB, wrote %lu MB; DONE.TXT %a\n", (UINT64)S,
    mRdReads >> 20, mRdWrites >> 20, Found ? "found" : "not found");
  LogSec (L);
  if (!Found) {
    FbPrintLine ("[M] NO DONE.TXT FROM THE PC", 0xFFFF8C00U);
    return;
  }
  Ok = RdCheckFiles (DoneFirst, DoneSize);
  AsciiSPrint (L, sizeof (L), "[MSD] session end: %u files verified\n", Ok);
  LogSec (L);
  // Test 66: the PC's clock (EPOCH.TXT, written just before DONE.TXT), for iOS's time
  {
    UINT32  EFirst, ESize;
    CHAR8   E[33];

    if (RdFind ("EPOCH   TXT", &EFirst, &ESize) && ESize != 0 && ESize < sizeof (E) &&
        RdGather (EFirst, ESize, (UINT8 *)E) != 0)
    {
      E[ESize]      = '\0';
      mR5qEpoch     = AsciiStrDecimalToUint64 (E);
      mR5qEpochCnt  = ReadCntPct ();
    }
    AsciiSPrint (L, sizeof (L), "[MSD] PC clock (EPOCH.TXT): %lu\n", mR5qEpoch);
    LogSec (L);
  }
}

//
// Test 66: iOS's clock. The payload's cmdline carries "epoch=<10 digits>" (the bundle time); overwrite the digits,
// in place and the same length, with the PC's clock plus the seconds since it was read.
//
STATIC VOID
R5qPatchEpoch (
  VOID
  )
{
  UINT8   *P = (UINT8 *)(UINTN)0xF0000000ULL;
  UINT32  Len, Off = 8, ItemLen, I, J;
  UINT64  Now;
  CHAR8   Digits[11];
  CHAR8   L[96];
  CHAR8   *C;

  if (mR5qEpoch == 0) {
    R5qEpochFromRtc ();      // Test 69: no PC (the card): the phone's own clock
  }
  if (mR5qEpoch < 1000000000ULL || mR5qEpoch > 9999999999ULL) {
    LogSec ("[TIME] no PC clock and no RTC: epoch boot-arg left as built\n");
    return;
  }
  Now = mR5qEpoch + (ReadCntPct () - mR5qEpochCnt) / ReadCntFrq ();
  if (*(UINT32 *)P != 0x50554e58) {
    LogSec ("[TIME] no XNUP payload at 0xF0000000\n");
    return;
  }
  Len = *(UINT32 *)(P + 4);
  while (Off + 24 <= Len + 8) {
    ItemLen = *(UINT32 *)(P + Off + 16);
    if (ItemLen == 0 && P[Off] == 0) {
      break;
    }
    if (AsciiStrnCmp ((CHAR8 *)P + Off, "cmdline", 8) == 0) {
      C = (CHAR8 *)P + Off + 24;
      for (I = 0; I + 16 <= ItemLen; I++) {
        if (CompareMem (C + I, "epoch=", 6) == 0) {
          for (J = 0; J < 10 && C[I + 6 + J] >= '0' && C[I + 6 + J] <= '9'; J++) {
          }
          if (J != 10) {
            break;
          }
          AsciiSPrint (Digits, sizeof (Digits), "%010lu", Now);
          CopyMem (C + I + 6, Digits, 10);
          WriteBackDataCacheRange (C + I, 16);
          AsciiSPrint (L, sizeof (L), "[TIME] epoch boot-arg set to %a (PC clock + %lu s)\n", Digits, Now - mR5qEpoch);
          LogSec (L);
          return;
        }
      }
      LogSec ("[TIME] cmdline has no 10-digit epoch=\n");
      return;
    }
    Off += 24 + ((ItemLen + 15) & ~15U);
  }
  LogSec ("[TIME] no cmdline item\n");
}

//
// Test 51: before the loader runs. The RAM disk goes back to the firmware, and so does the system image's
// allocation: the loader's window is the largest *free* block and it claims all of it, so the image must lie in
// free memory at the resident address. This DEBUG build does not fill freed pages (PcdDebugPropertyMask 0x27),
// and every allocation until the loader's claim comes from the top of RAM, above the 6 GiB window.
// Returns the resident image size for the log (0 if none).
//
STATIC UINT32
R5qUsbRamDiskRelease (
  VOID
  )
{
  CHAR8   L[128];
  UINT32  Crc = 0;

  R5qPatchEpoch ();
  if (mRd != NULL) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)mRd, EFI_SIZE_TO_PAGES (R5Q_RD_BYTES));
    mRd = NULL;
  }
  if (mR5qResidentPages == 0) {
    LogSec ("[MSD] no resident system image\n");
    return 0;
  }
  gBS->FreePages (mR5qResidentPa, mR5qResidentPages);
  gBS->CalculateCrc32 ((VOID *)(UINTN)mR5qResidentPa, mR5qResidentSize, &Crc);
  AsciiSPrint (L, sizeof (L), "[MSD] resident system image released for the loader: 0x%lx, %u bytes, crc32 still %08x\n",
    (UINT64)mR5qResidentPa, mR5qResidentSize, Crc);
  LogSec (L);
  return mR5qResidentSize;
}
#endif  // R5Q_HOST_TEST
