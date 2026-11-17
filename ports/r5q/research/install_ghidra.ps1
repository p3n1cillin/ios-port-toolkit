# Download the official Ghidra 12.2 release, verify its published SHA-256, and extract it.
$ErrorActionPreference = 'Stop'
$url = 'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.2_build/ghidra_12.2_PUBLIC_20261016.zip'
$expected = '2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67'
$dl = Join-Path $env:LOCALAPPDATA 'Programs\Ghidra-download'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\Ghidra'
New-Item -ItemType Directory -Force $dl, $dest | Out-Null
$zip = Join-Path $dl 'ghidra_12.2_PUBLIC_20261016.zip'
if (-not (Test-Path $zip)) {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}
$actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"sha256 $actual"
if ($actual -ne $expected) { throw "Ghidra download hash mismatch; not extracting" }
Expand-Archive -Path $zip -DestinationPath $dest -Force
$home12 = Get-ChildItem $dest -Directory | Where-Object Name -like 'ghidra_12.2*' | Select-Object -First 1
"GHIDRA_INSTALL_DIR=$($home12.FullName)"
Test-Path (Join-Path $home12.FullName 'support\analyzeHeadless.bat')
