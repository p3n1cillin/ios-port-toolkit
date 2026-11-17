"""Create a separate, source-only first-boot firmware package for R5Q."""
import pathlib
import re
import shutil
import sys

mu = pathlib.Path(sys.argv[1]).resolve(strict=True)
port = pathlib.Path(sys.argv[2]).resolve(strict=True)
base = mu / 'Platforms/Samsung/r5qPkg'
board = mu / 'Platforms/Samsung/r5qtestPkg'
board.mkdir(exist_ok=True)

for name in ['MemoryMapLib', 'ConfigurationMapLib']:
    for source in (base / 'Library' / name).iterdir():
        target = board / 'Library' / name / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
memory = board / 'Library/MemoryMapLib/MemoryMapLib.c'
text = memory.read_text()
text = text.replace('\n};', '\n  {"XNU Payload", 0xF0000000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},\n};', 1)
memory.write_text(text)
library = board / 'Library/FirstBootManager'
library.mkdir(parents=True, exist_ok=True)
for name in ['FirstBootManager.c', 'FirstBootManager.inf']:
    shutil.copyfile(port / 'first-test' / name, library / name)

dsc = (base / 'r5q.dsc').read_text().replace('r5qPkg', 'r5qtestPkg').replace('r5q.fdf', 'r5qtest.fdf')
dsc = dsc.replace('PLATFORM_NAME                  = r5q', 'PLATFORM_NAME                  = r5qtest')
dsc = re.sub(r'PLATFORM_GUID\s*=\s*[^\n]+', 'PLATFORM_GUID = CE3DA256-4C80-410F-9D0E-9D1C68E8424A', dsc)
dsc += '''
[LibraryClasses]
  PlatformBootManagerLib|r5qtestPkg/Library/FirstBootManager/FirstBootManager.inf
  DeviceBootManagerLib|SiliciumNullPkg/Library/PlatformBootManagerLibNull/PlatformBootManagerLibNull.inf
'''
(board / 'r5qtest.dsc').write_text(dsc)

modules = [
 'MdeModulePkg/Core/Dxe/DxeMain.inf',
 'MdeModulePkg/Universal/PCD/Dxe/Pcd.inf',
 'ArmPkg/Drivers/CpuDxe/CpuDxe.inf',
 'ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf',
 'ArmPkg/Drivers/TimerDxe/TimerDxe.inf',
 'MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf',
 'MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf',
 'MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf',
 'MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf',
 'MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf',
 'MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf',
 'MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf',
 'EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf',
 'EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf',
 'EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf',
 'MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf',
 'MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf',
 'MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf',
 'MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf',
 'MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf',
 'SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf',
 'MdeModulePkg/Universal/BdsDxe/BdsDxe.inf',
]
fdf = (base / 'r5q.fdf').read_text()
start = fdf.index('  !include Include/APRIORI.inc')
end = fdf.index('[FV.FVMAIN_COMPACT]')
fdf = fdf[:start] + '  APRIORI DXE {\n' + ''.join(f'    INF {x}\n' for x in modules[:6]) + '  }\n' + ''.join(f'  INF {x}\n' for x in modules) + '\n' + fdf[end:]
(board / 'r5qtest.fdf').write_text(fdf)
(board / 'test-modules.txt').write_text('\n'.join(modules) + '\n')
assert 'Binaries/' not in fdf and 'QcomPkg/Extra' not in fdf
print(f'Prepared {board}: {len(modules)} source-built DXE modules; no vendor storage, USB, charger or TZ drivers')
