# Runs clang-tidy (.clang-tidy at the repo root) over the engine, tests and tools included,
# from the dev preset's compile_commands.json. Fails on any finding.
#
#   cmake --preset dev
#   .\tools\run-clang-tidy.ps1 [-Tidy <clang-tidy.exe>] [-Filter <regex>] [-Jobs <n>]
#
# -Tidy defaults to the clang-tidy Visual Studio ships. CI pins its own version.
[CmdletBinding()]
param(
    [string]$Build = 'build\dev',
    [string]$Tidy = '',
    [string]$Filter = '',
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tidy = $Tidy
if (-not $tidy) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -prerelease -property installationPath
    $tidy = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
}
if (-not (Test-Path $tidy)) { throw "clang-tidy not found at $tidy; install the VS C++ Clang tools" }

$database = Join-Path $root $Build
$commands = Join-Path $database 'compile_commands.json'
if (-not (Test-Path $commands)) { throw "$commands not found; run cmake --preset dev first" }

$files = @(Get-Content $commands -Raw | ConvertFrom-Json |
    ForEach-Object { $_.file.Replace('\', '/') } |
    Where-Object { $_ -match '/engine/(domain|src|tools|tests)/' -and $_ -match $Filter } |
    Sort-Object -Unique)
if ($files.Count -eq 0) { throw "no engine files in $commands" }
Write-Host "clang-tidy $(& $tidy --version | Select-String 'version') on $($files.Count) files, $Jobs at a time"

# /WX- keeps clang's own compiler warnings as warnings, since the MSVC build enforces those
$output = $files | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    & $using:tidy -p $using:database --quiet --extra-arg=/WX- $_ 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { "clang-tidy failed on ${_}: exit $LASTEXITCODE" }
}
$failed = @($output | Where-Object { $_ -like 'clang-tidy failed on *' })

# A header's finding repeats in every file that includes it. Compile errors count too
$findings = @($output |
    Where-Object { $_ -match '^.+?:\d+:\d+: (warning|error): .+ \[[^\]]+\]$' } |
    Sort-Object -Unique)
$findings | ForEach-Object { Write-Host $_ }
$failed | ForEach-Object { Write-Host $_ }

if ($findings.Count -gt 0 -or $failed.Count -gt 0) {
    Write-Host ''
    $findings | ForEach-Object { [regex]::Match($_, '\[([^\],]+)[^\]]*\]$').Groups[1].Value } |
        Group-Object | Sort-Object Count -Descending |
        ForEach-Object { Write-Host ('{0,5}  {1}' -f $_.Count, $_.Name) }
    Write-Host "$($findings.Count) finding(s), $($failed.Count) file(s) not analysed"
    exit 1
}
Write-Host 'clang-tidy clean'
