# Builds the complete release folder: ClinicAVT.exe and the README at the top, and in app\ the
# app (self-contained, no runtime install), the engine and its runtimes, prompts, example
# recordings, guidelines and the model weights. Zip the folder and it is the whole release -
# extract, double-click ClinicAVT.exe, done. Build the release preset first: the launcher and
# the engine come from build\release.
#
# Model .cache dirs stay out: they are compiled blobs specific to this
# machine's GPU and driver, and every machine rebuilds its own on first use.
#
# -SmallNoteModel ships only the smallest note model, for a much smaller download.
#
#   stage-release.ps1 [-Out <folder>] [-SmallNoteModel]      default build\clinicavt
param(
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\clinicavt"),
    [switch]$SmallNoteModel
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$launcher = Join-Path $repo "build\release\launcher\ClinicAVT.exe"
if (-not (Test-Path $launcher)) { throw "no launcher at $launcher; run cmake --build --preset release" }

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null
$app = Join-Path $Out "app"

dotnet publish "$repo\app\ClinicAVT.App\ClinicAVT.App.csproj" -c Release -r win-x64 `
    --self-contained -p:Platform=x64 -o $app
if ($LASTEXITCODE -ne 0) { throw "app publish failed" }

# Anything SatelliteResourceLanguages does not catch: the recipient reads
# English, the culture folders are noise
Get-ChildItem $app -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]{2,12})*$' -and $_.Name -notmatch '^en' } |
    Remove-Item -Recurse -Force

# The engine is native C++; on a machine without the VC++ redistributable it
# will not start, so the CRT ships beside it
$crt = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse `
        -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -eq $crt) { throw "no VC++ CRT redist found beside Visual Studio" }
Copy-Item (Join-Path $crt.FullName "*.dll") $app

# Beside the exe in the dev tree these are junctions; the package carries real copies
Copy-Item (Join-Path $repo "prompts") (Join-Path $app "prompts") -Recurse
# The example recordings and cases; the reflections there are evaluation fixtures
robocopy (Join-Path $repo "demo") (Join-Path $app "demo") /E /XD reflections /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "demo copy failed" }

# Guidelines a first run copies into the clinician's folder: the client's set without NICE, and
# the open-licence BSR ones. They stay out of git (rag/sources). The shingles guidance is a scan
# with no text to search
$guidelines = Join-Path $app "guidelines"
New-Item -ItemType Directory $guidelines | Out-Null
Get-ChildItem (Join-Path $repo "rag\sources\st-georges\folder") -Filter *.pdf |
    Where-Object { $_.Name -notmatch '^NICE ' -and $_.Name -notmatch '^Shingles ' } |
    Copy-Item -Destination $guidelines
Copy-Item (Join-Path $repo "rag\sources\bsr-open\*.pdf") $guidelines

$leftOut = @(".cache")
if ($SmallNoteModel) {
    $leftOut += Get-ChildItem (Join-Path $repo "models") -Directory | Where-Object {
        $manifest = Join-Path $_.FullName "manifest.json"
        (Test-Path $manifest) -and ((Get-Content $manifest -Raw | ConvertFrom-Json) |
            Where-Object { $_.task -eq "note" -and $_.tier -ne "constrained" })
    } | ForEach-Object { $_.FullName }
}
robocopy (Join-Path $repo "models") (Join-Path $app "models") /E /XD @leftOut /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "model copy failed" }
$global:LASTEXITCODE = 0

# The top level holds only what the recipient opens
Copy-Item $launcher $Out
Copy-Item (Join-Path $repo "tools\release\README.txt") $Out

Write-Host "Release folder at $Out - smoke it, then:  tar -a -c -f build\clinicavt.zip -C build clinicavt"
