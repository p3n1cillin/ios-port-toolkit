/** R5Q first native boot: memory-only payload handoff, then a cold reset.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Configuration/BootDevices.h>

#define PAYLOAD_BASE 0xF0000000ULL
#define PAYLOAD_MAX  0x04000000U
#define PAYLOAD_MAGIC 0x50554E58U

typedef struct { UINT32 Magic; UINT32 Size; } PAYLOAD_HEADER;
typedef struct { CHAR8 Name[16]; UINT32 Size; UINT32 Reserved; } PAYLOAD_ENTRY;

STATIC EFI_STATUS StartTestLoader (VOID)
{
  CONST PAYLOAD_HEADER *Header = (CONST PAYLOAD_HEADER *)(UINTN)PAYLOAD_BASE;
  CONST PAYLOAD_ENTRY  *Entry = (CONST PAYLOAD_ENTRY *)(Header + 1);
  CONST PAYLOAD_ENTRY  *End;
  EFI_HANDLE Handle;
  EFI_STATUS Status;
  UINTN Padded;
  STATIC CONST CHAR8 LoaderName[16] = "loader.efi";
  STATIC CONST UINT8 EmptyEntry[sizeof (PAYLOAD_ENTRY)] = {0};

  // This test accepts exactly one bounded loader entry and a zero terminator.
  if (Header->Magic != PAYLOAD_MAGIC || Header->Size < 48 ||
      Header->Size > PAYLOAD_MAX - sizeof (*Header)) {
    return EFI_COMPROMISED_DATA;
  }
  if (CompareMem (Entry->Name, LoaderName, sizeof (LoaderName)) != 0 ||
      Entry->Reserved != 0 || Entry->Size < 256 || Entry->Size > SIZE_1MB) {
    return EFI_COMPROMISED_DATA;
  }
  Padded = ALIGN_VALUE ((UINTN)Entry->Size, 16);
  if (Header->Size != 2 * sizeof (*Entry) + Padded) {
    return EFI_COMPROMISED_DATA;
  }
  End = (CONST PAYLOAD_ENTRY *)((CONST UINT8 *)(Entry + 1) + Padded);
  if (CompareMem (End, EmptyEntry, sizeof (*End)) != 0) {
    return EFI_COMPROMISED_DATA;
  }
  Print (L"TEST1 PAYLOAD OK (%u bytes)\n", Entry->Size);
  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, (VOID *)(Entry + 1), Entry->Size, &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Print (L"TEST1 EFI IMAGE LOADED\n");
  Status = gBS->StartImage (Handle, NULL, NULL);
  gBS->UnloadImage (Handle);
  return Status;
}

VOID EFIAPI PlatformBootManagerBeforeConsole (VOID)
{
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
}

VOID EFIAPI PlatformBootManagerAfterConsole (VOID)
{
  EFI_STATUS Status;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"R5Q NATIVE TEST 1\nTEST1 UEFI CONSOLE OK\n");
  Status = StartTestLoader ();
  Print (L"TEST1 LOADER RESULT: %r\n", Status);
  Print (L"Reboot requested in 20 seconds. No iOS kernel was started.\n");
  gBS->Stall (20000000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

VOID EFIAPI PlatformBootManagerOnDemandConInConnect (VOID) {}
VOID EFIAPI PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption) {}
VOID EFIAPI PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext) {}
VOID EFIAPI PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain) {}
VOID EFIAPI PlatformBootManagerUnableToBoot (VOID) { CpuDeadLoop (); }
VOID EFIAPI PlatformBootManagerBdsEntry (VOID) {}
