# Builds the unsigned MSIX package.
#
# MSBuild first packs the app, without the models, as a small MSIX. This script unpacks that,
# adds the payload from stage-payload.ps1, sets the version and publisher, and packs it again. Build the release preset first, because the engine comes from build\release.
# Signing is a separate step, described in tools\release\SIGNING.txt.
#
#   pack-msix.ps1 [-Out <folder>] [-Tiers constrained,default] [-Publisher <subject>]
#
#   -Out        Where the package, SHA256SUMS.txt and README.txt are written. The default is
#               build\msix. The unpacked files go there while packing, so the drive needs
#               twice the package size free.
#   -Tiers      The note models to include, by the tier in their manifest.json: constrained
#               (4B), default (9B) or accuracy (35B). The default is constrained,default.
#   -Publisher  The subject of the signing certificate. The default is the Publisher in
#               app\ClinicAVT.App\Package.appxmanifest. Pass a test certificate's subject to
#               make a package for local testing.
#
# The version comes from the VERSION file, so 0.1.0 is packed as 0.1.0.0.
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

# Use the newest installed Windows SDK, or else the SDK build tools package from NuGet
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

# Step 1: build the app as an MSIX. The tooling resolves the manifest, writes resources.pri and
# includes the engine and the self-contained Windows App SDK runtime.
& $msbuild $project /restore /v:m /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 `
    /p:SelfContained=true /p:WindowsPackageType=MSIX /p:GenerateAppxPackageOnBuild=true `
    /p:AppxPackageSigningEnabled=false /p:AppxBundle=Never "/p:AppxPackageDir=$appDir\"
if ($LASTEXITCODE -ne 0) { throw "app package build failed" }
$appMsix = @(Get-ChildItem $appDir -Recurse -Filter *.msix)
if ($appMsix.Count -ne 1) { throw "expected one app package under $appDir, found $($appMsix.Count)" }

# Step 2: unpack it and add the payload.
& $makeappx.FullName unpack /p $appMsix[0].FullName /d $layout /o
if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed" }
# makeappx writes these again when it packs
Remove-Item -LiteralPath (Join-Path $layout "AppxBlockMap.xml"), (Join-Path $layout "[Content_Types].xml"),
    (Join-Path $layout "AppxSignature.p7x"), (Join-Path $layout "AppxMetadata") -Recurse -ErrorAction SilentlyContinue

& (Join-Path $PSScriptRoot "stage-payload.ps1") -Dest $layout -Tiers $Tiers

# Step 3: set the version and publisher.
$manifestPath = Join-Path $layout "AppxManifest.xml"
$manifest = [xml]::new()
$manifest.PreserveWhitespace = $true
$manifest.Load($manifestPath)
$manifest.Package.Identity.Version = $version
$manifest.Package.Identity.Publisher = $Publisher
# The build replaces the Windows versions with ones from the target framework, so put back the
# ones in the source manifest
$family = $manifest.Package.Dependencies.TargetDeviceFamily
$family.MinVersion = $source.Dependencies.TargetDeviceFamily.MinVersion
$family.MaxVersionTested = $source.Dependencies.TargetDeviceFamily.MaxVersionTested
$manifest.Save($manifestPath)

# Step 4: pack, then write the checksum and the recipient's README.
$msix = Join-Path $Out "ClinicAVT_${version}_x64.msix"
$packClock = [Diagnostics.Stopwatch]::StartNew()
& $makeappx.FullName pack /d $layout /p $msix /h SHA256 /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed" }
$packClock.Stop()
Remove-Item $work -Recurse -Force

$hash = (Get-FileHash $msix -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content (Join-Path $Out "SHA256SUMS.txt") "$hash  $(Split-Path $msix -Leaf)" -Encoding ascii
Copy-Item (Join-Path $repo "tools\release\README-msix.txt") (Join-Path $Out "README.txt")

$size = (Get-Item $msix).Length / 1GB
Write-Host ("{0}: {1:N2} GB, tiers {2}. Pack {3:mm\:ss}, total {4:mm\:ss}" -f $msix, $size,
    ($Tiers -join ","), $packClock.Elapsed, $clock.Elapsed)
