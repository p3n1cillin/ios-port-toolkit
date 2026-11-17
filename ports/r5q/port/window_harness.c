#include "R5qReservedWindow.h"
EFI_DXE_SERVICES *gDS;
EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImageHandle = (EFI_HANDLE)(UINTN)0x1234;
EFI_STATUS EFIAPI test_entry (EFI_DXE_SERVICES *Ds, EFI_BOOT_SERVICES *Bs, UINT64 Action)
{
  gDS = Ds;
  gBS = Bs;
  return Action == 0 ? ReserveXnuWindow () : ReleaseXnuWindow ();
}
