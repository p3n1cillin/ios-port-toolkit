# One authorized Test 18 hardware run (NOT run yet; needs the user's explicit OK): automatic capture/restore, then a 12 h restore watcher
# only if Android did not return in time. Never retries or starts another test.
$ErrorActionPreference = 'Continue'
if ($env:PLATFORM_TOOLS) { $env:Path = "$env:PLATFORM_TOOLS;$env:Path" }   # fastboot/adb: on PATH, or in PLATFORM_TOOLS
$py = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\python.exe'
$work = $PSScriptRoot
$outputs = if ($env:R5Q_OUTPUTS) { $env:R5Q_OUTPUTS } else { Join-Path $PSScriptRoot 'outputs' }   # test bundles
$bundle = Join-Path $outputs 'test18'
& $py -I "$work\run_native_test.py" $bundle --operator Manage-Test18.ps1 --timeout 300 `
    --expect '[TEST17] BootShim entry' --expect '[TEST17] BootShim: entering SEC' `
    --expect 'TEST18 FIRMWARE RETURN PATH ONLY' --expect '[LAB] before MMU-off handoff' `
    --expect 'RB0' --expect '[LAB] context restored' --expect 'TEST18 RETURN PATH SELF TEST VERIFIED' `
    --expect '[TEST12] Resetting via PSHOLD'
$code = $LASTEXITCODE
"controller exit: $code"
if ($code -eq 2) {
    'Android did not return in time: starting the one-shot restore watcher (up to 12 h).'
    & $py -I "$work\restore_native_on_return.py" $bundle --operator Manage-Test18.ps1 --max-wait 43200
    "watcher exit: $LASTEXITCODE"
}
