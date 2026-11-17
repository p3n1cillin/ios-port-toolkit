# Analyze hyp.mbn with Ghidra headless and decompile the uH TVM/KPP functions of interest.
$ErrorActionPreference = 'Stop'
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
$gh = if ($env:GHIDRA_HOME) { $env:GHIDRA_HOME } else { throw 'set GHIDRA_HOME to the Ghidra install folder' }
$base = $PSScriptRoot
$proj = Join-Path $base 'ghidra-proj'
$scripts = Join-Path $base 'ghidra-scripts'
$fw = Join-Path $base 'fw\hyp.mbn'
$out = Join-Path $base 'uh-decomp\uh_functions.c'
New-Item -ItemType Directory -Force $proj, (Split-Path $out) | Out-Null
# Addresses located by static objdump analysis (research/disasm_hyp*.sh).
$targets = @(
  '0x8576bf58',  # TVM trap handler (installed)
  '0x8576f6a8',  # checker called first inside the handler
  '0x8576ecf8',  # KPP init that enables TVM (caller of tvm-enable)
  '0x8576bf74',  # tvm-enable routine (sets HCR.TVM)
  '0x8576bf9c',  # tvm-disable routine (clears HCR.TVM)
  '0x85743cc0',  # uH boot path that sets the TVM/HypX flag 0x858d59c0
  '0x8574b444',  # stage-2 map/unmap dispatcher
  '0x8576cd7c'   # caller that reaches KPP init at EL1 fault time
)
& "$gh\support\analyzeHeadless.bat" $proj hyp -overwrite -import $fw -scriptPath $scripts `
  -postScript DecompileTargets.java $out @targets 2>&1 |
  Select-String -Pattern 'ERROR|Exception|DecompileTargets wrote|INFO  Packed|Analysis succeeded|REPORT' |
  Select-Object -Last 12
"exit=$LASTEXITCODE"
if (Test-Path $out) { "wrote $out  ($((Get-Item $out).Length) bytes)" }
