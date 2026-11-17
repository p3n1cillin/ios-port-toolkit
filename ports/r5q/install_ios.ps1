# The install: iOS replaces Android on the Galaxy S10 Lite (<SERIAL>). Approved by the owner 2026-11-16 ("im fine with
# that install plan"): BOOT is replaced with the iOS start image (port/build/r5q-install-boot.img) and USERDATA is erased
# (the firmware writes the iOS system image at its start). Recovery stays stock; nothing else is written.
# Back to Android: Download Mode + the stock G770FXXS9HXA1 firmware in work/s10-relock (or the boot backup + a wipe).
#
# Steps: the same preflight as the test bundles; the image pushed and hash-checked on the phone; the PC's USB session
# started with --install; BOOT written with dd and hash-checked; reboot. The phone's firmware then finds no iOS in
# userdata, takes it over USB, writes and verifies it, and starts iOS. From then on it boots iOS from the phone.
[CmdletBinding()]
param([ValidateSet('Check','Install')][string]$Action = 'Check')
$ErrorActionPreference = 'Stop'
$serial = '<SERIAL>'
$work = $PSScriptRoot
$remote = '/data/local/tmp/r5q-install'
$py = (Get-Command python -ErrorAction Stop).Source
$adbCommand = (Get-Command adb -ErrorAction Stop).Source
$image = Join-Path $work 'port\build\r5q-install-boot.img'
$report = Get-Content -LiteralPath (Join-Path $work 'port\build\r5q-install-boot-report.json') -Raw | ConvertFrom-Json
$originalBoot = '381e6aa72b0fee42a04c4f7be51d0c63bccaaed26e54f9ac30d3de6255e1bc1a'
$originalRecovery = '332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd'

function Invoke-Adb([string[]]$Arguments) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $result = & $adbCommand -s $serial @Arguments 2>&1
    $code = $LASTEXITCODE; $ErrorActionPreference = $prev
    if ($code -ne 0) { throw "ADB failed: $($result -join [Environment]::NewLine)" }
    return ($result -join "`n").Trim()
}
function Root([string]$Command) {
    if ($Command.Contains("'")) { throw 'Unexpected quote in root command' }
    return Invoke-Adb @('shell', "su -c '$Command'")
}
function Hash-Phone([string]$Path) {
    $result = Root "sha256sum $Path"
    if ($result -notmatch '^([0-9a-f]{64})\s') { throw "Unexpected phone checksum response for $Path" }
    return $Matches[1]
}

if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -ne $report.sha256) { throw 'Local image checksum mismatch' }
if ((Invoke-Adb @('get-state')) -ne 'device') { throw 'Android ADB is required' }
if ((Invoke-Adb @('shell','getprop','ro.product.model')) -ne 'SM-G770F') { throw 'Wrong phone model' }
if ((Root 'id') -notmatch 'uid=0\(root\)') { throw 'Magisk root is required' }
if ((Invoke-Adb @('shell','getprop','ro.boot.flash.locked')) -ne '0') { throw 'Bootloader is locked' }
$bootDev = Root 'readlink -f /dev/block/by-name/boot'
$dataDev = Root 'readlink -f /dev/block/by-name/userdata'
if ([int64](Root 'blockdev --getsize64 /dev/block/by-name/boot') -ne $report.bytes) { throw 'BOOT partition size differs from the image' }
if ((Hash-Phone '/dev/block/by-name/boot') -ne $originalBoot) { throw 'BOOT is not the original Android boot: review before installing' }
if ((Hash-Phone '/dev/block/by-name/recovery') -ne $originalRecovery) { throw 'RECOVERY is not the original: restore it first' }
$battery = Invoke-Adb @('shell','dumpsys','battery')
if ($battery -notmatch '(?m)^\s*level:\s*(\d+)\s*$' -or [int]$Matches[1] -lt 50) { throw 'Battery must be at least 50% (charging under iOS is not verified)' }
$pending = (Root 'dd if=/dev/block/by-name/misc bs=32 count=1 2>/dev/null | od -An -tx1 -v') -replace '\s', ''
if ($pending -notmatch '^0{64}$') { throw 'A bootloader command is pending' }
Write-Host "Preflight passed: boot $bootDev (original), userdata $dataDev, recovery original, battery $($Matches[1])%."
if ($Action -eq 'Check') { return }

# the image onto the phone and checked there
Write-Host (Root "set -e; mkdir -p $remote; chown 2000:2000 $remote; chmod 700 $remote")
Write-Host (Invoke-Adb @('push', $image, "$remote/boot.img"))
if ((Hash-Phone "$remote/boot.img") -ne $report.sha256) { throw 'Pushed image checksum mismatch' }

# the PC's side of the one USB session that delivers iOS and asks for the install
$usbOut = Join-Path $work ('port\build\install-usb-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$hostArgs = @('-I', "$work\usb_msd_session.py", $usbOut, '--wait', '900', '--test-mib', '0',
              '--file', "$work\port\build\test51-system.apfs=SYSTEM.APF", '--install')
$hostSession = Start-Process -FilePath $py -ArgumentList $hostArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput "$usbOut.stdout.txt" -RedirectStandardError "$usbOut.stderr.txt"

# BOOT, written and checked
Write-Host (Root "set -e; dd if=$remote/boot.img of=/dev/block/by-name/boot bs=4096; sync")
if ((Hash-Phone '/dev/block/by-name/boot') -ne $report.sha256) {
    Write-Host (Root "set -e; dd if=/dev/zero of=/dev/null count=0")
    $hostSession.Kill()
    throw 'Written BOOT checksum failed: restore the original boot (backups/<SERIAL>/boot.img) before rebooting'
}
Write-Host 'BOOT now holds the iOS start image. Rebooting into it; the phone takes iOS over USB once and installs it.'
& $adbCommand -s $serial reboot
if (-not $hostSession.WaitForExit(1200000)) { $hostSession.Kill(); 'USB session stopped after 20 minutes' }
"USB session exit: $($hostSession.ExitCode)"
Get-Content "$usbOut.stdout.txt" -ErrorAction SilentlyContinue
