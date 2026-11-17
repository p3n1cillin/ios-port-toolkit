"""Test 5: complete the normal DXE phase and verify the EFI app actually runs."""
import pathlib
import shutil
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
port = root / 'port'
test = port / 'test5'
test.mkdir(exist_ok=True)
for name in ['FirstBootManager.c', 'FirstBootManager.inf', 'BootShim.S', 'reboot-recovery.S']:
    shutil.copyfile(port / 'test4' / name, test / name)
source = test / 'FirstBootManager.c'
text = source.read_text().replace('[TEST3]', '[TEST5]').replace('TEST 3', 'TEST 5')
anchor = '  LogSec ("[TEST5] PlatformBootManagerBeforeConsole\\n");'
assert text.count(anchor) == 1
text = text.replace(anchor, '''  EFI_STATUS PhaseStatus;
  EFI_HANDLE LockHandle = NULL;
  VOID *LockInterface = NULL;

  // Standard firmware lifecycle: permit third-party EFI images only after DXE.
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  PhaseStatus = gBS->LocateProtocol (&gEfiDxeSmmReadyToLockProtocolGuid, NULL, &LockInterface);
  if (EFI_ERROR (PhaseStatus)) {
    PhaseStatus = gBS->InstallProtocolInterface (&LockHandle,
      &gEfiDxeSmmReadyToLockProtocolGuid, EFI_NATIVE_INTERFACE, NULL);
  }
  AsciiSPrint (Line, sizeof (Line), "[TEST5] DXE PHASE READY=0x%lx\\n", (UINT64)PhaseStatus);
  LogSec (Line);
  mBootPhaseReady = !EFI_ERROR (PhaseStatus);
''' + anchor)
text = text.replace('STATIC UINT32     mTextRow', 'STATIC BOOLEAN    mBootPhaseReady = FALSE;\nSTATIC UINT32     mTextRow', 1)
param = '''  Status = ClearSamsungParamFlag (Line, sizeof (Line));
  LogSec (Line);
  LogSec ("\\n");
  FbPrintLine (Line, EFI_ERROR (Status) ? 0xFFFF8C00U : 0xFF40FF60U);
'''
assert text.count(param) == 1
text = text.replace(param, '', 1)
anchor = '  Status = RunTestLoader (Line, sizeof (Line));'
assert text.count(anchor) == 1
text = text.replace(anchor, '''  // Clear the recovery-entry flag before executing code that might fault.
''' + param + '''
  if (!mBootPhaseReady || EFI_ERROR (Status)) {
    LogSec ("[TEST5] APP SKIPPED: phase/return guard failed\\n");
    MmioWrite32 (PSHOLD_ADDR, 0U);
    CpuDeadLoop ();
  }
''' + anchor)
anchor = '    if (OldVal != 1U) {'
assert text.count(anchor) == 1
text = text.replace(anchor, '''    if (OldVal != 1U && OldVal != 2U) {
      AsciiSPrint (OutLine, LineSize, "[6] PARAM UNEXPECTED VALUE 0x%X", OldVal);
      FreePool (Buf);
      FreePool (Handles);
      return EFI_COMPROMISED_DATA;
    }
''' + anchor)
source.write_text(text)
inf = test / 'FirstBootManager.inf'
text = inf.read_text() + '\n[Guids]\n  gEfiEndOfDxeEventGroupGuid\n'
text = text.replace('[Protocols]', '[Protocols]\n  gEfiDxeSmmReadyToLockProtocolGuid', 1)
inf.write_text(text)
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
destination = mu / 'Platforms/Samsung/r5qtestPkg/Library/FirstBootManager'
for name in ['FirstBootManager.c', 'FirstBootManager.inf']:
    shutil.copyfile(test / name, destination / name)
print('Prepared Test 5 EndOfDxe/ReadyToLock fix and pre-entry Android-return guard')
