# One authorized run of a return-path stage test (19-23): automatic capture/restore, then a 12 h
# restore watcher only if Android did not return in time. Never retries or starts another test.
# Needs the user's explicit OK and the bundle's start_blocked_reason lifted for that one run.
# Run 19 first, then 24 (watchdog probe), then 20, 21, 22, 23; stop at the first that does not return.
param([Parameter(Mandatory)][ValidateRange(19, 69)][int]$Number)
$ErrorActionPreference = 'Continue'
if ($env:PLATFORM_TOOLS) { $env:Path = "$env:PLATFORM_TOOLS;$env:Path" }   # fastboot/adb: on PATH, or in PLATFORM_TOOLS
$py = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\python.exe'
$work = $PSScriptRoot
$outputs = if ($env:R5Q_OUTPUTS) { $env:R5Q_OUTPUTS } else { Join-Path $PSScriptRoot 'outputs' }   # test bundles
$bundle = Join-Path $outputs "test$Number"
$markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
             "TEST$Number FIRMWARE RETURN PATH ONLY", '[LAB] before MMU-off handoff')
switch ($Number) {
    19 { $markers += '[LAB] STAGE 1: PS_HOLD reset before MMU-off' }
    20 { $markers += 'RB0' }
    21 { $markers += 'RB0', 'RB1', 'RB2' }
    22 { $markers += 'RB0', 'RB1', 'RB2', 'RB3' }
    23 { $markers += 'RB0', 'RB1', 'RB2', 'RB3', 'RB4', '[LAB] context restored',
                     'TEST23 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD' }
    24 { $markers += '[LAB] WDT PROBE: arming bark 5 s, bite 8 s; not petting',
                     '[LAB] WDT PROBE: armed, waiting for the bite' }   # a bite leaves no line after this; the fallback adds 'no bite after 20 s'
    25 { $markers += '[LAB] WDT ARMED: bite 8 s guard over the hand-off', 'RB0' }
    26 { $markers += '[LAB] WDT ARMED: bite 8 s guard over the hand-off', 'RB0', 'RB1', 'RB2' }
    27 { $markers += '[LAB] WDT ARMED: bite 8 s guard over the hand-off', 'RB0', 'RB1', 'RB2', 'RB3' }
    28 { $markers += '[LAB] WDT ARMED: bite 8 s guard over the hand-off', 'RB0', 'RB1', 'RB2', 'RB3', 'RB4',
                     '[LAB] context restored', 'TEST28 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD' }
    29 { $markers += '[LAB] ctx+stack flushed to PoC (explicit)', 'RB0', '[LAB] context restored',
                     'TEST29 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD' }
    30 { $markers += 'RB0', '[LAB] context restored', 'TEST30 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD' }
    31 { $markers += 'RB0', '[LAB] context restored', 'TEST31 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD' }
    32 {   # real XNU to checkpoint 1: the kernel returns through its own slot (no RB0 stub tag)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST32 REAL XNU EARLY CHECKPOINT 1', '[LAB] firmware map validated; per-region clean skipped',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', 'STOPPED AT CHECKPOINT 1',
                     'TEST32 REAL XNU CHECKPOINT 1 VERIFIED', '[TEST12] Resetting via PSHOLD') }
    33 {   # real XNU to checkpoint 5 (just after the kernel enables its own MMU)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST33 REAL XNU EARLY CHECKPOINT 5', '[LAB] firmware map validated; per-region clean skipped',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', 'STOPPED AT CHECKPOINT 5',
                     'TEST33 REAL XNU CHECKPOINT 5 VERIFIED', '[TEST12] Resetting via PSHOLD') }
    34 {   # real XNU, no checkpoint: boots for 25 s, returns by its own timer with its log
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST34 REAL XNU TIMED RUN 25 S', '[LAB] firmware map validated; per-region clean skipped',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', '== KERNEL LOG ==',
                     '== END OF LOG ==', 'TEST34 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD') }
    35 {   # Test 34 with -v: kernel text on the phone's screen; 60 s timed return
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST35 REAL XNU TIMED RUN 60 S', '[LAB] firmware map validated; per-region clean skipped',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', '== KERNEL LOG ==',
                     '== END OF LOG ==', 'TEST35 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD') }
    36 {   # Test 35 + timer fix (kernel r5q overlay, gic_vtimer=19): the 60 s timer must fire on the phone
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST36 REAL XNU TIMED RUN 60 S', 'gic_vtimer=19', '[LAB] firmware map validated; per-region clean skipped',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', '== KERNEL LOG ==',
                     'loader_return_after expired', 'virtual timer on PPI 19', '== END OF LOG ==',
                     'TEST36 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD') }
    37 {   # full r5q firmware (every Qualcomm DXE) + read-only probe loader
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST37 PROBE usb device protocol', 'TEST37 PROBE block io handles',
                     'TEST37 PROBE conventional total', 'TEST37 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    38 {   # Test 37 with RamManagerDxe in APRIORI
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST38 PROBE usb device protocol', 'TEST38 PROBE block io handles',
                     'TEST38 PROBE conventional total', 'TEST38 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    39 {   # full firmware with a second 60 MiB DXE heap
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST39 PROBE usb device protocol', 'TEST39 PROBE block io handles',
                     'TEST39 PROBE conventional total', 'TEST39 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    40 {   # DEBUG full firmware: the RAM ring holds the DXE trail whatever happens
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST40 PROBE usb device protocol', 'TEST40 PROBE block io handles',
                     'TEST40 PROBE conventional total', 'TEST40 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    41 {   # DEBUG full firmware with the secure no-map regions reserved
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST41 PROBE usb device protocol', 'TEST41 PROBE block io handles',
                     'TEST41 PROBE conventional total', 'TEST41 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    42 {   # Test 41 with the readable debug writer
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST42 PROBE usb device protocol', 'TEST42 PROBE block io handles',
                     'TEST42 PROBE conventional total', 'TEST42 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    43 {   # Test 42 + probe results on the dashboard (the RAM log is wiped by this firmware's cold reset)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST43 PROBE usb device protocol', 'TEST43 PROBE block io handles',
                     'TEST43 PROBE conventional total', 'TEST43 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD') }
    44 {   # Test 43 + the firmware saves the RAM log into the slot in recovery (read back before Restore)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST44 PROBE usb device protocol', 'TEST44 PROBE block io handles',
                     'TEST44 PROBE conventional total', 'TEST44 FULL FIRMWARE PROBE DONE',
                     '[P] USB DEVICE PROTOCOL', '[P] BLOCK IO', '[P] FREE RAM', '[P] LARGEST AT',
                     '[SLOT] saving the RAM log to recovery (after probe', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    45 {   # Test 44 + fastboot on Qualcomm UsbfnIo in the boot manager; usb_host_session.py runs on the PC meanwhile
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST45 PROBE usb device protocol', 'TEST45 FULL FIRMWARE PROBE DONE',
                     '[SLOT] saving the RAM log to recovery (after probe', '[USB] UsbfnIo',
                     '[USB] session end', '[SLOT] saving the RAM log to recovery (after usb',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    46 {   # Test 45 with a USB RAM drive (Qualcomm UsbMsd + FAT32 RAM disk); usb_msd_session.py copies files onto it
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST46 PROBE usb device protocol', 'TEST46 FULL FIRMWARE PROBE DONE',
                     '[SLOT] saving the RAM log to recovery (after probe', '[MSD] UsbMsd 0x0',
                     '[MSD] StartDevice 0x0', '[MSD] StopDevice', '[MSD] TEST.BIN', ': MATCH', '[MSD] session end: 1 files verified',
                     '[SLOT] saving the RAM log to recovery (after usb', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    47 {   # Test 46 + connect all controllers first (UsbfnDwc3Dxe then installs UsbfnIo for UsbMsd)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST47 PROBE usb device protocol', 'TEST47 FULL FIRMWARE PROBE DONE',
                     '[SLOT] saving the RAM log to recovery (after probe', '[MSD] connect all: UsbfnIo handles',
                     '[MSD] UsbMsd 0x0', '[MSD] StartDevice 0x0', '[MSD] StopDevice', '[MSD] TEST.BIN', ': MATCH',
                     '[MSD] session end: 1 files verified', '[SLOT] saving the RAM log to recovery (after usb',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    48 {   # Test 47 + signal gUsbControllerInitGuid first (UsbConfigDxe starts the USB cores)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST48 PROBE usb device protocol', 'TEST48 FULL FIRMWARE PROBE DONE',
                     '[SLOT] saving the RAM log to recovery (after probe', '[MSD] USB controller init event signalled',
                     '[MSD] connect all: UsbfnIo handles', '[MSD] UsbMsd 0x0', '[MSD] StartDevice 0x0', '[MSD] StopDevice',
                     '[MSD] TEST.BIN', ': MATCH', '[MSD] session end: 1 files verified',
                     '[SLOT] saving the RAM log to recovery (after usb', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    69 {   # Test 68 + REST=1 root + wifi survey + firmware fulldbg23 (system image from a microSD card when present, RTC epoch)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[FS] file systems the firmware can read', ': MATCH', '[TIME] epoch boot-arg set',
                     'TEST69 REAL XNU USERLAND RUN 900 S', 'cores for the kernel', 'resident image at pa fc000000 size 948c0000',
                     '== KERNEL LOG ==', '$ exec /sbin/launchd.ios', '== END OF LOG ==',
                     'TEST69 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    68 {   # the big one: flip fixed (HW_CTL clock, third buffer), fbscale=2, battery gauge, buttons, 900 s run
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[MSD] StartDevice 0x0', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[TIME] epoch boot-arg set',
                     'TEST68 REAL XNU USERLAND RUN 900 S', 'cores for the kernel', 'resident image at pa fc000000 size 948c0000',
                     '== KERNEL LOG ==', '$ exec /sbin/launchd.ios', '== END OF LOG ==',
                     'TEST68 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    67 {   # Test 66 + framebuffer flip on r5q (two/three buffers, pipe scaler) + fbscale=2
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[MSD] StartDevice 0x0', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[TIME] epoch boot-arg set',
                     'TEST67 REAL XNU USERLAND RUN 240 S', 'cores for the kernel', 'resident image at pa fc000000 size 948c0000',
                     '== KERNEL LOG ==', 'touch: step 1:', '$ exec /sbin/launchd.ios', '== END OF LOG ==',
                     'TEST67 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    66 {   # Test 65 + cpufreq kernel (OSM, cpufreq=1) + epoch from the PC (EPOCH.TXT, firmware fulldbg22)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[MSD] StartDevice 0x0', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[TIME] epoch boot-arg set',
                     'TEST66 REAL XNU USERLAND RUN 240 S', 'cores for the kernel', 'resident image at pa fc000000 size 948c0000',
                     '== KERNEL LOG ==', 'touch: step 1:', '$ exec /sbin/launchd.ios', '== END OF LOG ==',
                     'TEST66 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    65 {   # Test 64 with all 8 cores (R5Q_LAB_SMP, cpus=8)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[MSD] StartDevice 0x0', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH',
                     'TEST65 REAL XNU USERLAND RUN 240 S', 'cores for the kernel', 'resident image at pa fc000000 size 948c0000',
                     '== KERNEL LOG ==', 'touch: step 1:', '$ exec /sbin/launchd.ios', '== END OF LOG ==',
                     'TEST65 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    64 {   # Test 63 + the touch kernel (r5q redwood_sec_ts port), touchscreen=1; unbuffered PC copy
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[PANEL] command page released', '[MSD] StartDevice 0x0', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH',
                     'TEST64 REAL XNU USERLAND RUN 240 S', 'resident image at pa fc000000 size 948c0000', '== KERNEL LOG ==',
                     'touch: step 1:', '$ exec /sbin/launchd.ios', '== END OF LOG ==', 'TEST64 REAL XNU TIMED RUN RETURNED',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    63 {   # Test 62 with the panel command page released before the USB stage
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[PANEL] command page released', '[MSD] connect all: UsbfnIo handles', '[MSD] StartDevice 0x0',
                     '[MSD] StopDevice', '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH',
                     '[MSD] resident system image released for the loader', 'TEST63 REAL XNU USERLAND RUN 240 S',
                     'resident image at pa fc000000 size 948c0000', '[LAB] before MMU-off handoff', '== KERNEL LOG ==',
                     '$ exec /sbin/launchd.ios', '== END OF LOG ==', 'TEST63 REAL XNU TIMED RUN RETURNED',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    62 {   # the iOS run with the display fixed (Test 61)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[PANEL] 2B rows 0-2399: sent',
                     '[MSD] connect all: UsbfnIo handles', '[MSD] StartDevice 0x0', '[MSD] StopDevice',
                     '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[MSD] resident system image released for the loader',
                     'TEST62 REAL XNU USERLAND RUN 240 S', 'resident image at pa fc000000 size 948c0000',
                     '[LAB] before MMU-off handoff', '== KERNEL LOG ==', '$ exec /sbin/launchd.ios',
                     '== END OF LOG ==', 'TEST62 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    61 {   # Test 60 with the DSI 6G offsets fixed (RESET 0x118), DMA done-mask on, the bootloader's command buffer
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[TE] gpio8 cfg', '[SYNC] ctl',
                     '[SYNC] dsi0 reset:', '[PANEL] command buffer', '[PANEL] 13 normal display mode', '[SYNC] clean frame',
                     '[SYNC] ruler + magenta', 'TEST61 REAL XNU USERLAND RUN 90 S', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST61 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    60 {   # Test 59 + the panel write window resent by DSI command DMA (13, 2A, 2B)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[TE] gpio8 cfg', '[SYNC] ctl',
                     '[SYNC] dsi0 reset:', '[PANEL] dma_ctrl', '[PANEL] 13 normal display mode', '[SYNC] clean frame',
                     '[SYNC] ruler + magenta', 'TEST60 REAL XNU USERLAND RUN 90 S', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST60 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    59 {   # row ruler in the framebuffer + magenta after it (fetch translated past our buffer, or rows dropped?)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[TE] gpio8 cfg', '[SYNC] ctl',
                     '[SYNC] dsi0 reset:', '[SYNC] ruler + magenta', 'TEST59 REAL XNU USERLAND RUN 90 S', '[LAB] context restored',
                     '== KERNEL LOG ==', '== END OF LOG ==', 'TEST59 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    58 {   # Test 57 + the panel TE pin (gpio8) checked/set to mdp_vsync
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[TE] gpio8 cfg', '[SYNC] ctl',
                     '[SYNC] dsi0 reset:', '[SYNC] phase 1:', '[SYNC] phase 2:', '[SYNC] phase 3:', '[SYNC] phases done',
                     'TEST58 REAL XNU USERLAND RUN 90 S', '[LAB] context restored', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST58 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    57 {   # Test 56 + the pattern in three refresh modes (autorefresh off + one frame / every 2nd TE / every TE)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[SYNC] ctl', '[SYNC] dsi0 reset:',
                     '[SYNC] phase 1:', '[SYNC] phase 2:', '[SYNC] phase 3:', '[SYNC] phases done',
                     'TEST57 REAL XNU USERLAND RUN 90 S', '[LAB] context restored', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST57 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    56 {   # Test 55 + DSI soft reset in the resync
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[SYNC] ctl', '[SYNC] autorefresh off',
                     '[SYNC] dsi0 reset:', '[SYNC] clean frame', '[SYNC] calibration pattern on screen',
                     'TEST56 REAL XNU USERLAND RUN 90 S', '[LAB] context restored', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST56 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    55 {   # display resync (no kicks during autorefresh) + 15 s calibration pattern; Test 50's kernel run, no USB
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[SYNC] ctl', '[SYNC] autorefresh off',
                     '[SYNC] clean frame', '[SYNC] calibration pattern on screen', 'TEST55 REAL XNU USERLAND RUN 90 S',
                     '[LAB] context restored', '== KERNEL LOG ==', '== END OF LOG ==', 'TEST55 REAL XNU TIMED RUN RETURNED',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
    54 {   # Test 51 + the firmware points the panel's pipe at 0x9C400000 (R5qFixScanout)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[SCAN] BootShim info',
                     '[SCAN] ctl', '[MSD] connect all: UsbfnIo handles', '[MSD] StartDevice 0x0', '[MSD] StopDevice',
                     '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[MSD] resident system image released for the loader',
                     'TEST54 REAL XNU USERLAND RUN 240 S', 'resident image at pa fc000000 size 948c0000',
                     '[LAB] before MMU-off handoff', '[LAB] context restored', '== KERNEL LOG ==', '$ exec /sbin/launchd.ios',
                     '== END OF LOG ==', 'TEST54 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    52 {   # display diagnosis: read-only MDSS dump under the FULL firmware
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', '[MDSS] mdp_top',
                     '[MDSS] kicked', 'TEST52 MDSS DUMP DONE', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    53 {   # display diagnosis: the same dump under the MINIMAL firmware (warm reset keeps last_kmsg)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC', 'TEST53 MDSS DUMP DONE',
                     '[TEST12] Resetting via PSHOLD') }
    51 {   # the iOS system volume over the USB RAM drive, resident at 0xFC000000 (md0), root.apfs md1, 240 s
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     '[MSD] connect all: UsbfnIo handles', '[MSD] StartDevice 0x0', '[MSD] StopDevice',
                     '[MSD] SYSTEM.APF: 2492203008 bytes', ': MATCH', '[MSD] resident system image released for the loader',
                     'TEST51 REAL XNU USERLAND RUN 240 S', 'phys window f4000000',
                     'resident image at pa fc000000 size 948c0000', '[LAB] gic: core', '[LAB] before MMU-off handoff',
                     '[LAB] context restored', '[LAB] gic: the kernel left', '== KERNEL LOG ==', '$ exec /sbin/launchd.ios',
                     '== END OF LOG ==', 'TEST51 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    50 {   # Test 49 + R5Q_GIC_RESTORE (Test 49: endless 'Spurious GIC interrupt 0x13' after the kernel came back)
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST50 REAL XNU USERLAND RUN 90 S', 'phys window f4000000',
                     '[LAB] firmware map validated; per-region clean skipped', '[LAB] gic: core', '[LAB] before MMU-off handoff',
                     '[LAB] context restored', '[LAB] gic: the kernel left', '== KERNEL LOG ==', '== END OF LOG ==',
                     'TEST50 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD',
                     '[SLOT] saving the RAM log to recovery (final') }
    49 {   # first iOS-root run on the full firmware: lab loader + R5Q_LAB_USERLAND, root.apfs as md0, 90 s timed return
        $markers = @('[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC',
                     'TEST49 REAL XNU USERLAND RUN 90 S', 'phys window f4000000',
                     '[LAB] firmware map validated; per-region clean skipped', '[LAB] before MMU-off handoff',
                     '[LAB] context restored', '== KERNEL LOG ==', '== END OF LOG ==', 'TEST49 REAL XNU TIMED RUN RETURNED',
                     '[TEST12] Resetting via PSHOLD', '[SLOT] saving the RAM log to recovery (final') }
}
$expect = @(); foreach ($m in $markers) { $expect += '--expect'; $expect += $m }
$slot = @(); if ($Number -ge 44) { $slot = @('--log-slot', '0x4000000') }
$hostSession = $null
$timeout = 300
$iosRuns = @(51, 54, 62, 63, 64, 65, 66, 67, 68, 69)   # runs that take the iOS system volume over the USB RAM drive
if (($Number -ge 45 -and $Number -le 48) -or $iosRuns -contains $Number) {   # the PC side of the phone's USB session, started before the phone leaves Android
    $usbOut = Join-Path $bundle ('validation\usb-host-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $hostScript = if ($Number -ge 46) { "$work\usb_msd_session.py" } else { "$work\usb_host_session.py" }
    $hostArgs = @('-I', $hostScript, $usbOut, '--wait', '260')
    if ($Number -ge 51) {   # the iOS system volume instead of a random test file
        $hostArgs = @('-I', $hostScript, $usbOut, '--wait', '600', '--test-mib', '0',
                      '--file', "$work\port\build\test51-system.apfs=SYSTEM.APF")   # iOS 16.5 system volume (2492203008 bytes)
        $timeout = 1200
        if ($Number -ge 68) { $timeout = 1800 }   # Test 68+: a 15-minute iOS run (five fingers held 1.5 s end it sooner)
    }
    $hostSession = Start-Process -FilePath $py -ArgumentList $hostArgs `
        -NoNewWindow -PassThru -RedirectStandardOutput "$usbOut.stdout.txt" -RedirectStandardError "$usbOut.stderr.txt"
}
# the third marker is this test's own (TEST<n> ...): shared ones may already be in the previous boot's log
& $py -I "$work\run_native_test.py" $bundle --operator "Manage-Test$Number.ps1" --timeout $timeout --anchor $markers[2] @slot @expect
$code = $LASTEXITCODE
"controller exit: $code"
if ($hostSession) {
    if (-not $hostSession.WaitForExit(30000)) { $hostSession.Kill(); 'usb host session stopped' }
    "usb host session exit: $($hostSession.ExitCode)"
    Get-Content "$usbOut.stdout.txt" -ErrorAction SilentlyContinue
}
if ($code -eq 2) {
    'Android did not return in time: starting the one-shot restore watcher (up to 12 h).'
    & $py -I "$work\restore_native_on_return.py" $bundle --operator "Manage-Test$Number.ps1" --max-wait 43200
    "watcher exit: $LASTEXITCODE"
}
