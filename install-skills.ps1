# Make the toolkit's skills available to Claude Code in this project (copies toolkit/skills/* to .claude/skills/).
# Usage, from anywhere:  pwsh -File toolkit/install-skills.ps1 [-Target <project root>]
param([string]$Target = (Split-Path -Parent $PSScriptRoot))
$src = Join-Path $PSScriptRoot 'skills'
$dst = Join-Path $Target '.claude\skills'
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Get-ChildItem -Directory $src | ForEach-Object {
    $to = Join-Path $dst $_.Name
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Copy-Item -Force (Join-Path $_.FullName 'SKILL.md') (Join-Path $to 'SKILL.md')
    "installed skill $($_.Name) -> $to"
}
