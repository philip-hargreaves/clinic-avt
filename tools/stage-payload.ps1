# Adds the runtime, prompts, examples, guidelines and models to a built app folder. Used by
# stage-release.ps1 and pack-msix.ps1.
#
#   stage-payload.ps1 -Dest <folder> [-Tiers constrained,default]
#
# -Tiers picks note models by manifest tier: constrained (4B), default (9B), accuracy (35B).
param(
    [Parameter(Mandatory)][string]$Dest,
    [ValidateSet("constrained", "default", "accuracy")]
    [string[]]$Tiers = @("constrained", "default")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$models = Join-Path $repo "models"
$manifests = @(Get-ChildItem $models -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") })
if ($manifests.Count -eq 0) { throw "no models in $models; fetch them as README.md describes" }
$notes = @{}
foreach ($dir in $manifests) {
    $m = Get-Content (Join-Path $dir.FullName "manifest.json") -Raw | ConvertFrom-Json
    if ($m.task -eq "note") { $notes[$dir.FullName] = $m.tier }
}
foreach ($tier in $Tiers) {
    if ($notes.Values -notcontains $tier) { throw "no note model with tier '$tier' in $models; fetch it as README.md describes" }
}

$st = Join-Path $repo "rag\sources\st-georges\folder"
$bsr = Join-Path $repo "rag\sources\bsr-open"
foreach ($dir in $st, $bsr) {
    if (-not (Test-Path $dir)) { throw "no guidelines at $dir; they are not in git, so copy the PDFs there first" }
}

New-Item -ItemType Directory $Dest -Force | Out-Null

# The app is English only
Get-ChildItem $Dest -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]{2,12})*$' -and $_.Name -notmatch '^en' } |
    Remove-Item -Recurse -Force

# The engine needs the VC++ runtime beside it
$crt = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse `
        -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -eq $crt) { throw "no VC++ runtime found in the Visual Studio install" }
Copy-Item (Join-Path $crt.FullName "*.dll") $Dest

Copy-Item (Join-Path $repo "prompts") (Join-Path $Dest "prompts") -Recurse
robocopy (Join-Path $repo "demo") (Join-Path $Dest "demo") /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "demo copy failed" }

# No NICE documents, and no scans without text
$guidelines = Join-Path $Dest "guidelines"
New-Item -ItemType Directory $guidelines | Out-Null
Get-ChildItem $st -Filter *.pdf |
    Where-Object { $_.Name -notmatch '^NICE ' -and $_.Name -notmatch '^Shingles ' } |
    Copy-Item -Destination $guidelines
Copy-Item (Join-Path $bsr "*.pdf") $guidelines

$leftOut = @(".cache") + @($notes.Keys | Where-Object { $notes[$_] -notin $Tiers })
robocopy $models (Join-Path $Dest "models") /E /XD @leftOut /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "model copy failed" }
# robocopy returns 1 to 7 on success
$global:LASTEXITCODE = 0
