// Lab-only reserved RAM loan. The manager retains ownership until app return.
// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef R5Q_RESERVED_WINDOW_H
#define R5Q_RESERVED_WINDOW_H
#include <PiDxe.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define R5Q_WINDOW_BASE 0xE0000000ULL
#define R5Q_WINDOW_SIZE 0x10000000ULL
#define R5Q_WINDOW_MAGIC 0x31304E4957513552ULL // "R5QWIN01"
STATIC EFI_GUID mR5qWindowGuid = {
  0x487b9945, 0x3e22, 0x4ddd, {0x91,0xb7,0x89,0x24,0x02,0xd6,0xcf,0x99}
};
typedef struct {
  UINT64 Magic;
  UINT32 Version;
  UINT32 Size;
  UINT64 Base;
  UINT64 Length;
  UINT64 Attributes;
  UINT64 Flags;
} R5Q_RESERVED_WINDOW;
STATIC R5Q_RESERVED_WINDOW mR5qWindow = {
  R5Q_WINDOW_MAGIC, 1, sizeof(R5Q_RESERVED_WINDOW),
  R5Q_WINDOW_BASE, R5Q_WINDOW_SIZE, EFI_MEMORY_WB, 0
};
STATIC EFI_HANDLE mR5qWindowHandle = NULL;

STATIC EFI_STATUS ReserveXnuWindow (VOID)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_PHYSICAL_ADDRESS Claim = R5Q_WINDOW_BASE;
  EFI_STATUS Status;
  if (mR5qWindowHandle != NULL) return EFI_ALREADY_STARTED;
  Status = gDS->GetMemorySpaceDescriptor (R5Q_WINDOW_BASE, &Desc);
  if (EFI_ERROR (Status)) return Status;
  // Reject fragmented, owned, wrong-type or non-WB resources without changing them.
  if (Desc.BaseAddress > R5Q_WINDOW_BASE ||
      Desc.Length < R5Q_WINDOW_BASE + R5Q_WINDOW_SIZE - Desc.BaseAddress ||
      Desc.GcdMemoryType != EfiGcdMemoryTypeReserved ||
      Desc.ImageHandle != NULL || Desc.DeviceHandle != NULL ||
      (Desc.Capabilities & EFI_MEMORY_WB) == 0) return EFI_ACCESS_DENIED;
  Status = gDS->AllocateMemorySpace (EfiGcdAllocateAddress, EfiGcdMemoryTypeReserved,
    EFI_PAGE_SHIFT, R5Q_WINDOW_SIZE, &Claim, gImageHandle, NULL);
  if (EFI_ERROR (Status)) return Status;
  if (Claim != R5Q_WINDOW_BASE) return EFI_COMPROMISED_DATA;
  mR5qWindow.Flags = 1;
  Status = gBS->InstallProtocolInterface (&mR5qWindowHandle, &mR5qWindowGuid,
    EFI_NATIVE_INTERFACE, &mR5qWindow);
  if (EFI_ERROR (Status)) {
    mR5qWindow.Flags = 0;
    mR5qWindowHandle = NULL;
    gDS->FreeMemorySpace (R5Q_WINDOW_BASE, R5Q_WINDOW_SIZE);
  }
  return Status;
}

STATIC EFI_STATUS ReleaseXnuWindow (VOID)
{
  EFI_STATUS Status;
  if (mR5qWindowHandle == NULL) return EFI_NOT_STARTED;
  Status = gBS->UninstallProtocolInterface (mR5qWindowHandle,
    &mR5qWindowGuid, &mR5qWindow);
  if (EFI_ERROR (Status)) return Status;
  mR5qWindowHandle = NULL;
  mR5qWindow.Flags = 0;
  return gDS->FreeMemorySpace (R5Q_WINDOW_BASE, R5Q_WINDOW_SIZE);
}
#endif
