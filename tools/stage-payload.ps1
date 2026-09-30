# Adds the release payload to a built app folder. It removes the non-English culture folders
# and copies in the VC++ runtime, prompts, example recordings, guidelines and model weights.
# stage-release.ps1 (zip) and pack-msix.ps1 (MSIX) both call it, so the two releases carry
# the same files.
#
# Compile caches are not shipped. Each computer builds its own under
# %LOCALAPPDATA%\ClinicAVT\cache the first time a model is used.
#
#   stage-payload.ps1 -Dest <folder> [-Tiers constrained,default]
#
#   -Dest   The built app folder to add to.
#   -Tiers  The note models to include, by the tier in their manifest.json: constrained (4B),
#           default (9B) or accuracy (35B). The default is constrained,default. The other
#           models are always included.
param(
    [Parameter(Mandatory)][string]$Dest,
    [ValidateSet("constrained", "default", "accuracy")]
    [string[]]$Tiers = @("constrained", "default")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

# Find the note models and the tier of each
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

# The app is English only, and SatelliteResourceLanguages leaves some culture folders behind
Get-ChildItem $Dest -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]{2,12})*$' -and $_.Name -notmatch '^en' } |
    Remove-Item -Recurse -Force

# The engine will not start without the VC++ runtime, so its DLLs ship beside it
$crt = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse `
        -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -eq $crt) { throw "no VC++ runtime found in the Visual Studio install" }
Copy-Item (Join-Path $crt.FullName "*.dll") $Dest

# In the dev tree these folders are junctions beside the exe. A release needs real copies
Copy-Item (Join-Path $repo "prompts") (Join-Path $Dest "prompts") -Recurse
# Example recordings and cases, and demo\reflections for the Seed data switch in Settings
robocopy (Join-Path $repo "demo") (Join-Path $Dest "demo") /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "demo copy failed" }

# Guidelines that the first run copies into the clinician's folder. These are the clinic's own
# set without the NICE documents, and the open-licence BSR ones. Neither set is in git. The
# shingles guidance is left out because it is a scan with no text
$guidelines = Join-Path $Dest "guidelines"
New-Item -ItemType Directory $guidelines | Out-Null
Get-ChildItem $st -Filter *.pdf |
    Where-Object { $_.Name -notmatch '^NICE ' -and $_.Name -notmatch '^Shingles ' } |
    Copy-Item -Destination $guidelines
Copy-Item (Join-Path $bsr "*.pdf") $guidelines

# Leave out the note models that were not asked for, and any .cache folder from an older build
$leftOut = @(".cache") + @($notes.Keys | Where-Object { $notes[$_] -notin $Tiers })
robocopy $models (Join-Path $Dest "models") /E /XD @leftOut /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "model copy failed" }
# robocopy returns 1 to 7 when it succeeds, which a caller would read as a failure
$global:LASTEXITCODE = 0
