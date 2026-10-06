# Builds the unsigned MSIX. Build the release preset first.
#
#   pack-msix.ps1 [-Out <folder>] [-Tiers constrained,default] [-Publisher <subject>]
#
# -Tiers picks note models by manifest tier: constrained (4B), default (9B), accuracy (35B).
# -Publisher is the subject of the signing certificate. The manifest holds a placeholder.
param(
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\msix"),
    [ValidateSet("constrained", "default", "accuracy")]
    [string[]]$Tiers = @("constrained", "default"),
    [string]$Publisher
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$project = Join-Path $repo "app\ClinicAVT.App\ClinicAVT.App.csproj"

$source = ([xml](Get-Content (Join-Path $repo "app\ClinicAVT.App\Package.appxmanifest") -Raw)).Package
if (-not $Publisher) { $Publisher = $source.Identity.Publisher }

$release = (Get-Content (Join-Path $repo "VERSION") -Raw).Trim()
if ($release -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION holds '$release'; expected major.minor.patch" }
$version = "$release.0"

foreach ($exe in "clinicavt_engine.exe", "clinicavt_note_host.exe", "clinicavt_ingest_host.exe") {
    $path = Join-Path $repo "build\release\engine\$exe"
    if (-not (Test-Path $path)) { throw "no $exe at $path; run cmake --build --preset release" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = if (Test-Path $vswhere) {
    & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe" |
        Select-Object -First 1
}
if (-not $msbuild) { throw "no MSBuild found; install Visual Studio as README.md describes" }

$makeappx = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Directory.Parent.Name } -Descending) +
    @(Get-ChildItem "$env:USERPROFILE\.nuget\packages\microsoft.windows.sdk.buildtools\*\bin\*\x64\makeappx.exe" -ErrorAction SilentlyContinue) |
    Select-Object -First 1
if ($null -eq $makeappx) { throw "no makeappx.exe; install the Windows SDK" }
Write-Host "makeappx: $($makeappx.FullName)"

New-Item -ItemType Directory $Out -Force | Out-Null
$work = Join-Path $Out "work"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
$appDir = Join-Path $work "app"
$layout = Join-Path $work "layout"
$clock = [Diagnostics.Stopwatch]::StartNew()

# The app alone, as an MSIX
& $msbuild $project /restore /v:m /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 `
    /p:SelfContained=true /p:WindowsPackageType=MSIX /p:GenerateAppxPackageOnBuild=true `
    /p:AppxPackageSigningEnabled=false /p:AppxBundle=Never "/p:AppxPackageDir=$appDir\"
if ($LASTEXITCODE -ne 0) { throw "app package build failed" }
$appMsix = @(Get-ChildItem $appDir -Recurse -Filter *.msix)
if ($appMsix.Count -ne 1) { throw "expected one app package under $appDir, found $($appMsix.Count)" }

# Unpack it and add the payload
& $makeappx.FullName unpack /p $appMsix[0].FullName /d $layout /o
if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed" }
Remove-Item -LiteralPath (Join-Path $layout "AppxBlockMap.xml"), (Join-Path $layout "[Content_Types].xml"),
    (Join-Path $layout "AppxSignature.p7x"), (Join-Path $layout "AppxMetadata") -Recurse -ErrorAction SilentlyContinue

& (Join-Path $PSScriptRoot "stage-payload.ps1") -Dest $layout -Tiers $Tiers

$manifestPath = Join-Path $layout "AppxManifest.xml"
$manifest = [xml]::new()
$manifest.PreserveWhitespace = $true
$manifest.Load($manifestPath)
$manifest.Package.Identity.Version = $version
$manifest.Package.Identity.Publisher = $Publisher
if ($Publisher -match 'CN=([^,]+)') { $manifest.Package.Properties.PublisherDisplayName = $Matches[1].Trim() }
# Restore the Windows versions the build overwrites
$family = $manifest.Package.Dependencies.TargetDeviceFamily
$family.MinVersion = $source.Dependencies.TargetDeviceFamily.MinVersion
$family.MaxVersionTested = $source.Dependencies.TargetDeviceFamily.MaxVersionTested
$manifest.Save($manifestPath)

$msix = Join-Path $Out "ClinicAVT_${version}_x64.msix"
$packClock = [Diagnostics.Stopwatch]::StartNew()
& $makeappx.FullName pack /d $layout /p $msix /h SHA256 /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed" }
$packClock.Stop()

Copy-Item (Join-Path $repo "tools\release\README-msix.txt") (Join-Path $Out "README.txt")
# A scanner can still hold a file in the work folder, and the package is already written
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $work) { Write-Warning "could not remove $work" }

$size = (Get-Item $msix).Length / 1GB
Write-Host ("{0}: {1:N2} GB, tiers {2}. Pack {3:mm\:ss}, total {4:mm\:ss}" -f $msix, $size,
    ($Tiers -join ","), $packClock.Elapsed, $clock.Elapsed)
