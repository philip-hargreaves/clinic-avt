# Builds the zip release folder. Build the release preset first.
#
#   stage-release.ps1 [-Out <folder>] [-Tiers constrained,default]
#
# -Tiers picks note models by manifest tier: constrained (4B), default (9B), accuracy (35B).
param(
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\clinicavt"),
    [ValidateSet("constrained", "default", "accuracy")]
    [string[]]$Tiers = @("constrained", "default")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$launcher = Join-Path $repo "build\release\launcher\ClinicAVT.exe"
if (-not (Test-Path $launcher)) { throw "no launcher at $launcher; run cmake --build --preset release" }
$engine = Join-Path $repo "build\release\engine\clinicavt_engine.exe"
if (-not (Test-Path $engine)) { throw "no engine at $engine; run cmake --build --preset release" }

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null
$app = Join-Path $Out "app"

dotnet publish "$repo\app\ClinicAVT.App\ClinicAVT.App.csproj" -c Release -r win-x64 `
    --self-contained -p:Platform=x64 -o $app
if ($LASTEXITCODE -ne 0) { throw "app publish failed" }

& (Join-Path $PSScriptRoot "stage-payload.ps1") -Dest $app -Tiers $Tiers

Copy-Item $launcher $Out
Copy-Item (Join-Path $repo "tools\release\README.txt") $Out

$zip = "tar -a -c -f `"$Out.zip`" -C `"$(Split-Path $Out)`" $(Split-Path $Out -Leaf)"
Write-Host "Release folder at $Out. Try it, then zip it:  $zip"
