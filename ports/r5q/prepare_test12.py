"""Keep XNU RAM reserved, then lend it exclusively after the Android-return guard."""
import pathlib
import shutil
import sys
from validate_memory_window import validate_window

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
validate_window(root, 0xe0000000, 0x10000000)
port = root / 'port'
test = port / 'test12'
test.mkdir(exist_ok=True)
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
board = mu / 'Platforms/Samsung/r5qtestPkg'
data = (port / 'test11/FirstBootManager.c').read_text().replace('[TEST11]', '[TEST12]').replace('TEST 11', 'TEST 12')
data = data.replace('#include <PiDxe.h>', '#include <PiDxe.h>\n#include "R5qReservedWindow.h"', 1)
anchor = '  Status = RunTestLoader (Line, sizeof (Line));'
assert data.count(anchor) == 1
data = data.replace(anchor, '''  Status = ReserveXnuWindow ();
  AsciiSPrint (Line, sizeof (Line), "[TEST12] RESERVED WINDOW STATUS=0x%lx\\n", (UINT64)Status);
  LogSec (Line);
  if (EFI_ERROR (Status)) {
    LogSec ("[TEST12] APP SKIPPED: reserved RAM ownership failed\\n");
    MmioWrite32 (PSHOLD_ADDR, 0U);
    CpuDeadLoop ();
  }
''' + anchor, 1)
anchor = '  // Re-establish clean dashboard in case loader.efi cleared screen'
assert data.count(anchor) == 1
data = data.replace(anchor, '''  Status = ReleaseXnuWindow ();
  AsciiSPrint (Line, sizeof (Line), "[TEST12] RESERVED WINDOW RELEASE=0x%lx\\n", (UINT64)Status);
  LogSec (Line);

''' + anchor, 1)
(test / 'FirstBootManager.c').write_text(data)
inf = (port / 'test11/FirstBootManager.inf').read_text().replace('  BaseLib\n', '  BaseLib\n  DxeServicesTableLib\n', 1)
(test / 'FirstBootManager.inf').write_text(inf)
for name in ['FirstBootManager.c', 'FirstBootManager.inf']:
    shutil.copyfile(test / name, board / 'Library/FirstBootManager' / name)
shutil.copyfile(port / 'R5qReservedWindow.h', test / 'R5qReservedWindow.h')
shutil.copyfile(port / 'R5qReservedWindow.h', board / 'Library/FirstBootManager/R5qReservedWindow.h')
shutil.copyfile(port / 'test11/MemoryMapLib.c', test / 'MemoryMapLib.c')
shutil.copyfile(port / 'test11/MemoryMapLib.c', board / 'Library/MemoryMapLib/MemoryMapLib.c')
print('Prepared Test 12 reserved-window ownership handoff; XNU preparation only')
