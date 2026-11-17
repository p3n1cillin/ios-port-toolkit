[CmdletBinding()]
param([ValidateSet('Check','Stage','Start','Restore')][string]$Action = 'Check')
$ErrorActionPreference = 'Stop'
$serial = '<SERIAL>'
$remote = '/data/local/tmp/r5q-test1'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
$adbCommand = (Get-Command adb -ErrorAction Stop).Source
function Invoke-Adb([string[]]$Arguments) {
    $result = & $adbCommand -s $serial @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($result -join [Environment]::NewLine)" }
    return ($result -join "`n").Trim()
}
function Root([string]$Command) {
    if ($Command.Contains("'")) { throw 'Unexpected quote in root command' }
    return Invoke-Adb @('shell', "su -c '$Command'")
}
function Hash-Local([string]$Name, [string]$Expected) {
    $path = Join-Path $PSScriptRoot $Name
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Expected) {
        throw "Local checksum mismatch: $Name"
    }
    return $path
}
function Hash-Phone([string]$Path) {
    $result = Root "sha256sum $Path"
    if ($result -notmatch '^([0-9a-f]{64})\s') { throw "Unexpected phone checksum response for $Path" }
    return $Matches[1]
}
function Push-Verified([string]$Local, [string]$Target, [string]$Expected) {
    Write-Host (Invoke-Adb @('push', $Local, $Target))
    if ((Hash-Phone $Target) -ne $Expected) { throw "Staged checksum mismatch: $Target" }
}

$image = Hash-Local 'r5q-test1-recovery.img' $manifest.image.sha256
$backup = Hash-Local 'restore/recovery-original.img' $manifest.original_recovery.sha256
$helper = Hash-Local 'reboot-recovery' $manifest.reboot_helper.sha256
if ((Invoke-Adb @('get-state')) -ne 'device') { throw 'Android ADB is required' }
if ((Invoke-Adb @('shell','getprop','ro.product.model')) -ne 'SM-G770F') { throw 'Wrong phone model' }
if ((Root 'id') -notmatch 'uid=0\(root\)') { throw 'Magisk root is required' }
if ((Invoke-Adb @('shell','getprop','ro.boot.flash.locked')) -ne '0') { throw 'Bootloader is locked' }
if ((Root 'readlink -f /dev/block/by-name/recovery') -ne '/dev/block/sda21') { throw 'Unexpected recovery target' }
if ([int64](Root 'blockdev --getsize64 /dev/block/by-name/recovery') -ne $manifest.image.bytes) { throw 'Partition size differs' }
if ((Hash-Phone '/dev/block/by-name/boot') -ne $manifest.original_boot_sha256) { throw 'Android boot image changed: review before proceeding' }
if ((Hash-Phone '/dev/block/by-name/vbmeta') -ne $manifest.original_vbmeta_sha256) { throw 'VBMeta changed: review before proceeding' }
$battery = Invoke-Adb @('shell','dumpsys','battery')
if ($battery -notmatch '(?m)^\s*level:\s*(\d+)\s*$' -or [int]$Matches[1] -lt 30) { throw 'Battery must be at least 30%' }
$recoveryHash = Hash-Phone '/dev/block/by-name/recovery'
if ($Action -ne 'Restore' -and $recoveryHash -notin @($manifest.original_recovery.sha256, $manifest.image.sha256)) {
    throw 'Current recovery differs from both known images: investigate before testing'
}
Write-Host "Preflight passed for $serial (SM-G770F). Boot checksum matches the Android backup."
if ($Action -eq 'Check') { return }

Write-Host (Root "set -e; mkdir -p $remote; chown 2000:2000 $remote; chmod 700 $remote")
Push-Verified $backup "$remote/recovery-original.img" $manifest.original_recovery.sha256
if ($Action -eq 'Restore') {
    Write-Host (Root "set -e; dd if=$remote/recovery-original.img of=/dev/block/by-name/recovery bs=4096; sync")
    if ((Hash-Phone '/dev/block/by-name/recovery') -ne $manifest.original_recovery.sha256) { throw 'Restore checksum failed' }
    Write-Host 'Original recovery restored and verified. Android is still running.'
    return
}
Push-Verified $image "$remote/recovery.img" $manifest.image.sha256
Push-Verified $helper "$remote/reboot-recovery" $manifest.reboot_helper.sha256
Write-Host (Root "chmod 755 $remote/reboot-recovery")
if ($Action -eq 'Stage') {
    Write-Host 'Staged and verified all files. No firmware partition was written and no reboot was requested.'
    return
}

# Start is the explicit test action. It changes only the recovery partition.
$pending = (Root 'dd if=/dev/block/by-name/misc bs=32 count=1 2>/dev/null | od -An -tx1 -v') -replace '\s', ''
if ($pending -notmatch '^0{64}$') {
    throw 'A bootloader command is pending. Clear that through normal Android/recovery before testing.'
}
Write-Host (Root "set -e; dd if=$remote/recovery.img of=/dev/block/by-name/recovery bs=4096; sync")
if ((Hash-Phone '/dev/block/by-name/recovery') -ne $manifest.image.sha256) { throw 'Written recovery checksum failed; run Restore before rebooting' }
if ((Hash-Phone '/dev/block/by-name/boot') -ne $manifest.original_boot_sha256) { throw 'Unexpected Android boot change' }
Write-Host 'Starting Test 1. Watch the phone for R5Q NATIVE TEST 1 and TEST1 HANDOFF OK.'
Write-Host 'The diagnostic requests a reboot after 20 seconds. If it hangs, hold Volume Down + Side until black, then release both.'
& $adbCommand -s $serial shell "su -c 'exec $remote/reboot-recovery'"
Write-Host 'Recovery entry was requested. Use Restore once Android is available again.'
