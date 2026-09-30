# Builds the release folder: ClinicAVT.exe and README.txt at the top; in app\ the self-contained
# app, the engine and its runtimes, prompts, example recordings, guidelines and model weights.
# Zip the folder and it is the whole release. Build the release preset first: the launcher and
# the engine come from build\release.
#
# Compile caches live under %LOCALAPPDATA%\ClinicAVT\cache, and every machine builds its own on
# first use. A model .cache dir left by an older build stays out.
#
#   stage-release.ps1 [-Out <folder>] [-SmallNoteModel]      default build\clinicavt
#   -SmallNoteModel ships only the constrained-tier note model, for a much smaller download.
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

# Culture folders SatelliteResourceLanguages misses; the app is English only
Get-ChildItem $app -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]{2,12})*$' -and $_.Name -notmatch '^en' } |
    Remove-Item -Recurse -Force

# Without the VC++ redistributable the engine will not start, so the CRT ships beside it
$crt = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse `
        -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -eq $crt) { throw "no VC++ CRT redist found beside Visual Studio" }
Copy-Item (Join-Path $crt.FullName "*.dll") $app

# Beside the exe in the dev tree these are junctions; the package carries real copies
Copy-Item (Join-Path $repo "prompts") (Join-Path $app "prompts") -Recurse
# Example recordings and cases; demo\reflections only seeds the developer sample year
robocopy (Join-Path $repo "demo") (Join-Path $app "demo") /E /XD reflections /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "demo copy failed" }

# Guidelines a first run copies into the clinician's folder: the client's set without NICE, plus
# the open-licence BSR ones (both out of git). The shingles guidance is a scan with no text
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
