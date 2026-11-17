"""Isolate mapping a RAM window from making it a general DXE heap."""
import pathlib
import shutil
import sys
from validate_memory_window import validate_window

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
validate_window(root, 0xe0000000, 0x10000000)
port = root / 'port'
test = port / 'test11'
test.mkdir(exist_ok=True)
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
board = mu / 'Platforms/Samsung/r5qtestPkg'
for name in ['FirstBootManager.c', 'FirstBootManager.inf']:
    data = (port / 'test8' / name).read_text().replace('[TEST8]', '[TEST11]').replace('TEST 8', 'TEST 11')
    (test / name).write_text(data)
    shutil.copyfile(test / name, board / 'Library/FirstBootManager' / name)
memory = (port / 'test8/MemoryMapLib.c').read_text().replace(
    '{"XNU Test RAM", 0xE0000000ULL, 0x10000000ULL, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK}',
    '{"XNU Test RAM", 0xE0000000ULL, 0x10000000ULL, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}')
assert memory.count('"XNU Test RAM"') == 1 and 'MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}' in memory
(test / 'MemoryMapLib.c').write_text(memory)
(board / 'Library/MemoryMapLib/MemoryMapLib.c').write_text(memory)

sec = mu / 'Silicon/Silicium/SiliciumPkg/Sec/Sec.c'
data = sec.read_text()
if 'LabSecStage' not in data:
    anchor = '#include "Sec.h"\n'
    helper = r'''
// Lab-only stage telemetry in the existing Samsung RAM log; no storage writes.
STATIC VOID LabSecStage (IN CONST CHAR8 *Msg)
{
  volatile UINT32 *Hdr = (volatile UINT32 *)(UINTN)0xC1600000ULL;
  if (Hdr[1] != 0x4D474F4CU) return;
  UINT32 WIdx = Hdr[2];
  UINT32 Off = WIdx & 0x1FFFFFU;
  if (Off < 0x10U || Off >= 0x1FFF00U) Off = 0x10U;
  volatile CHAR8 *Buf = (volatile CHAR8 *)(UINTN)0xC1600000ULL;
  while (*Msg != '\0') {
    Buf[Off++] = *Msg++;
    if (Off >= 0x1FFF00U) Off = 0x10U;
  }
  Hdr[2] = (WIdx & ~0x1FFFFFU) | Off;
  ArmDataSynchronizationBarrier ();
}
'''
    assert data.count(anchor) == 1
    data = data.replace(anchor, anchor + helper, 1)
    for anchor, before, after in [
        ('  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);',
         '  LabSecStage ("[LABSEC] memory map begin\\n");\n',
         '\n  LabSecStage ("[LABSEC] memory map returned\\n");'),
        ('  Status = DecompressFirstFv ();',
         '  LabSecStage ("[LABSEC] decompress begin\\n");\n',
         '\n  LabSecStage ("[LABSEC] decompress returned\\n");'),
        ('  Status = LoadDxeCoreFromFv (NULL, 0);',
         '  LabSecStage ("[LABSEC] DXE enter\\n");\n', ''),
        ('  SecMain ();', '  LabSecStage ("[LABSEC] SEC enter\\n");\n', '')]:
        assert data.count(anchor) == 1
        data = data.replace(anchor, before + anchor + after, 1)
    sec.write_text(data)
shutil.copyfile(sec, test / 'Sec.c')
print('Prepared reserved RAM-window control with early SEC telemetry')
