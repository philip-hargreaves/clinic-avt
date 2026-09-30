# Every suite with nothing filtered out: engine, shell, client and contract, and the model
# fetch tool. Run before a PR.
#
#   .\tools\run-gates.ps1 [-Build] [-WithMicrophone] [-List]
#
# -List checks the plumbing without running anything. Needs staged weights and
# the Intel GPU. The model tests that read the PriMock consultation skip without it.
# CLINICAVT_PRIMOCK_MIXED and CLINICAVT_PRIMOCK_REFERENCE override its default paths.
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$WithMicrophone,
    [switch]$List
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vs = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
if (-not (Test-Path $ctest)) { $ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source }
if (-not $ctest) { throw 'ctest not found; install the CMake component or put ctest on PATH' }

# The suites load real models, so they need the GPU to themselves. Nothing is killed: a
# process stopped mid-GPU can wedge the driver, and a note host still here with its engine
# gone already has, which only a restart clears
$running = @(Get-Process 'ClinicAVT.App', 'clinicavt_engine', 'clinicavt_note_host' -ErrorAction SilentlyContinue)
if ($running) {
    $names = ($running | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', '
    throw "close the app and let its engine exit before the gates ($names still running)"
}

# ctest counts a skipped test as passed, so missing PriMock files are reported here.
# The defaults match engine\tests\support\primock.hpp
$primock = [ordered]@{
    CLINICAVT_PRIMOCK_MIXED     = 'C:\dev\intelliscribe\bench\transcription\mixed\day1_consultation01_mixed.wav'
    CLINICAVT_PRIMOCK_REFERENCE = 'C:\dev\intelliscribe\bench\transcription\references\day1_consultation01.json'
}
$missingPrimock = @($primock.GetEnumerator() | ForEach-Object {
    $path = [Environment]::GetEnvironmentVariable($_.Key)
    if (-not $path) { $path = $_.Value }
    if (-not (Test-Path $path)) { $path }
})
$primockWarning = "PriMock file(s) missing, so the model tests that read them will skip: $($missingPrimock -join ', ')"
if ($missingPrimock) { Write-Warning $primockWarning }

$results = [ordered]@{}
$started = Get-Date

function Get-Summary([string]$text) {
    if ($text -match '(\d+)% tests passed, (\d+) tests failed out of (\d+)') {
        return "$([int]$Matches[3] - [int]$Matches[2])/$($Matches[3]) passed"
    }
    if ($text -match 'Failed:\s+(\d+), Passed:\s+(\d+), Skipped:\s+(\d+), Total:\s+(\d+)') {
        return "$($Matches[2])/$($Matches[4]) passed, $($Matches[3]) skipped, $($Matches[1]) failed"
    }
    if ($text -match 'Total Tests: (\d+)') { return "$($Matches[1]) tests listed" }
    return 'no summary'
}

function Invoke-Gate([string]$name, [scriptblock]$body) {
    Write-Host "== $name" -ForegroundColor Cyan
    $at = Get-Date
    $output = & $body 2>&1 | Out-String
    $results[$name] = [pscustomobject]@{
        Exit    = $LASTEXITCODE
        Minutes = [math]::Round(((Get-Date) - $at).TotalMinutes, 1)
        Summary = Get-Summary $output
    }
    $output | Out-File (Join-Path $root "build\gates-$name.log")
}

if ($Build) {
    Invoke-Gate 'build' {
        Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
        Enter-VsDevShell -VsInstallPath $vs -DevCmdArguments '-arch=x64 -no_logo' -SkipAutomaticLocation | Out-Null
        Set-Location $root
        cmake --build --preset release
        dotnet build clinicavt.slnx -p:Platform=x64
    }
}

$exclude = if ($WithMicrophone) { @() } else { @('-LE', 'microphone') }
Invoke-Gate 'engine' {
    if ($List) { & $ctest --test-dir "$root\build\release" @exclude -N }
    else { & $ctest --test-dir "$root\build\release" @exclude --output-on-failure --timeout 1500 }
}

Invoke-Gate 'shell' {
    if ($List) { dotnet test "$root\app\ClinicAVT.App.Tests" --no-build -t }
    else { dotnet test "$root\app\ClinicAVT.App.Tests" --no-build }
}

Invoke-Gate 'contract' {
    if ($List) { dotnet test "$root\app\ClinicAVT.Client.Tests" -t }
    else { dotnet test "$root\app\ClinicAVT.Client.Tests" }
}

Invoke-Gate 'fetch' {
    if ($List) { dotnet test "$root\tools\ClinicAVT.FetchModels.Tests" -t }
    else { dotnet test "$root\tools\ClinicAVT.FetchModels.Tests" }
}

$left = @(Get-Process 'clinicavt_engine', 'clinicavt_note_host' -ErrorAction SilentlyContinue)
if ($left) {
    Write-Warning "still running after the suites: $(($left | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')"
}

Write-Host ''
$results.GetEnumerator() | ForEach-Object {
    '{0,-10} exit {1,-4} {2,6} min  {3}' -f $_.Key, $_.Value.Exit, $_.Value.Minutes, $_.Value.Summary
}
'{0:N0} minutes in total; logs in build\gates-*.log' -f ((Get-Date) - $started).TotalMinutes
if ($missingPrimock) { Write-Warning $primockWarning }
if (@($results.Values | Where-Object { $_.Exit -ne 0 })) { exit 1 }
