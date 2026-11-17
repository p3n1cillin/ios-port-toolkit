# Second pass: decompile the TVM mapping-commit worker, its gating condition, and the KPP-init flag users.
$ErrorActionPreference = 'Stop'
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
$gh = 'C:\Users\user\AppData\Local\Programs\Ghidra\ghidra_12.2_PUBLIC'
$base = 'C:\Users\user\source\repos\WIP\S10-Lite-project\work\ios-r5q\research'
$proj = Join-Path $base 'ghidra-proj'
$scripts = Join-Path $base 'ghidra-scripts'
$fw = Join-Path $base 'fw\hyp.mbn'
$out = Join-Path $base 'uh-decomp\uh_worker.c'
$targets = @(
  '0x8576f418',  # the mapping-commit worker (does the real work in the TVM fault)
  '0x8574d5fc',  # FUN_8574d5fc(3): the condition that selects the "cannot commit" error path
  '0x8574d9a4',  # the predicate the KPP init uses before enabling TVM (region-registered?)
  '0x85741698',  # critical-section enter used by the handler
  '0x857416b8'   # critical-section exit / commit used by the handler
)
& "$gh\support\analyzeHeadless.bat" $proj hyp -process hyp.mbn -scriptPath $scripts `
  -postScript DecompileTargets.java $out @targets 2>&1 |
  Select-String -Pattern 'ERROR|Exception|DecompileTargets wrote|Analysis succeeded|REPORT: Save' |
  Select-Object -Last 8
"exit=$LASTEXITCODE"
if (Test-Path $out) { "wrote $out ($((Get-Item $out).Length) bytes)" }
